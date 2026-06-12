#pragma once

// The persisted UI config: display preferences plus the collapse-by-default and
// reasoning toggles. This is *human-owned* — the frontend reads and writes it, so
// it sits beside the session under .astrite/config.json but is never touched by
// the model through effects. Missing or corrupt files load defaults: the config
// is a convenience, never a hard dependency for starting the client.

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <glaze/glaze.hpp>

namespace astrite::tui {

struct UiConfig {
    bool markdown = true;            // render assistant text as markdown
    bool show_index = false;         // always show segment indices (else only when focused)
    bool collapse_tool_calls = true; // tool segments start collapsed (they are noisy)
    bool collapse_reasoning = true;  // reasoning segments start collapsed (they are long)
    bool show_reasoning = false;     // request + surface provider reasoning traces at all
};

// Write the config as pretty JSON. Returns false on any IO error.
[[nodiscard]] inline bool save_config(const std::filesystem::path& path, const UiConfig& cfg) {
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::string json;
    if (glz::write<glz::opts{.prettify = true}>(cfg, json)) {
        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out << json;
    return out.good();
}

// Load the config, falling back to defaults for a missing/corrupt file. Keys
// absent from the file keep their default (we read into a default-constructed
// value), so the file can grow new fields without breaking older configs.
[[nodiscard]] inline UiConfig load_config(const std::filesystem::path& path) {
    UiConfig cfg;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return cfg;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string json = ss.str();
    if (json.empty()) {
        return cfg;
    }
    UiConfig parsed;
    if (!glz::read<glz::opts{.error_on_unknown_keys = false}>(parsed, json)) {
        return parsed; // success
    }
    return cfg; // corrupt -> defaults
}

} // namespace astrite::tui
