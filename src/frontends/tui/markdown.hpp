#pragma once

// A small, safe, zero-dependency markdown parser → AST. "Safe" means there is no
// HTML passthrough and no raw markup of any kind: anything not in the recognised
// subset becomes literal text, so untrusted model output can never inject styling
// it didn't earn. The parser is pure (no FTXUI), so it is fully unit-testable; a
// thin renderer (markdown_render.hpp) turns the AST into FTXUI elements, and the
// same AST will back the future `ui.markdown` constructor (DESIGN §10).
//
// Recognised: headings (#..######), paragraphs, fenced code (``` with language),
// bullet/ordered list items, blockquotes, horizontal rules; inline **strong**,
// *emph*, `code`, and [text](url). Underscores are literal (snake_case is safe).

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace astrite::tui::md {

enum class Span { text, strong, emph, code, link };
struct Inline {
    Span kind = Span::text;
    std::string text;
    std::string url; // links only
};

enum class Block { paragraph, heading, code, bullet, ordered, quote, rule };
struct Node {
    Block kind = Block::paragraph;
    int level = 0;          // heading level
    std::string lang;       // fenced-code language
    std::string code;       // fenced-code body
    std::string marker;     // ordered-list number
    std::vector<Inline> inlines;
};
using Document = std::vector<Node>;

namespace detail {

inline std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

inline std::size_t find_double(std::string_view s, char c, std::size_t from) {
    for (std::size_t k = from; k + 1 < s.size(); ++k) {
        if (s[k] == c && s[k + 1] == c) {
            return k;
        }
    }
    return std::string_view::npos;
}

} // namespace detail

inline std::vector<Inline> parse_inline(std::string_view s) {
    std::vector<Inline> out;
    std::string buf;
    auto flush = [&] {
        if (!buf.empty()) {
            out.push_back({Span::text, buf, {}});
            buf.clear();
        }
    };
    std::size_t i = 0;
    while (i < s.size()) {
        char c = s[i];
        if (c == '`') {
            std::size_t j = s.find('`', i + 1);
            if (j != std::string_view::npos) {
                flush();
                out.push_back({Span::code, std::string(s.substr(i + 1, j - i - 1)), {}});
                i = j + 1;
                continue;
            }
        } else if (c == '*' && i + 1 < s.size() && s[i + 1] == '*') {
            std::size_t j = detail::find_double(s, '*', i + 2);
            if (j != std::string_view::npos && j > i + 2) {
                flush();
                out.push_back({Span::strong, std::string(s.substr(i + 2, j - i - 2)), {}});
                i = j + 2;
                continue;
            }
        } else if (c == '*') {
            std::size_t j = s.find('*', i + 1);
            if (j != std::string_view::npos && j > i + 1) {
                flush();
                out.push_back({Span::emph, std::string(s.substr(i + 1, j - i - 1)), {}});
                i = j + 1;
                continue;
            }
        } else if (c == '[') {
            std::size_t close = s.find(']', i + 1);
            if (close != std::string_view::npos && close + 1 < s.size() && s[close + 1] == '(') {
                std::size_t url_end = s.find(')', close + 2);
                if (url_end != std::string_view::npos) {
                    flush();
                    out.push_back({Span::link, std::string(s.substr(i + 1, close - i - 1)),
                                   std::string(s.substr(close + 2, url_end - close - 2))});
                    i = url_end + 1;
                    continue;
                }
            }
        }
        buf += c;
        ++i;
    }
    flush();
    return out;
}

inline Document parse(std::string_view text) {
    Document doc;
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n') {
            lines.push_back(text.substr(start, i - start));
            start = i + 1;
        }
    }

    std::string para;
    auto flush_para = [&] {
        if (!para.empty()) {
            Node n;
            n.kind = Block::paragraph;
            n.inlines = parse_inline(para);
            doc.push_back(std::move(n));
            para.clear();
        }
    };

    auto is_rule = [](std::string_view t) {
        if (t.size() < 3) {
            return false;
        }
        char c = t.front();
        if (c != '-' && c != '*' && c != '_') {
            return false;
        }
        for (char ch : t) {
            if (ch != c) {
                return false;
            }
        }
        return true;
    };

    for (std::size_t li = 0; li < lines.size(); ++li) {
        std::string_view raw = lines[li];
        std::string_view t = detail::trim(raw);

        // Fenced code: capture verbatim until the closing fence.
        if (t.substr(0, 3) == "```") {
            flush_para();
            Node n;
            n.kind = Block::code;
            n.lang = std::string(detail::trim(t.substr(3)));
            std::string body;
            ++li;
            for (; li < lines.size(); ++li) {
                if (detail::trim(lines[li]).substr(0, 3) == "```") {
                    break;
                }
                if (!body.empty()) {
                    body += '\n';
                }
                body += std::string(lines[li]);
            }
            n.code = std::move(body);
            doc.push_back(std::move(n));
            continue;
        }

        if (t.empty()) {
            flush_para();
            continue;
        }
        if (is_rule(t)) {
            flush_para();
            doc.push_back(Node{Block::rule, 0, {}, {}, {}, {}});
            continue;
        }
        if (t.front() == '#') {
            std::size_t level = 0;
            while (level < t.size() && t[level] == '#') {
                ++level;
            }
            if (level <= 6 && level < t.size() && t[level] == ' ') {
                flush_para();
                Node n;
                n.kind = Block::heading;
                n.level = static_cast<int>(level);
                n.inlines = parse_inline(detail::trim(t.substr(level)));
                doc.push_back(std::move(n));
                continue;
            }
        }
        if (t.front() == '>') {
            flush_para();
            std::string_view body = detail::trim(t.substr(1));
            Node n;
            n.kind = Block::quote;
            n.inlines = parse_inline(body);
            doc.push_back(std::move(n));
            continue;
        }
        if ((t.front() == '-' || t.front() == '*' || t.front() == '+') && t.size() > 1 &&
            t[1] == ' ') {
            flush_para();
            Node n;
            n.kind = Block::bullet;
            n.inlines = parse_inline(detail::trim(t.substr(2)));
            doc.push_back(std::move(n));
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(t.front()))) {
            std::size_t k = 0;
            while (k < t.size() && std::isdigit(static_cast<unsigned char>(t[k]))) {
                ++k;
            }
            if (k < t.size() && (t[k] == '.' || t[k] == ')') && k + 1 < t.size() &&
                t[k + 1] == ' ') {
                flush_para();
                Node n;
                n.kind = Block::ordered;
                n.marker = std::string(t.substr(0, k));
                n.inlines = parse_inline(detail::trim(t.substr(k + 2)));
                doc.push_back(std::move(n));
                continue;
            }
        }

        // Plain paragraph line: join with a space.
        if (!para.empty()) {
            para += ' ';
        }
        para += std::string(t);
    }
    flush_para();
    return doc;
}

} // namespace astrite::tui::md
