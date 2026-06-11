#pragma once

// Shared frontend wiring: component loading (embedded defaults + disk overrides)
// and the system prompt. Frontends are interchangeable shells over the same
// runtime; none of this grants authority.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "embedded_components.hpp" // generated at configure time
#include "kernel/primitive.hpp"
#include "runtime/trust_store.hpp"
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

// A disk component must be admitted by this predicate (name, source)->load? — the
// load-time trust gate. Empty means "admit all" (the permissive default the unit
// tests use); load_components installs a real gate.
using TrustFn = std::function<bool(const std::string& name, const std::string& source)>;

inline runtime::TrustPolicy trust_policy_from_env() {
    const char* p = std::getenv("ASTRITE_TRUST_POLICY");
    return (p != nullptr && std::string_view(p) == "strict") ? runtime::TrustPolicy::strict
                                                             : runtime::TrustPolicy::warn;
}

// Evaluate one disk source against its trust record + policy, log the outcome,
// and return whether it may load. The control-plane reservation guarantees only
// a human (via the consent flow) can ever have written the record.
inline bool admit_disk_component(const std::string& name, const std::string& source,
                                 const std::filesystem::path& trust_dir,
                                 runtime::TrustPolicy policy) {
    auto rec = runtime::read_trust_record(trust_dir, name);
    runtime::TrustState st = runtime::evaluate_trust(source, rec);
    if (st == runtime::TrustState::trusted) {
        return true;
    }
    const char* why = st == runtime::TrustState::untrusted_new ? "unapproved" : "changed since approval";
    if (runtime::should_load(st, policy)) {
        std::fprintf(stderr, "[trust] %s: %s — loaded (policy=warn; approve to pin it)\n",
                     name.c_str(), why);
        return true;
    }
    std::fprintf(stderr, "[trust] %s: %s — REFUSED (policy=strict)\n", name.c_str(), why);
    return false;
}

// Pick up NEW components (names not already present) from a directory of .json
// files — repo-local task verbs and (consent-approved) self-authored components.
// Same-name files are left to the embedded-override path; this only adds. Fail
// closed: a parse error or name/file mismatch rejects the whole load. A component
// the trust gate refuses is skipped (not an error).
inline std::expected<void, std::string> merge_dir_components(kernel::ComponentMap& map,
                                                             const std::filesystem::path& dir,
                                                             const TrustFn& admit = {}) {
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
        if (admit && !admit(stem, *source)) {
            continue; // trust gate refused it; admit_disk_component already logged
        }
        auto component = schema::load_component_jsonc(*source);
        if (!component.has_value()) {
            return std::unexpected(stem + ": " + component.error().message);
        }
        if (component->name != stem) {
            return std::unexpected("component file " + path.filename().string() +
                                   " declares mismatched name `" + component->name + "`");
        }
        map.emplace(component->name, std::move(*component));
    }
    return {};
}

inline std::expected<kernel::ComponentMap, std::string> load_components() {
    kernel::ComponentMap map;
    const std::filesystem::path trust_dir = ".astrite/trust";
    const runtime::TrustPolicy policy = trust_policy_from_env();
    const TrustFn admit = [trust_dir, policy](const std::string& name, const std::string& source) {
        return admit_disk_component(name, source, trust_dir, policy);
    };

    for (const auto& [name, embedded] : components::ALL) {
        std::string source(embedded); // embedded defaults are compiled-in, implicitly trusted
        const std::filesystem::path override_path =
            std::filesystem::path(".astrite/components") / (std::string(name) + ".json");
        if (auto disk = read_file(override_path)) {
            // An override replaces a trusted baseline, so it must clear the gate.
            // If it doesn't, fall back to the embedded default rather than fail.
            if (admit(std::string(name), *disk)) {
                source = std::move(*disk);
            } else {
                std::fprintf(stderr, "[override] %s: falling back to the embedded default\n",
                             std::string(name).c_str());
            }
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
    // Then repo-local components that aren't embedded at all (e.g. task verbs),
    // each subject to the same trust gate.
    if (auto added = merge_dir_components(map, ".astrite/components", admit); !added.has_value()) {
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
           "Orientation: call workspace_context once at the start to learn the project type and "
           "available verbs. Use diff_workspace to review and explain the changes you've made.\n"
           "Authoring your own components: draft inert component data, validate it with "
           "component_check, then submit it with component_propose — it is staged for the human to "
           "approve and is NOT active until then; you cannot write the .astrite control plane "
           "yourself. Declare the minimal caps a component truly needs, and give tool components a "
           "`parameters` JSON Schema so the model (you) knows what inputs to pass.\n"
           "Use the available tools when they help, and answer concisely.";
}

} // namespace astrite::frontends
