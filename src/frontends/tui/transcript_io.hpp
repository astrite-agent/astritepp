#pragma once

// Persist the full typed transcript (every segment kind — user, assistant, tool,
// error, proposal, notice) as JSONL, and restore it on load so a reopened client
// shows the prior conversation, cards and all. Separate from the LLM message log
// (.astrite/sessions/current.jsonl, which the turn loop reads): that carries the
// model's exact message/tool-call structure, this carries the rendered UI.

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <glaze/glaze.hpp>

#include "frontends/tui/transcript.hpp"

namespace astrite::tui {

// On-disk shape of one segment (glaze-reflected). `type` names the kind.
struct SegmentWire {
    std::string type;
    std::string text;
    std::string tool_name;
    std::string tool_args;
    std::string tool_result;
    std::string risk;
    std::vector<std::string> caps;
    int resolution = 0; // proposal cards: 0 pending, 1 approved, 2 rejected
};

[[nodiscard]] inline const char* kind_name(Segment::Kind k) {
    switch (k) {
    case Segment::Kind::user:
        return "user";
    case Segment::Kind::assistant:
        return "assistant";
    case Segment::Kind::tool:
        return "tool";
    case Segment::Kind::error:
        return "error";
    case Segment::Kind::proposal:
        return "proposal";
    case Segment::Kind::notice:
        return "notice";
    }
    return "notice";
}

[[nodiscard]] inline std::optional<Segment::Kind> kind_of(std::string_view s) {
    if (s == "user") return Segment::Kind::user;
    if (s == "assistant") return Segment::Kind::assistant;
    if (s == "tool") return Segment::Kind::tool;
    if (s == "error") return Segment::Kind::error;
    if (s == "proposal") return Segment::Kind::proposal;
    if (s == "notice") return Segment::Kind::notice;
    return std::nullopt;
}

[[nodiscard]] inline SegmentWire to_wire(const Segment& s) {
    return SegmentWire{kind_name(s.kind), s.text, s.tool_name, s.tool_args,
                       s.tool_result,    s.risk, s.caps,      s.resolution};
}

[[nodiscard]] inline Segment from_wire(const SegmentWire& w) {
    Segment s;
    s.kind = kind_of(w.type).value_or(Segment::Kind::notice);
    s.text = w.text;
    s.tool_name = w.tool_name;
    s.tool_args = w.tool_args;
    s.tool_result = w.tool_result;
    s.risk = w.risk;
    s.caps = w.caps;
    s.resolution = w.resolution;
    s.done = true; // persisted segments are finalized
    return s;
}

// Rewrite the transcript file with the current segments (one JSON object/line).
// A streaming, not-yet-done segment is skipped — only settled state is persisted.
inline bool save_transcript(const std::filesystem::path& path,
                            const std::vector<Segment>& segments) {
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    for (const Segment& s : segments) {
        if (!s.done) {
            continue;
        }
        std::string line;
        if (glz::write_json(to_wire(s), line)) {
            return false;
        }
        out << line << '\n';
    }
    return out.good();
}

// Load persisted segments (skips blank/corrupt lines rather than failing the load).
[[nodiscard]] inline std::vector<Segment> load_transcript(const std::filesystem::path& path) {
    std::vector<Segment> segments;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return segments;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        SegmentWire w;
        if (!glz::read_json(w, line)) {
            segments.push_back(from_wire(w));
        }
    }
    return segments;
}

} // namespace astrite::tui
