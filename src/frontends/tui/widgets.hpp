#pragma once

// Reusable FTXUI DOM builders. Everything composes through the DOM layer (no bare
// ANSI), so styling nests cleanly.

#include <string>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

namespace astrite::tui {

// A scrollable modal: a centered, bordered panel over a dimmed backdrop. The
// idiom (the one FTXUI's own Modal uses) is `panel | clear_under | center`:
// clear_under wipes the cells beneath the panel before it draws, so the dimmed
// backdrop can't bleed its `dim` pixel flag onto the panel's border/text. That
// lets us keep the real `base | dim` backdrop with a clean border.
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
    return dbox({base | dim, panel | clear_under | center});
}

} // namespace astrite::tui
