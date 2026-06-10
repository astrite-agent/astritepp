// The self-re-exec fs helper, exercised end to end: run_fs_op forks+execs THIS
// test binary with the hidden __sbx subcommand (tests/main.cpp dispatches it),
// which applies the sandbox ratchet confined to the base and does the op. So
// these tests prove the real delivery path, and that Landlock is the suspenders
// even when the belt (path.resolve) is bypassed by calling run_fs_op directly.
#include <doctest.h>

#ifdef __linux__

#include <filesystem>
#include <fstream>
#include <string>

#include "sandbox/sandbox.hpp"

namespace fs = std::filesystem;
using astrite::sandbox::run_fs_op;

namespace {
fs::path make_ws(std::string suffix) {
    static int counter = 0;
    fs::path p = fs::temp_directory_path() /
                 ("astrite_fs_" + std::to_string(++counter) + "_" + suffix);
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}
void write_file(const fs::path& p, std::string_view content) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << content;
}
} // namespace

TEST_CASE("fs helper read: returns file content from inside the base") {
    fs::path ws = make_ws("read");
    write_file(ws / "f.txt", "the content\nline two");
    auto r = run_fs_op("read", ws, "f.txt");
    CHECK(r.ok);
    CHECK(r.output == "the content\nline two");
    fs::remove_all(ws);
}

TEST_CASE("fs helper read: a missing file fails without crashing") {
    fs::path ws = make_ws("read_missing");
    auto r = run_fs_op("read", ws, "nope.txt");
    CHECK_FALSE(r.ok);
    fs::remove_all(ws);
}

TEST_CASE("fs helper read: a directory is not a file") {
    fs::path ws = make_ws("read_dir");
    fs::create_directories(ws / "sub");
    auto r = run_fs_op("read", ws, "sub");
    CHECK_FALSE(r.ok);
    fs::remove_all(ws);
}

TEST_CASE("fs helper read: Landlock denies a `..` escape even to a real file") {
    fs::path ws = make_ws("escape");
    fs::path outside = ws.parent_path() / (ws.filename().string() + "_secret.txt");
    write_file(outside, "secret");
    // The belt (path.resolve) would reject this; here we bypass it to prove the
    // suspenders hold — the file exists and is readable to the parent, but the
    // confined child cannot reach it.
    auto r = run_fs_op("read", ws, "../" + outside.filename().string());
    CHECK_FALSE(r.ok);
    fs::remove_all(ws);
    fs::remove(outside);
}

TEST_CASE("fs helper list: enumerates entries, marks directories, omits . and ..") {
    fs::path ws = make_ws("list");
    write_file(ws / "alpha.txt", "a");
    write_file(ws / "beta.txt", "b");
    fs::create_directories(ws / "subdir");
    auto r = run_fs_op("list", ws, ""); // empty rel == base root
    REQUIRE(r.ok);
    CHECK(r.output.find("alpha.txt") != std::string::npos);
    CHECK(r.output.find("beta.txt") != std::string::npos);
    CHECK(r.output.find("subdir/") != std::string::npos); // trailing slash marks a dir
    CHECK(r.output.find("..") == std::string::npos);
    fs::remove_all(ws);
}

TEST_CASE("fs helper list: a subdirectory path lists that subtree") {
    fs::path ws = make_ws("list_sub");
    write_file(ws / "sub" / "inner.txt", "x");
    auto r = run_fs_op("list", ws, "sub");
    REQUIRE(r.ok);
    CHECK(r.output.find("inner.txt") != std::string::npos);
    fs::remove_all(ws);
}

TEST_CASE("fs helper find: literal matches across files, with path:line:text") {
    fs::path ws = make_ws("find");
    write_file(ws / "a.txt", "alpha\nbeta\nalpha again");
    write_file(ws / "sub" / "b.txt", "gamma\nbeta here");
    auto r = run_fs_op("find", ws, "", "beta");
    REQUIRE(r.ok);
    CHECK(r.output.find("a.txt") != std::string::npos);
    CHECK(r.output.find("b.txt") != std::string::npos);
    CHECK(r.output.find("beta") != std::string::npos);
    // 'alpha' lines must not appear in a search for 'beta'
    CHECK(r.output.find("alpha") == std::string::npos);
    fs::remove_all(ws);
}

TEST_CASE("fs helper find: no matches succeeds with empty output") {
    fs::path ws = make_ws("find_none");
    write_file(ws / "a.txt", "nothing here");
    auto r = run_fs_op("find", ws, "", "zzz_absent");
    CHECK(r.ok);
    CHECK(r.output.empty());
    fs::remove_all(ws);
}

TEST_CASE("fs helper find: skips the version-control and astrite state dirs") {
    fs::path ws = make_ws("find_skip");
    write_file(ws / ".git" / "config", "needle");
    write_file(ws / "real.txt", "needle");
    auto r = run_fs_op("find", ws, "", "needle");
    REQUIRE(r.ok);
    CHECK(r.output.find("real.txt") != std::string::npos);
    CHECK(r.output.find(".git") == std::string::npos);
    fs::remove_all(ws);
}

#endif // __linux__
