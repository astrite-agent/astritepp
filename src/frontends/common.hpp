#pragma once

// Shared frontend wiring: component loading (embedded defaults + disk overrides)
// and the system prompt. Frontends are interchangeable shells over the same
// runtime; none of this grants authority.

#include <algorithm>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

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

// Pick up NEW components (names not already present) from a directory of .json
// files — repo-local task verbs today, trust-gated self-authored components at
// M9. Same-name files are left to the embedded-override path; this only adds.
// Fail closed: a parse error or name/file mismatch rejects the whole load.
inline std::expected<void, std::string> merge_dir_components(kernel::ComponentMap& map,
                                                             const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return {}; // no directory: nothing to add
    }
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end()); // deterministic load order
    for (const std::filesystem::path& path : files) {
        const std::string stem = path.stem().string();
        if (map.contains(stem)) {
            continue; // an embedded name + its override are handled elsewhere
        }
        auto source = read_file(path);
        if (!source.has_value()) {
            return std::unexpected("cannot read " + path.string());
        }
        auto component = schema::load_component_jsonc(*source);
        if (!component.has_value()) {
            return std::unexpected(stem + ": " + component.error().message);
        }
        if (component->name != stem) {
            return std::unexpected("component file " + path.filename().string() +
                                   " declares mismatched name `" + component->name + "`");
        }
        std::fprintf(stderr, "[component] %s loaded from %s (trust gate arrives at M9)\n",
                     stem.c_str(), path.string().c_str());
        map.emplace(component->name, std::move(*component));
    }
    return {};
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
    // Then repo-local components that aren't embedded at all (e.g. task verbs).
    if (auto added = merge_dir_components(map, ".astrite/components"); !added.has_value()) {
        return std::unexpected(added.error());
    }
    return map;
}

inline std::string system_prompt() {
    return "You are astrite++, a minimal secure coding agent. Your behavior is declarative "
           "component data interpreted by a small kernel; effects are capability-gated. "
           "Workspace root: " +
           std::filesystem::current_path().string() +
           ".\n"
           "File edits: prefer fs_edit (exact-match replacement) for existing files; use "
           "fs_write freely for new files, but only overwrite an existing file with fs_write "
           "when replacing the whole file is intentional.\n"
           "Execution: you may select declared task verbs (e.g. build, test) to run checks; you "
           "cannot compose shell commands — there is no shell, and a task's command is fixed. "
           "After changing code, run the relevant task to verify rather than assuming.\n"
           "Use the available tools when they help, and answer concisely.";
}

} // namespace astrite::frontends
