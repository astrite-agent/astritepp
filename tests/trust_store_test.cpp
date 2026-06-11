// Trust records + the pure trust decision that gates disk-sourced components.
#include <doctest.h>

#include <filesystem>
#include <optional>
#include <string>

#include "runtime/trust_store.hpp"

namespace fs = std::filesystem;
using namespace astrite::runtime;

namespace {
fs::path make_dir(std::string suffix) {
    static int counter = 0;
    fs::path p = fs::temp_directory_path() /
                 ("astrite_trust_" + std::to_string(++counter) + "_" + suffix);
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}
} // namespace

TEST_CASE("evaluate_trust: no record is new; matching hash is trusted; mismatch is changed") {
    const std::string source = R"({"component":"x","kind":"tool"})";
    CHECK(evaluate_trust(source, std::nullopt) == TrustState::untrusted_new);

    auto rec = make_trust_record("x", source, {"fs.read"}, "read", "2026-06-11T00:00:00Z");
    CHECK(evaluate_trust(source, rec) == TrustState::trusted);
    CHECK(evaluate_trust(source + " ", rec) == TrustState::untrusted_changed);
}

TEST_CASE("make_trust_record pins the source hash") {
    const std::string source = "hello";
    auto rec = make_trust_record("x", source, {}, "none", "t");
    CHECK(rec.sha256 == astrite::util::sha256_hex(source));
    CHECK(rec.name == "x");
}

TEST_CASE("should_load: strict admits only trusted; warn admits anything") {
    CHECK(should_load(TrustState::trusted, TrustPolicy::strict));
    CHECK_FALSE(should_load(TrustState::untrusted_new, TrustPolicy::strict));
    CHECK_FALSE(should_load(TrustState::untrusted_changed, TrustPolicy::strict));

    CHECK(should_load(TrustState::trusted, TrustPolicy::warn));
    CHECK(should_load(TrustState::untrusted_new, TrustPolicy::warn));
    CHECK(should_load(TrustState::untrusted_changed, TrustPolicy::warn));
}

TEST_CASE("trust records round-trip through disk") {
    fs::path dir = make_dir("rt");
    CHECK_FALSE(read_trust_record(dir, "build").has_value()); // none yet

    auto rec = make_trust_record("build", R"({"component":"build"})", {"task.run"}, "exec",
                                 "2026-06-11T01:00:00Z");
    REQUIRE(write_trust_record(dir, rec));

    auto loaded = read_trust_record(dir, "build");
    REQUIRE(loaded.has_value());
    CHECK(loaded->name == "build");
    CHECK(loaded->sha256 == rec.sha256);
    CHECK(loaded->caps == std::vector<std::string>{"task.run"});
    CHECK(loaded->risk == "exec");
    fs::remove_all(dir);
}

TEST_CASE("a written record makes its exact source trusted, a changed source not") {
    fs::path dir = make_dir("gate");
    const std::string approved = R"({"component":"lint","kind":"task"})";
    REQUIRE(write_trust_record(dir, make_trust_record("lint", approved, {"task.run"}, "exec", "t")));

    auto rec = read_trust_record(dir, "lint");
    CHECK(evaluate_trust(approved, rec) == TrustState::trusted);
    CHECK(evaluate_trust(approved + "\n", rec) == TrustState::untrusted_changed);
    fs::remove_all(dir);
}
