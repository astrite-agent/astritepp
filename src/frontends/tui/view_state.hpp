#pragma once

// What modal overlay (if any) is open. Focus, selection, and collapse used to
// live here too, but those are now FTXUI-managed by the literal-component
// transcript (transcript_view.hpp) — the kernel of pure UI state that remains is
// just the active modal.

#include <cstdint>

namespace astrite::tui {

enum class Modal : std::uint8_t { none, help, commands, status, config };

class ViewState {
public:
    [[nodiscard]] Modal modal() const { return modal_; }
    [[nodiscard]] bool modal_open() const { return modal_ != Modal::none; }
    void open_modal(Modal m) { modal_ = m; }
    void close_modal() { modal_ = Modal::none; }

private:
    Modal modal_ = Modal::none;
};

} // namespace astrite::tui
