// Off-screen render tests for the FTXUI DOM composition. These render real
// elements into a fixed Screen and inspect pixel attributes — the only way to
// catch composition bugs like the modal backdrop bleeding its `dim` flag onto
// the panel border (which is exactly what this guards against).
#include <doctest.h>

#include <string>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include "frontends/tui/markdown_render.hpp"
#include "frontends/tui/widgets.hpp"

using namespace ftxui;
using namespace astrite::tui;

namespace {
bool is_border(const std::string& c) {
    return c == "│" || c == "─" || c == "╭" || c == "╮" || c == "╰" || c == "╯" || c == "┌" ||
           c == "┐" || c == "└" || c == "┘";
}
} // namespace

TEST_CASE("modal border composes over a backdrop without inheriting dim") {
    Element content =
        render_markdown("## Heading\n\nSome text that runs out toward the right edge of the box.");
    Element base = vbox({text("backdrop row one"), text("backdrop row two"), filler()});
    Element root = modal_overlay(base, "help", content, 0.0f, "esc close");

    auto screen = Screen::Create(Dimension::Fixed(80), Dimension::Fixed(24));
    Render(screen, root);

    int border_cells = 0, dim_border_cells = 0;
    for (int y = 0; y < 24; ++y) {
        for (int x = 0; x < 80; ++x) {
            const Pixel& p = screen.PixelAt(x, y);
            if (is_border(p.character)) {
                ++border_cells;
                if (p.dim) {
                    ++dim_border_cells;
                }
            }
        }
    }
    CHECK(border_cells > 0);        // a border was actually drawn
    CHECK(dim_border_cells == 0);   // and none of it inherited the backdrop's dim
}

TEST_CASE("markdown headings render as bold cells") {
    Element e = render_markdown("# Title");
    auto screen = Screen::Create(Dimension::Fixed(20), Dimension::Fixed(2));
    Render(screen, e);
    bool any_bold = false;
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 20; ++x) {
            if (screen.PixelAt(x, y).bold) {
                any_bold = true;
            }
        }
    }
    CHECK(any_bold);
}

TEST_CASE("markdown wraps long paragraphs within the available width") {
    Element e = render_markdown("word word word word word word word word word word word word");
    auto screen = Screen::Create(Dimension::Fixed(20), Dimension::Fixed(8));
    Render(screen, e);
    // Nothing should be drawn in the last column beyond the width-20 wrap point
    // on more than one row (i.e. the text actually wrapped rather than clipping).
    int rows_with_text = 0;
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 20; ++x) {
            if (screen.PixelAt(x, y).character != " " && !screen.PixelAt(x, y).character.empty()) {
                ++rows_with_text;
                break;
            }
        }
    }
    CHECK(rows_with_text >= 2); // wrapped across multiple lines
}
