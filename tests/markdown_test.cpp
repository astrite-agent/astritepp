#include <doctest.h>

#include "frontends/tui/markdown.hpp"

using namespace astrite::tui::md;

TEST_CASE("inline: plain text is one text span") {
    auto s = parse_inline("just words");
    REQUIRE(s.size() == 1);
    CHECK(s[0].kind == Span::text);
    CHECK(s[0].text == "just words");
}

TEST_CASE("inline: bold, italic, and code spans") {
    auto s = parse_inline("a **b** c *d* `e`");
    REQUIRE(s.size() == 6);
    CHECK(s[0].kind == Span::text);
    CHECK(s[0].text == "a ");
    CHECK(s[1].kind == Span::strong);
    CHECK(s[1].text == "b");
    CHECK(s[2].text == " c ");
    CHECK(s[3].kind == Span::emph);
    CHECK(s[3].text == "d");
    CHECK(s[5].kind == Span::code);
    CHECK(s[5].text == "e");
}

TEST_CASE("inline: links carry text and url") {
    auto s = parse_inline("see [docs](https://x.y) now");
    REQUIRE(s.size() == 3);
    CHECK(s[1].kind == Span::link);
    CHECK(s[1].text == "docs");
    CHECK(s[1].url == "https://x.y");
}

TEST_CASE("inline: unmatched markers are literal (safe), no HTML interpretation") {
    auto a = parse_inline("2 * 3 = 6");
    REQUIRE(a.size() == 1);
    CHECK(a[0].kind == Span::text);
    CHECK(a[0].text == "2 * 3 = 6");

    auto b = parse_inline("<script>alert(1)</script>");
    REQUIRE(b.size() == 1);
    CHECK(b[0].kind == Span::text);
    CHECK(b[0].text == "<script>alert(1)</script>"); // rendered as literal text, never markup

    auto c = parse_inline("snake_case_word"); // underscores are literal, not emphasis
    REQUIRE(c.size() == 1);
    CHECK(c[0].text == "snake_case_word");
}

TEST_CASE("inline: code span content is never re-parsed") {
    auto s = parse_inline("`**not bold**`");
    REQUIRE(s.size() == 1);
    CHECK(s[0].kind == Span::code);
    CHECK(s[0].text == "**not bold**");
}

TEST_CASE("blocks: headings carry level and inline content") {
    auto d = parse("# Title\n## Sub **x**");
    REQUIRE(d.size() == 2);
    CHECK(d[0].kind == Block::heading);
    CHECK(d[0].level == 1);
    CHECK(d[0].inlines[0].text == "Title");
    CHECK(d[1].level == 2);
    CHECK(d[1].inlines.back().kind == Span::strong);
}

TEST_CASE("blocks: consecutive lines join into one paragraph; blank lines separate") {
    auto d = parse("one\ntwo\n\nthree");
    REQUIRE(d.size() == 2);
    CHECK(d[0].kind == Block::paragraph);
    CHECK(d[0].inlines[0].text == "one two");
    CHECK(d[1].inlines[0].text == "three");
}

TEST_CASE("blocks: fenced code is captured verbatim with its language") {
    auto d = parse("```cpp\nint x = 1;\n**still code**\n```\nafter");
    REQUIRE(d.size() == 2);
    CHECK(d[0].kind == Block::code);
    CHECK(d[0].lang == "cpp");
    CHECK(d[0].code == "int x = 1;\n**still code**");
    CHECK(d[1].kind == Block::paragraph);
    CHECK(d[1].inlines[0].text == "after");
}

TEST_CASE("blocks: bullet and ordered list items") {
    auto d = parse("- one\n- two\n\n1. first\n2. second");
    REQUIRE(d.size() == 4);
    CHECK(d[0].kind == Block::bullet);
    CHECK(d[0].inlines[0].text == "one");
    CHECK(d[2].kind == Block::ordered);
    CHECK(d[2].marker == "1");
    CHECK(d[3].marker == "2");
}

TEST_CASE("blocks: blockquote and horizontal rule") {
    auto d = parse("> quoted line\n\n---\n\ntext");
    REQUIRE(d.size() == 3);
    CHECK(d[0].kind == Block::quote);
    CHECK(d[0].inlines[0].text == "quoted line");
    CHECK(d[1].kind == Block::rule);
    CHECK(d[2].kind == Block::paragraph);
}

TEST_CASE("blocks: a list item right after a paragraph flushes the paragraph") {
    auto d = parse("intro line\n- item");
    REQUIRE(d.size() == 2);
    CHECK(d[0].kind == Block::paragraph);
    CHECK(d[1].kind == Block::bullet);
}

TEST_CASE("empty input yields no blocks") {
    CHECK(parse("").empty());
    CHECK(parse("\n\n").empty());
}
