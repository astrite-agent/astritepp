#pragma once

// Shared frontend wiring: component loading (embedded defaults + disk overrides)
// and the system prompt. Frontends are interchangeable shells over the same
// runtime; none of this grants authority.

#include <cstdio>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

#include "embedded_components.hpp" // generated at configure time
#include "kernel/primitive.hpp"
#include "schema/load.hpp"

namespace astrite::frontends {

inline std::optional<std::string> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return std::move(ss).str();
}

inline std::expected<kernel::ComponentMap, std::string> load_components() {
    kernel::ComponentMap map;
    for (const auto& [name, embedded] : components::ALL) {
        std::string source(embedded);
        const std::filesystem::path override_path =
            std::filesystem::path(".astrite/components") / (std::string(name) + ".json");
        if (auto disk = read_file(override_path)) {
            // NOTE(M9): overrides will be trust-gated against the embedded baseline
            // (capability/risk diffs + hash-pinned records). The sprint loads them
            // un-gated but never silently.
            std::fprintf(stderr, "[override] %s loaded from %s (trust gate arrives at M9)\n",
                         std::string(name).c_str(), override_path.string().c_str());
            source = std::move(*disk);
        }
        auto component = schema::load_component_jsonc(source);
        if (!component.has_value()) {
            return std::unexpected(std::string(name) + ": " + component.error().message);
        }
        if (component->name != name) {
            return std::unexpected("component file for `" + std::string(name) +
                                   "` declares mismatched name `" + component->name + "`");
        }
        map.emplace(component->name, std::move(*component));
    }
    return map;
}

inline std::string system_prompt() {
    return "You are astrite++, a minimal secure coding agent. Your behavior is declarative "
           "component data interpreted by a small kernel; effects are capability-gated. "
           "Workspace root: " +
           std::filesystem::current_path().string() +
           ". Use the available tools when they help, and answer concisely.";
}

} // namespace astrite::frontends
