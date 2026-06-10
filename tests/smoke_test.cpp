// Toolchain smoke: the vendored libs compile under our flag set and do something trivial.
#include <doctest.h>

#define RYML_SINGLE_HDR_DEFINE_NOW
#include <rapidyaml.hpp>

#include <glaze/json.hpp>

TEST_CASE("rapidyaml parses a scalar map") {
    ryml::Tree tree = ryml::parse_in_arena("component: smoke\nkind: tool\n");
    ryml::ConstNodeRef root = tree.rootref();
    CHECK(root.is_map());
    CHECK(root["component"].val() == "smoke");
}

TEST_CASE("glaze round-trips generic json") {
    glz::generic json{};
    auto err = glz::read_json(json, R"({"a": 1, "b": [true, "x"]})");
    REQUIRE_FALSE(err);
    CHECK(json["a"].get<double>() == 1.0);
    auto out = glz::write_json(json);
    REQUIRE(out.has_value());
    CHECK(out->find("\"a\"") != std::string::npos);
}
