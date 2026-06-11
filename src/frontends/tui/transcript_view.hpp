#pragma once

// A live FTXUI view of the transcript: a vertical Container of focusable,
// collapsible segment *components*, kept in sync with the pure Transcript fold.
// This is the literal-component transcript (M8) — FTXUI manages focus among the
// segments (arrows/click), Enter toggles the focused segment's collapse, and a
// segment can host interactive sub-widgets (e.g. a proposal's Approve/Reject).

#include <algorithm>
#include <cstddef>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include "frontends/tui/segment_render.hpp"
#include "frontends/tui/transcript.hpp"

namespace astrite::tui {

class TranscriptView {
public:
    TranscriptView(const Transcript& transcript, const bool* markdown, const bool* show_index)
        : transcript_(transcript), markdown_(markdown), show_index_(show_index),
          container_(ftxui::Container::Vertical({}, &selected_)) {}

    [[nodiscard]] ftxui::Component component() const { return container_; }
    [[nodiscard]] int selected() const { return selected_; }
    [[nodiscard]] int count() const { return built_; }

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
        auto render = [this, index](bool focused) -> Element {
            const auto& segs = transcript_.segments();
            if (index >= static_cast<int>(segs.size())) {
                return text("");
            }
            return render_segment(segs[static_cast<std::size_t>(index)], index, focused,
                                  is_collapsed(index), (show_index_ && *show_index_) || focused,
                                  markdown_ && *markdown_);
        };
        Component leaf = Renderer(render);
        return CatchEvent(std::move(leaf), [this, index](const Event& e) {
            if (e == Event::Return) {
                toggle(index);
                return true;
            }
            return false;
        });
    }

    const Transcript& transcript_;
    const bool* markdown_;
    const bool* show_index_;
    int selected_ = 0; // declared before container_: Container::Vertical captures &selected_
    int built_ = 0;
    std::vector<bool> collapsed_;
    ftxui::Component container_;
};

} // namespace astrite::tui
