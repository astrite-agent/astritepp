#pragma once

// Reusable FTXUI DOM builders. Everything composes through the DOM layer (no bare
// ANSI), so styling nests cleanly.

#include <string>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

namespace astrite::tui {

// A scrollable modal: a centered, bordered, solid-background panel over the live
// view. The backdrop is darkened with `color()` rather than `dim` on purpose —
// `dim` is an additive pixel flag, and `dbox` draws the panel on top without
// clearing it, so `base | dim` would bleed the dim attribute onto the panel's
// own border and text. `color()` sets the foreground, which the panel's own
// cells overwrite, so the border composes cleanly.
inline ftxui::Element modal_overlay(ftxui::Element base, const std::string& title,
                                    ftxui::Element content, float scroll,
                                    const std::string& footer) {
    using namespace ftxui;
    Element panel =
        vbox({
            text(" " + title + " ") | bold | color(Color::White) | bgcolor(Color::Blue),
            separator(),
            content | focusPositionRelative(0.0f, scroll) | yframe | flex,
            separator(),
            text(footer) | dim | hcenter,
        }) |
        border | bgcolor(Color::Black) | size(WIDTH, LESS_THAN, 92) | size(HEIGHT, LESS_THAN, 32);
    return dbox({base | color(Color::GrayDark), panel | center});
}

} // namespace astrite::tui
