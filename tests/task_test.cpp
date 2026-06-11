// run_task: the confined fork+exec path that runs a declared external program.
// Linux-gated like the other sandbox proofs — these fork real children, apply
// the ratchet, exec real tools, and observe capture/timeout/confinement as
// ground truth. /bin/sh is used purely as a convenient test subject; the real
// task components exec build/test tools directly (the "no shell" invariant is
// about the *model* never composing argv, not about which binaries can run).
#include <doctest.h>

#ifdef __linux__

#include <filesystem>
#include <fstream>
#include <string>

#include "sandbox/sandbox.hpp"

namespace fs = std::filesystem;
using astrite::sandbox::run_task;
using astrite::sandbox::system_read_exec_paths;
using astrite::sandbox::TaskSpec;

namespace {
fs::path make_ws(std::string suffix) {
    static int counter = 0;
    fs::path p = fs::temp_directory_path() /
                 ("astrite_task_" + std::to_string(++counter) + "_" + suffix);
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

std::string sh_binary() {
    for (const char* candidate : {"/bin/sh", "/usr/bin/sh"}) {
        if (fs::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

// A spec whose system bases let a dynamically-linked tool run, plus the
// workspace as the one read/write base.
TaskSpec sh_spec(const fs::path& ws, std::string script) {
    TaskSpec s;
    s.argv = {sh_binary(), "-c", std::move(script)};
    s.cwd = ws;
    s.read_paths = system_read_exec_paths();
    s.read_paths.push_back(ws);
    s.timeout_ms = 5000;
    return s;
}
} // namespace

TEST_CASE("run_task: captures stdout and a clean exit code") {
    fs::path ws = make_ws("ok");
    auto r = run_task(sh_spec(ws, "printf hello"));
    CHECK(r.spawned);
    CHECK(r.code == 0);
    CHECK(r.out == "hello");
    CHECK(r.err.empty());
    CHECK_FALSE(r.timed_out);
    fs::remove_all(ws);
}

TEST_CASE("run_task: captures stderr separately and a non-zero exit code") {
    fs::path ws = make_ws("err");
    auto r = run_task(sh_spec(ws, "printf oops 1>&2; exit 3"));
    CHECK(r.spawned);
    CHECK(r.code == 3);
    CHECK(r.out.empty());
    CHECK(r.err == "oops");
    fs::remove_all(ws);
}

TEST_CASE("run_task: a runaway child is killed at the timeout") {
    fs::path ws = make_ws("timeout");
    TaskSpec s = sh_spec(ws, "sleep 5");
    s.timeout_ms = 200;
    auto r = run_task(s);
    CHECK(r.spawned);
    CHECK(r.timed_out);
    CHECK(r.code != 0);
    fs::remove_all(ws);
}

TEST_CASE("run_task: reads outside the granted bases are denied (Landlock)") {
    fs::path ws = make_ws("read_confine");
    fs::path secret = ws.parent_path() / (ws.filename().string() + "_secret.txt");
    { std::ofstream(secret) << "top secret"; }
    auto r = run_task(sh_spec(ws, "cat '" + secret.string() + "'"));
    CHECK(r.spawned);
    CHECK(r.code != 0);            // cat could not open the file
    CHECK(r.out.find("secret") == std::string::npos);
    fs::remove(secret);
    fs::remove_all(ws);
}

TEST_CASE("run_task: writes outside the write bases are denied (Landlock)") {
    fs::path ws = make_ws("write_confine");
    fs::path outside = ws.parent_path() / (ws.filename().string() + "_out.txt");
    auto r = run_task(sh_spec(ws, "printf x > '" + outside.string() + "'"));
    CHECK(r.spawned);
    CHECK(r.code != 0);
    CHECK_FALSE(fs::exists(outside));
    fs::remove_all(ws);
}

TEST_CASE("run_task: a declared write base is writable") {
    fs::path ws = make_ws("write_ok");
    fs::path build = ws / "build";
    fs::create_directories(build);
    TaskSpec s = sh_spec(ws, "printf done > build/artifact.txt");
    s.write_paths = {build};
    auto r = run_task(s);
    CHECK(r.spawned);
    CHECK(r.code == 0);
    CHECK(fs::exists(build / "artifact.txt"));
    fs::remove_all(ws);
}

TEST_CASE("run_task: stdout is capped and the truncation is flagged") {
    fs::path ws = make_ws("cap");
    TaskSpec s = sh_spec(ws, "i=0; while [ $i -lt 500 ]; do printf X; i=$((i+1)); done");
    s.output_cap = 16;
    auto r = run_task(s);
    CHECK(r.spawned);
    CHECK(r.out.size() == 16);
    CHECK(r.out_truncated);
    fs::remove_all(ws);
}

TEST_CASE("run_task: a missing program is a spawn failure, not a crash") {
    fs::path ws = make_ws("nospawn");
    TaskSpec s;
    s.argv = {"/no/such/program_xyzzy"};
    s.cwd = ws;
    s.read_paths = system_read_exec_paths();
    s.read_paths.push_back(ws);
    auto r = run_task(s);
    CHECK_FALSE(r.spawned);
    CHECK_FALSE(r.spawn_error.empty());
    fs::remove_all(ws);
}

TEST_CASE("run_task: a bare program name is resolved against PATH") {
    fs::path ws = make_ws("path");
    TaskSpec s;
    s.argv = {"true"}; // not an absolute path
    s.cwd = ws;
    s.read_paths = system_read_exec_paths();
    s.read_paths.push_back(ws);
    auto r = run_task(s);
    CHECK(r.spawned);
    CHECK(r.code == 0);
    fs::remove_all(ws);
}

#endif // __linux__
