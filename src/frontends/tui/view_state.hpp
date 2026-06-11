#pragma once

// Interactive view state, kept separate from the transcript fold (which stays a
// pure event projection). This holds what the user is doing *to* the view —
// focus, the selected segment, which segments are collapsed, the active modal —
// as a pure, unit-testable model the FTXUI layer reads and mutates.

#include <algorithm>
#include <cstdint>
#include <set>

namespace astrite::tui {

enum class Focus : std::uint8_t { input, transcript };
enum class Modal : std::uint8_t { none, help, commands, status, config };

class ViewState {
public:
    [[nodiscard]] Focus focus() const { return focus_; }
    [[nodiscard]] Modal modal() const { return modal_; }
    [[nodiscard]] bool modal_open() const { return modal_ != Modal::none; }
    [[nodiscard]] int selected() const { return selected_; }

    void focus_input() { focus_ = Focus::input; }
    void focus_transcript(int count) {
        focus_ = Focus::transcript;
        if (count <= 0) {
            selected_ = -1;
        } else if (selected_ < 0 || selected_ >= count) {
            selected_ = count - 1; // default to the newest segment
        }
    }
    void toggle_focus(int count) {
        if (focus_ == Focus::input) {
            focus_transcript(count);
        } else {
            focus_input();
        }
    }

    void select_next(int count) { set_selected(selected_ + 1, count); }
    void select_prev(int count) { set_selected(selected_ - 1, count); }
    void select_first(int count) { set_selected(0, count); }
    void select_last(int count) { set_selected(count - 1, count); }
    void select_relative(int delta, int count) { set_selected(selected_ + delta, count); }

    [[nodiscard]] bool is_collapsed(int index) const { return collapsed_.contains(index); }
    void toggle_collapse_selected() {
        if (selected_ < 0) {
            return;
        }
        if (!collapsed_.insert(selected_).second) {
            collapsed_.erase(selected_);
        }
    }
    void collapse_all(int count) {
        for (int i = 0; i < count; ++i) {
            collapsed_.insert(i);
        }
    }
    void expand_all() { collapsed_.clear(); }

    void open_modal(Modal m) { modal_ = m; }
    void close_modal() { modal_ = Modal::none; }

private:
    void set_selected(int index, int count) {
        if (count <= 0) {
            selected_ = -1;
            return;
        }
        selected_ = std::clamp(index, 0, count - 1);
    }

    Focus focus_ = Focus::input;
    Modal modal_ = Modal::none;
    int selected_ = -1;
    std::set<int> collapsed_;
};

} // namespace astrite::tui
