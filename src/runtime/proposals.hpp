#pragma once

// Proposal staging: kernel-owned scratch where a model-authored component waits
// for human review. The model reaches this only through the component.propose
// effect (which the KERNEL services) — it never writes .astrite/ itself. Staging
// is separate from the live components dir: a staged proposal is inert until a
// human accepts it (B4), which is what installs it and writes a trust record.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace astrite::runtime {

inline std::filesystem::path proposals_dir(const std::filesystem::path& workspace_root) {
    return workspace_root / ".astrite" / "proposals";
}

inline std::filesystem::path staged_proposal_path(const std::filesystem::path& workspace_root,
                                                  const std::string& name) {
    return proposals_dir(workspace_root) / (name + ".json");
}

[[nodiscard]] inline bool stage_proposal(const std::filesystem::path& workspace_root,
                                         const std::string& name, const std::string& source) {
    std::error_code ec;
    std::filesystem::create_directories(proposals_dir(workspace_root), ec);
    std::ofstream o(staged_proposal_path(workspace_root, name), std::ios::binary | std::ios::trunc);
    if (!o) {
        return false;
    }
    o << source;
    return o.good();
}

[[nodiscard]] inline std::optional<std::string>
read_staged_proposal(const std::filesystem::path& workspace_root, const std::string& name) {
    std::ifstream in(staged_proposal_path(workspace_root, name), std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

[[nodiscard]] inline std::vector<std::string>
list_staged_proposals(const std::filesystem::path& workspace_root) {
    std::vector<std::string> names;
    std::error_code ec;
    const auto dir = proposals_dir(workspace_root);
    if (!std::filesystem::is_directory(dir, ec)) {
        return names;
    }
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.is_regular_file() && e.path().extension() == ".json") {
            names.push_back(e.path().stem().string());
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

[[nodiscard]] inline bool discard_proposal(const std::filesystem::path& workspace_root,
                                           const std::string& name) {
    std::error_code ec;
    return std::filesystem::remove(staged_proposal_path(workspace_root, name), ec);
}

} // namespace astrite::runtime
