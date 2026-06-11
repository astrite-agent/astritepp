#pragma once

// A live FTXUI view of the transcript: a vertical Container of focusable,
// collapsible segment *components*, kept in sync with the pure Transcript fold.
// This is the literal-component transcript (M8) — FTXUI manages focus among the
// segments (arrows/click), Enter toggles the focused segment's collapse, and a
// segment can host interactive sub-widgets (e.g. a proposal's Approve/Reject).

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/color.hpp>

#include "frontends/tui/segment_render.hpp"
#include "frontends/tui/transcript.hpp"

namespace astrite::tui {

// Invoked when the human clicks/activates a proposal card's button. `approve`
// false means reject. Wired by the frontend to the runtime consent methods.
using ProposalAction = std::function<void(const std::string& name, bool approve)>;

class TranscriptView {
public:
    TranscriptView(Transcript& transcript, const bool* markdown, const bool* show_index)
        : transcript_(transcript), markdown_(markdown), show_index_(show_index),
          container_(ftxui::Container::Vertical({}, &selected_)) {}

    [[nodiscard]] ftxui::Component component() const { return container_; }
    [[nodiscard]] int selected() const { return selected_; }
    [[nodiscard]] int count() const { return built_; }

    void set_proposal_action(ProposalAction action) { on_action_ = std::move(action); }

    // True if the focused segment hosts interactive sub-widgets (a proposal's
    // buttons) — the frontend then lets Enter/←/→ fall through to it instead of
    // treating them as segment nav/collapse.
    [[nodiscard]] bool selected_is_interactive() const {
        const auto& segs = transcript_.segments();
        return on_action_ && selected_ >= 0 && selected_ < static_cast<int>(segs.size()) &&
               segs[static_cast<std::size_t>(selected_)].kind == Segment::Kind::proposal;
    }

    void select(int i) {
        if (built_ > 0) {
            selected_ = std::clamp(i, 0, built_ - 1);
        }
    }

    [[nodiscard]] bool is_collapsed(int i) const {
        return i >= 0 && i < static_cast<int>(collapsed_.size()) &&
               collapsed_[static_cast<std::size_t>(i)];
    }
    void toggle(int i) {
        if (i >= 0 && i < static_cast<int>(collapsed_.size())) {
            collapsed_[static_cast<std::size_t>(i)] = !collapsed_[static_cast<std::size_t>(i)];
        }
    }
    void toggle_selected() { toggle(selected_); }
    void collapse_all() {
        for (std::size_t i = 0; i < collapsed_.size(); ++i) {
            collapsed_[i] = true;
        }
    }
    void expand_all() {
        for (std::size_t i = 0; i < collapsed_.size(); ++i) {
            collapsed_[i] = false;
        }
    }

    // Reconcile the child components with the current segment count. Called each
    // frame (cheap when unchanged). Returns true if a new segment was added.
    bool sync() {
        const int n = static_cast<int>(transcript_.segments().size());
        if (n < built_) { // transcript cleared/shrank — rebuild from scratch
            container_->DetachAllChildren();
            built_ = 0;
            collapsed_.clear();
            selected_ = 0;
        }
        bool grew = false;
        while (built_ < n) {
            collapsed_.push_back(false);
            container_->Add(make_segment(built_));
            ++built_;
            grew = true;
        }
        return grew;
    }

private:
    ftxui::Component make_segment(int index) {
        using namespace ftxui;
        const auto& segs = transcript_.segments();
        if (on_action_ && index < static_cast<int>(segs.size()) &&
            segs[static_cast<std::size_t>(index)].kind == Segment::Kind::proposal) {
            return make_proposal(index);
        }
        auto box = std::make_shared<Box>();
        auto render = [this, index, box](bool focused) -> Element {
            const auto& s = transcript_.segments();
            if (index >= static_cast<int>(s.size())) {
                return text("");
            }
            Element el = render_segment(s[static_cast<std::size_t>(index)], index, focused,
                                        is_collapsed(index), (show_index_ && *show_index_) || focused,
                                        markdown_ && *markdown_);
            if (focused) {
                el = el | focus; // FTXUI scrolls the frame to the focused segment
            }
            return el | reflect(*box);
        };
        Component leaf = Renderer(render);
        return CatchEvent(std::move(leaf), [this, index, box](Event e) {
            if (e == Event::Return) {
                toggle(index);
                return true;
            }
            // Click toggles collapse (the focusable Renderer already takes focus on
            // press); a drag that releases elsewhere won't match the box.
            if (e.is_mouse() && e.mouse().button == Mouse::Left &&
                e.mouse().motion == Mouse::Released && box->Contain(e.mouse().x, e.mouse().y)) {
                toggle(index);
                return true;
            }
            return false;
        });
    }

    // A proposal card with real, focusable Approve/Reject buttons — the showcase
    // for interactive segments. The buttons live in a Container so FTXUI routes
    // ←/→ and Enter to them when the segment is focused.
    ftxui::Component make_proposal(int index) {
        using namespace ftxui;
        const std::string name = transcript_.segments()[static_cast<std::size_t>(index)].tool_name;
        Component approve = Button(
            "Approve",
            [this, index, name] {
                transcript_.set_resolution(index, 1);
                if (on_action_) {
                    on_action_(name, true);
                }
            },
            ButtonOption::Ascii());
        Component reject = Button(
            "Reject",
            [this, index, name] {
                transcript_.set_resolution(index, 2);
                if (on_action_) {
                    on_action_(name, false);
                }
            },
            ButtonOption::Ascii());
        Component buttons = Container::Horizontal({approve, reject});
        auto render = [this, index, approve, reject, buttons]() -> Element {
            const auto& segs = transcript_.segments();
            if (index >= static_cast<int>(segs.size())) {
                return text("");
            }
            const Segment& s = segs[static_cast<std::size_t>(index)];
            Elements rows;
            rows.push_back(hbox({text("⛨ component proposal: ") | bold, text(s.tool_name) | bold}));
            std::string meta = "risk " + s.risk;
            if (!s.caps.empty()) {
                meta += " · caps ";
                for (std::size_t i = 0; i < s.caps.size(); ++i) {
                    meta += (i != 0 ? ", " : "") + s.caps[i];
                }
            }
            rows.push_back(text(meta));
            const int state = s.resolution;
            if (state == 1) {
                rows.push_back(text("✓ approved") | color(Color::GreenLight));
            } else if (state == 2) {
                rows.push_back(text("✗ rejected") | color(Color::GrayLight));
            } else {
                rows.push_back(hbox({approve->Render(), text("  "), reject->Render()}));
            }
            Element card = vbox(std::move(rows)) | borderDouble | color(Color::Magenta);
            if (buttons->Focused()) {
                card = card | focus; // scroll the frame to this card when active
            }
            return card;
        };
        return Renderer(buttons, render);
    }

    Transcript& transcript_;
    const bool* markdown_;
    const bool* show_index_;
    ProposalAction on_action_;
    int selected_ = 0; // declared before container_: Container::Vertical captures &selected_
    int built_ = 0;
    std::vector<bool> collapsed_;
    ftxui::Component container_;
};

} // namespace astrite::tui
