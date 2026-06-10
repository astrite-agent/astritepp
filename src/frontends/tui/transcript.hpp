#pragma once

// Pure transcript state: folds the runtime's sealed event stream into renderable
// segments. No FTXUI types here — this is the testable half of the TUI, and the
// shape the M8 UiTree interpreter will consume.

#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "runtime/events.hpp"
#include "util/overloaded.hpp"

namespace astrite::tui {

struct Segment {
    enum class Kind { user, assistant, tool, error };
    Kind kind = Kind::user;
    std::string text;        // user input / accumulated assistant text / error message
    std::string tool_name;   // tool segments only
    std::string tool_args;
    std::string tool_result;
    bool done = false;
};

class Transcript {
public:
    void user(std::string text) {
        Segment s;
        s.kind = Segment::Kind::user;
        s.text = std::move(text);
        s.done = true;
        segments_.push_back(std::move(s));
    }

    void apply(const runtime::Event& event) {
        std::visit(
            overloaded{
                [&](const runtime::Started&) { turn_active_ = true; },
                [&](const runtime::AssistantDelta& d) { open_assistant().text += d.text; },
                [&](const runtime::AssistantSettled& s) {
                    if (Segment* open = open_assistant_or_null()) {
                        open->done = true;
                    } else if (!s.text.empty()) {
                        Segment seg;
                        seg.kind = Segment::Kind::assistant;
                        seg.text = s.text;
                        seg.done = true;
                        segments_.push_back(std::move(seg));
                    } // empty settle (pure tool-call turn): nothing to show
                },
                [&](const runtime::ToolStarted& t) {
                    Segment seg;
                    seg.kind = Segment::Kind::tool;
                    seg.tool_name = t.name;
                    seg.tool_args = t.arguments;
                    segments_.push_back(std::move(seg));
                },
                [&](const runtime::ToolFinished& t) {
                    for (auto it = segments_.rbegin(); it != segments_.rend(); ++it) {
                        if (it->kind == Segment::Kind::tool && !it->done &&
                            it->tool_name == t.name) {
                            it->tool_result = t.content;
                            it->done = true;
                            break;
                        }
                    }
                },
                [&](const runtime::TurnFinished&) {
                    if (Segment* open = open_assistant_or_null()) {
                        open->done = true;
                    }
                    turn_active_ = false;
                },
                [&](const runtime::TurnError& e) {
                    Segment seg;
                    seg.kind = Segment::Kind::error;
                    seg.text = e.message;
                    seg.done = true;
                    segments_.push_back(std::move(seg));
                    turn_active_ = false;
                },
            },
            event);
    }

    [[nodiscard]] const std::vector<Segment>& segments() const { return segments_; }
    [[nodiscard]] bool turn_active() const { return turn_active_; }

private:
    Segment* open_assistant_or_null() {
        if (!segments_.empty() && segments_.back().kind == Segment::Kind::assistant &&
            !segments_.back().done) {
            return &segments_.back();
        }
        return nullptr;
    }

    Segment& open_assistant() {
        if (Segment* open = open_assistant_or_null()) {
            return *open;
        }
        Segment seg;
        seg.kind = Segment::Kind::assistant;
        segments_.push_back(std::move(seg));
        return segments_.back();
    }

    std::vector<Segment> segments_;
    bool turn_active_ = false;
};

} // namespace astrite::tui
