#pragma once

// The control plane: the in-repo `.astrite/` tree holds components, the session
// log, and (M9) trust records — the data that defines the agent's behavior and
// authority. It must never be writable by a model-driven effect, or fs.write /
// fs.edit / task writes would walk straight around the trust boundary (a model
// could author its own capability-bearing component, forge the transcript, or
// later rewrite its own trust records). Reads are fine — components and the
// transcript are meant to be inspectable. This is the one reserved prefix; the
// kernel writes the control plane itself, only through reviewed flows.

#include <string_view>

namespace astrite {

inline constexpr std::string_view kControlPlaneDir = ".astrite";

// True if a workspace-relative path lies in the control plane (`.astrite` itself
// or anything beneath it). The caller has already lexically confined the path
// (no `..`, not absolute), so a simple prefix test is sound. `.astritefoo` is a
// different directory and is *not* reserved.
[[nodiscard]] inline bool is_control_plane_rel(std::string_view rel) {
    if (rel == kControlPlaneDir) {
        return true;
    }
    return rel.size() > kControlPlaneDir.size() && rel.starts_with(kControlPlaneDir) &&
           rel[kControlPlaneDir.size()] == '/';
}

} // namespace astrite
