// Repo-local component discovery: load_components picks up NEW components from
// .astrite/components/ (beyond embedded defaults + same-name overrides). This is
// the substrate for repo-specific task verbs now, and trust-gated self-authored
// components at M9. Tested via the extracted merge helper against a temp dir.
#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "frontends/common.hpp"
#include "schema/load.hpp"

namespace fs = std::filesystem;
using astrite::frontends::merge_dir_components;

TEST_CASE("every embedded component parses with a name matching its key") {
    for (const auto& [name, src] : astrite::components::ALL) {
        INFO("component: " << name);
        auto c = astrite::schema::load_component_jsonc(std::string(src));
        REQUIRE(c.has_value());
        CHECK(c->name == name);
    }
}

namespace {
fs::path make_dir(std::string suffix) {
    static int counter = 0;
    fs::path p = fs::temp_directory_path() /
                 ("astrite_comp_" + std::to_string(++counter) + "_" + suffix);
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}
void write_file(const fs::path& p, std::string_view content) {
    std::ofstream out(p, std::ios::binary);
    out << content;
}
constexpr std::string_view BUILD_TASK = R"({
  "component": "build",
  "kind": "task",
  "caps": ["task.run"],
  "task": {"argv": ["true"], "effect": "check"}
})";
} // namespace

TEST_CASE("merge_dir_components: a new component file is loaded and added") {
    fs::path dir = make_dir("new");
    write_file(dir / "build.json", BUILD_TASK);
    astrite::kernel::ComponentMap map;
    auto r = merge_dir_components(map, dir);
    REQUIRE(r.has_value());
    REQUIRE(map.contains("build"));
    CHECK(map.at("build").kind == "task");
    fs::remove_all(dir);
}

TEST_CASE("merge_dir_components: a name already present is left untouched") {
    fs::path dir = make_dir("dup");
    write_file(dir / "build.json", BUILD_TASK);
    astrite::kernel::ComponentMap map;
    astrite::schema::Component sentinel;
    sentinel.name = "build";
    sentinel.kind = "tool"; // distinct from the disk file's "task"
    map.emplace("build", sentinel);
    auto r = merge_dir_components(map, dir);
    REQUIRE(r.has_value());
    CHECK(map.at("build").kind == "tool"); // disk file did not overwrite it
    fs::remove_all(dir);
}

TEST_CASE("merge_dir_components: a file whose name mismatches its component is rejected") {
    fs::path dir = make_dir("mismatch");
    write_file(dir / "renamed.json", BUILD_TASK); // declares "build", not "renamed"
    astrite::kernel::ComponentMap map;
    auto r = merge_dir_components(map, dir);
    CHECK_FALSE(r.has_value());
    fs::remove_all(dir);
}

TEST_CASE("merge_dir_components: a malformed file is rejected, fail closed") {
    fs::path dir = make_dir("bad");
    write_file(dir / "broken.json", "{ not valid");
    astrite::kernel::ComponentMap map;
    CHECK_FALSE(merge_dir_components(map, dir).has_value());
    fs::remove_all(dir);
}

TEST_CASE("merge_dir_components: a missing directory is a no-op, not an error") {
    astrite::kernel::ComponentMap map;
    auto r = merge_dir_components(map, fs::temp_directory_path() / "astrite_no_such_dir_xyzzy");
    CHECK(r.has_value());
    CHECK(map.empty());
}
