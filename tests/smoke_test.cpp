// Toolchain smoke: the vendored libs compile under our flag set and do something trivial.
#include <doctest.h>

#include <glaze/json.hpp>
#include <glaze/yaml.hpp>

TEST_CASE("glaze round-trips generic json") {
    glz::generic json{};
    auto err = glz::read_json(json, R"({"a": 1, "b": [true, "x"]})");
    REQUIRE_FALSE(err);
    CHECK(json["a"].get<double>() == 1.0);
    auto out = glz::write_json(json);
    REQUIRE(out.has_value());
    CHECK(out->find("\"a\"") != std::string::npos);
}

TEST_CASE("glaze reads yaml into generic") {
    glz::generic g{};
    std::string y = "component: smoke\nkind: tool\n";
    auto err = glz::read_yaml(g, y);
    REQUIRE_FALSE(err);
    CHECK(g["component"].get<std::string>() == "smoke");
}
