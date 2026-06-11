// The control-plane reservation: .astrite/ (components, sessions, trust records)
// is never writable by a model-driven effect. Tested at every enforcement point
// — the pure prefix test, the sandbox helper's in-child re-check, and the task
// writable confinement.
#include <doctest.h>

#include "util/control_plane.hpp"

using astrite::is_control_plane_rel;

TEST_CASE("is_control_plane_rel: matches .astrite and its subtree only") {
    CHECK(is_control_plane_rel(".astrite"));
    CHECK(is_control_plane_rel(".astrite/components/x.json"));
    CHECK(is_control_plane_rel(".astrite/sessions/current.jsonl"));
    CHECK(is_control_plane_rel(".astrite/trust/x.json"));

    CHECK_FALSE(is_control_plane_rel("src/main.cpp"));
    CHECK_FALSE(is_control_plane_rel(".astritefoo"));      // different directory
    CHECK_FALSE(is_control_plane_rel(".astritefoo/x"));    // not a prefix segment
    CHECK_FALSE(is_control_plane_rel("build/.astrite/x")); // not at the root
    CHECK_FALSE(is_control_plane_rel(""));
}

#ifdef __linux__

#include <filesystem>
#include <fstream>
#include <string>

#include "sandbox/sandbox.hpp"

namespace fs = std::filesystem;

namespace {
fs::path make_ws(std::string suffix) {
    static int counter = 0;
    fs::path p = fs::temp_directory_path() /
                 ("astrite_cp_" + std::to_string(++counter) + "_" + suffix);
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}
std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
} // namespace

TEST_CASE("fs helper write: a control-plane path is refused in the confined child") {
    fs::path ws = make_ws("write");
    auto r = astrite::sandbox::fs_write(ws, ".astrite/components/evil.json", "{}");
    CHECK_FALSE(r.ok);
    CHECK_FALSE(fs::exists(ws / ".astrite" / "components" / "evil.json"));
    fs::remove_all(ws);
}

TEST_CASE("fs helper edit: a control-plane path is refused even if the file exists") {
    fs::path ws = make_ws("edit");
    fs::create_directories(ws / ".astrite" / "components");
    std::ofstream(ws / ".astrite" / "components" / "turn.json") << "original";
    auto r = astrite::sandbox::fs_edit(ws, ".astrite/components/turn.json", "original", "hacked", 1);
    CHECK_FALSE(r.ok);
    CHECK(slurp(ws / ".astrite" / "components" / "turn.json") == "original"); // untouched
    fs::remove_all(ws);
}

TEST_CASE("fs helper write: a normal workspace path still works") {
    fs::path ws = make_ws("ok");
    auto r = astrite::sandbox::fs_write(ws, "main.cpp", "int main(){}");
    CHECK(r.ok);
    CHECK(slurp(ws / "main.cpp") == "int main(){}");
    fs::remove_all(ws);
}

#endif // __linux__
