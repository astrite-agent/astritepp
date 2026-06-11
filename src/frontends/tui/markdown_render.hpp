#pragma once

// Thin renderer: markdown AST → FTXUI elements. Inline runs become per-word
// styled elements inside a wrapping flexbox, so styled text still wraps to the
// pane width. Kept separate from the (pure, tested) parser so the parser carries
// no FTXUI dependency.

#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/flexbox_config.hpp>
#include <ftxui/screen/color.hpp>

#include "frontends/tui/markdown.hpp"

namespace astrite::tui {

namespace detail {

// Split an inline span into word elements (preserving spacing) carrying its style.
inline void emit_words(ftxui::Elements& out, const md::Inline& span) {
    using namespace ftxui;
    auto style = [&](Element e) -> Element {
        switch (span.kind) {
        case md::Span::strong:
            return e | bold;
        case md::Span::emph:
            return e | italic;
        case md::Span::code:
            return e | inverted | color(Color::GrayLight);
        case md::Span::link:
            return e | underlined | color(Color::BlueLight);
        case md::Span::text:
            break;
        }
        return e;
    };
    const std::string& s = span.text;
    std::size_t i = 0;
    while (i < s.size()) {
        std::size_t sp = s.find(' ', i);
        std::size_t end = sp == std::string::npos ? s.size() : sp;
        if (end > i) {
            out.push_back(style(ftxui::text(s.substr(i, end - i))));
        }
        if (sp != std::string::npos) {
            out.push_back(ftxui::text(" ")); // a wrap point between words
            i = sp + 1;
        } else {
            break;
        }
    }
}

inline ftxui::Element inline_flow(const std::vector<md::Inline>& spans) {
    using namespace ftxui;
    Elements words;
    for (const md::Inline& sp : spans) {
        emit_words(words, sp);
    }
    if (words.empty()) {
        return text("");
    }
    FlexboxConfig cfg;
    cfg.wrap = FlexboxConfig::Wrap::Wrap;
    return flexbox(std::move(words), cfg);
}

} // namespace detail

inline ftxui::Element render_markdown(const std::string& source) {
    using namespace ftxui;
    md::Document doc = md::parse(source);
    Elements blocks;
    for (const md::Node& n : doc) {
        switch (n.kind) {
        case md::Block::heading: {
            Color c = n.level == 1 ? Color::CyanLight : Color::YellowLight;
            blocks.push_back(detail::inline_flow(n.inlines) | bold | color(c));
            break;
        }
        case md::Block::paragraph:
            blocks.push_back(detail::inline_flow(n.inlines));
            break;
        case md::Block::code: {
            Elements rows;
            std::size_t start = 0;
            const std::string& code = n.code;
            for (std::size_t i = 0; i <= code.size(); ++i) {
                if (i == code.size() || code[i] == '\n') {
                    rows.push_back(text("  " + code.substr(start, i - start)));
                    start = i + 1;
                }
            }
            blocks.push_back(vbox(std::move(rows)) | color(Color::GreenLight) |
                             bgcolor(Color::RGB(28, 28, 28)));
            break;
        }
        case md::Block::bullet:
            blocks.push_back(hbox({text("  • ") | color(Color::CyanLight),
                                   detail::inline_flow(n.inlines) | flex}));
            break;
        case md::Block::ordered:
            blocks.push_back(hbox({text("  " + n.marker + ". ") | color(Color::CyanLight),
                                   detail::inline_flow(n.inlines) | flex}));
            break;
        case md::Block::quote:
            blocks.push_back(hbox({text("│ ") | color(Color::GrayDark),
                                   detail::inline_flow(n.inlines) | dim | flex}));
            break;
        case md::Block::rule:
            blocks.push_back(separator());
            break;
        }
    }
    if (blocks.empty()) {
        return text("");
    }
    return vbox(std::move(blocks));
}

} // namespace astrite::tui
