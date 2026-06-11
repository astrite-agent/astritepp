#pragma once

// Input history: Up/Down recall with draft preservation, plus backward substring
// search for reverse-i-search (Ctrl-R). Pure and unit-tested; the FTXUI layer
// owns the cursor and the search-mode UI.

#include <string>
#include <vector>

namespace astrite::tui {

class History {
public:
    // Record a submitted line. Empty lines and an immediate duplicate of the most
    // recent entry are ignored. Resets navigation to the end.
    void push(std::string entry) {
        if (entry.empty()) {
            return;
        }
        if (!items_.empty() && items_.back() == entry) {
            reset();
            return;
        }
        items_.push_back(std::move(entry));
        reset();
    }

    // Move to an older entry. `current` is the in-progress text, captured as the
    // draft the first time navigation leaves the end.
    std::string up(const std::string& current) {
        if (items_.empty()) {
            return current;
        }
        if (cursor_ == static_cast<int>(items_.size())) {
            draft_ = current;
        }
        if (cursor_ > 0) {
            --cursor_;
        }
        return items_[static_cast<std::size_t>(cursor_)];
    }

    // Move to a newer entry; past the newest restores the draft.
    std::string down(const std::string& current) {
        if (cursor_ == static_cast<int>(items_.size())) {
            return current;
        }
        ++cursor_;
        if (cursor_ == static_cast<int>(items_.size())) {
            return draft_;
        }
        return items_[static_cast<std::size_t>(cursor_)];
    }

    void reset() {
        cursor_ = static_cast<int>(items_.size());
        draft_.clear();
    }

    // Most-recent index <= `from` whose entry contains `query`; -1 if none (an
    // empty query never matches, matching reverse-i-search behaviour).
    [[nodiscard]] int search_backward(const std::string& query, int from) const {
        if (query.empty() || items_.empty()) {
            return -1;
        }
        if (from >= static_cast<int>(items_.size())) {
            from = static_cast<int>(items_.size()) - 1;
        }
        for (int i = from; i >= 0; --i) {
            if (items_[static_cast<std::size_t>(i)].find(query) != std::string::npos) {
                return i;
            }
        }
        return -1;
    }

    [[nodiscard]] const std::vector<std::string>& items() const { return items_; }

private:
    std::vector<std::string> items_;
    int cursor_ = 0;
    std::string draft_;
};

} // namespace astrite::tui
