#pragma once

// Pure transcript-segment rendering: Segment -> FTXUI Element. Extracted from the
// frontend so both the (DOM) renderer and the focusable segment *components*
// (transcript_view.hpp) draw from one source, and so it's testable off-screen.

#include <cstddef>
#include <string>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

#include "frontends/tui/markdown_render.hpp"
#include "frontends/tui/transcript.hpp"

namespace astrite::tui {

inline std::string first_line(const std::string& s) {
    std::size_t nl = s.find('\n');
    return nl == std::string::npos ? s : s.substr(0, nl);
}

inline std::string clip_lines(const std::string& s, std::size_t max_lines) {
    std::size_t lines = 0, pos = 0;
    while (pos != std::string::npos && lines < max_lines) {
        pos = s.find('\n', pos);
        if (pos != std::string::npos) {
            ++pos;
            ++lines;
        }
    }
    if (pos == std::string::npos) {
        return s;
    }
    std::size_t remaining = 0;
    for (std::size_t p = pos; p < s.size(); ++p) {
        if (s[p] == '\n') {
            ++remaining;
        }
    }
    return s.substr(0, pos) + "… (+" + std::to_string(remaining + 1) + " more lines)";
}

// The body of a segment (no selection accent / gutter). `collapsed` shows a terse
// form; `markdown` styles settled assistant text.
inline ftxui::Element segment_body(const Segment& s, bool collapsed, bool markdown) {
    using namespace ftxui;
    switch (s.kind) {
    case Segment::Kind::user:
        return hbox({text("› ") | bold, paragraph(s.text)}) | color(Color::CyanLight);
    case Segment::Kind::assistant:
        if (collapsed) {
            return paragraph(first_line(s.text) + " …") | color(Color::GrayLight);
        }
        if (!s.done) {
            return paragraph(s.text + " ▌");
        }
        return markdown ? render_markdown(s.text) : paragraph(s.text);
    case Segment::Kind::reasoning: {
        // The model's thinking trace — dim, visually quieter than the answer, and
        // collapsible (collapsed by default via config). Never markdown: it's a
        // raw trace, not authored prose.
        std::string head = std::string(collapsed ? "▸ " : "▾ ") + "🧠 reasoning";
        if (collapsed) {
            std::string peek = first_line(s.text);
            if (!peek.empty()) {
                head += " · " + peek;
            }
            return text(head + " …") | dim | color(Color::Magenta);
        }
        return vbox({text(head) | color(Color::Magenta),
                     paragraph(clip_lines(s.text, 24)) | dim}) |
               borderLight | color(Color::GrayDark);
    }
    case Segment::Kind::tool: {
        std::string head = std::string(collapsed ? "▸ " : "▾ ") + "⚙ " + s.tool_name;
        if (!s.tool_args.empty() && s.tool_args != "{}") {
            head += " " + first_line(s.tool_args);
        }
        if (collapsed) {
            return text(head) | color(Color::YellowLight);
        }
        Elements rows{text(head) | color(Color::YellowLight)};
        rows.push_back(s.done ? paragraph(clip_lines(s.tool_result, 14)) | color(Color::GrayLight)
                              : text("running…") | dim);
        return vbox(std::move(rows)) | borderLight | color(Color::GrayDark);
    }
    case Segment::Kind::error:
        return hbox({text("✗ ") | bold, paragraph(s.text)}) | color(Color::RedLight) | borderLight;
    case Segment::Kind::proposal: {
        // Kernel chrome — a distinct visual language from model content (double
        // border, magenta) so a proposal can't be mistaken for, or forged by, output.
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
        rows.push_back(text("pending review — approve with /approve " + s.tool_name) | dim);
        return vbox(std::move(rows)) | borderDouble | color(Color::Magenta);
    }
    case Segment::Kind::notice:
        return hbox({text("⛨ ") | bold, paragraph(s.text)}) | color(Color::Magenta);
    }
    return text("");
}

// A segment with its selection accent and optional index gutter (the DOM path).
inline ftxui::Element render_segment(const Segment& s, int idx, bool selected, bool collapsed,
                                     bool show_index, bool markdown) {
    using namespace ftxui;
    Element body = segment_body(s, collapsed, markdown);
    Element accent = selected ? (text("┃ ") | color(Color::Cyan)) : text("  ");
    Element row = hbox({accent, body | flex});
    if (show_index) {
        std::string label = (idx < 10 ? " " : "") + std::to_string(idx);
        Element gutter = text(label + " ") | (selected ? color(Color::Cyan) : dim);
        row = hbox({gutter, row | flex});
    }
    return row;
}

} // namespace astrite::tui
