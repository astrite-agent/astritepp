// The persisted UI config: render/display preferences plus the new collapse and
// reasoning toggles. Human-owned (frontend writes it), so it lives outside the
// model-unwritable control plane proper but is loaded at startup.
#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "frontends/tui/config.hpp"

namespace fs = std::filesystem;
using namespace astrite::tui;

namespace {
fs::path tmp(std::string tag) {
    static int n = 0;
    fs::path p = fs::temp_directory_path() / ("astrite_cfg_" + std::to_string(++n) + "_" + tag + ".json");
    fs::remove(p);
    return p;
}
} // namespace

TEST_CASE("config: defaults match the intended out-of-box behavior") {
    UiConfig cfg;
    CHECK(cfg.markdown == true);
    CHECK(cfg.show_index == false);
    CHECK(cfg.collapse_tool_calls == true);  // tool calls are noisy: fold by default
    CHECK(cfg.collapse_reasoning == true);    // reasoning is long: fold by default
    CHECK(cfg.show_reasoning == false);       // off => today's exact behavior
}

TEST_CASE("config: round-trips through save/load") {
    UiConfig cfg;
    cfg.markdown = false;
    cfg.show_index = true;
    cfg.collapse_tool_calls = false;
    cfg.collapse_reasoning = false;
    cfg.show_reasoning = true;

    fs::path path = tmp("roundtrip");
    REQUIRE(save_config(path, cfg));

    UiConfig loaded = load_config(path);
    CHECK(loaded.markdown == false);
    CHECK(loaded.show_index == true);
    CHECK(loaded.collapse_tool_calls == false);
    CHECK(loaded.collapse_reasoning == false);
    CHECK(loaded.show_reasoning == true);
}

TEST_CASE("config: missing file loads defaults") {
    fs::path path = tmp("missing"); // never created
    UiConfig loaded = load_config(path);
    CHECK(loaded.markdown == true);
    CHECK(loaded.collapse_tool_calls == true);
    CHECK(loaded.show_reasoning == false);
}

TEST_CASE("config: corrupt file loads defaults") {
    fs::path path = tmp("corrupt");
    std::ofstream(path) << "{ this is not json";
    UiConfig loaded = load_config(path);
    CHECK(loaded.markdown == true);
    CHECK(loaded.collapse_reasoning == true);
}

TEST_CASE("config: partial file keeps defaults for absent keys") {
    fs::path path = tmp("partial");
    std::ofstream(path) << R"({"show_reasoning":true})";
    UiConfig loaded = load_config(path);
    CHECK(loaded.show_reasoning == true);    // present key honored
    CHECK(loaded.markdown == true);          // absent key keeps default
    CHECK(loaded.collapse_tool_calls == true);
}
