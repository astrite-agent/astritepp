// The literal-component transcript: focusable, collapsible segment components in
// a Container, driven off-screen via OnEvent + Render into a fixed Screen.
#include <doctest.h>

#include <string>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

#include "frontends/tui/transcript.hpp"
#include "frontends/tui/transcript_view.hpp"

using namespace astrite::tui;
using ftxui::Event;

namespace {
std::string render_to_string(const ftxui::Component& c, int w = 60, int h = 12) {
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(w), ftxui::Dimension::Fixed(h));
    ftxui::Render(screen, c->Render());
    return screen.ToString();
}
} // namespace

TEST_CASE("transcript view: sync builds one component per segment") {
    Transcript t;
    bool md = false, idx = false;
    TranscriptView v(t, &md, &idx);

    CHECK(v.count() == 0);
    t.user("hello");
    t.apply(astrite::runtime::AssistantSettled{"hi there", 0, 0, 0, false});
    CHECK(v.sync()); // grew
    CHECK(v.count() == 2);
    CHECK(v.component()->ChildCount() == 2);
    CHECK_FALSE(v.sync()); // no change second time

    std::string out = render_to_string(v.component());
    CHECK(out.find("hello") != std::string::npos);
    CHECK(out.find("hi there") != std::string::npos);
}

TEST_CASE("transcript view: Enter toggles the focused segment's collapse") {
    Transcript t;
    bool md = false, idx = false;
    TranscriptView v(t, &md, &idx);
    // a tool segment collapses to a one-line head
    t.apply(astrite::runtime::ToolStarted{"id", "fs_read", "{}"});
    t.apply(astrite::runtime::ToolFinished{"id", "fs_read", "line one\nline two\nline three"});
    v.sync();

    CHECK_FALSE(v.is_collapsed(0));
    // route Enter to the active child (selected_ defaults to 0)
    CHECK(v.component()->OnEvent(Event::Return));
    CHECK(v.is_collapsed(0));
    CHECK(v.component()->OnEvent(Event::Return));
    CHECK_FALSE(v.is_collapsed(0));
}

TEST_CASE("transcript view: collapse_all / expand_all and select clamp") {
    Transcript t;
    bool md = false, idx = false;
    TranscriptView v(t, &md, &idx);
    for (int i = 0; i < 4; ++i) {
        t.user("msg" + std::to_string(i));
    }
    v.sync();
    CHECK(v.count() == 4);

    v.collapse_all();
    CHECK(v.is_collapsed(0));
    CHECK(v.is_collapsed(3));
    v.expand_all();
    CHECK_FALSE(v.is_collapsed(2));

    v.select(99);
    CHECK(v.selected() == 3); // clamped to last
    v.select(-5);
    CHECK(v.selected() == 0);
}

TEST_CASE("transcript view: clearing the transcript rebuilds from scratch") {
    Transcript t;
    bool md = false, idx = false;
    TranscriptView v(t, &md, &idx);
    t.user("a");
    t.user("b");
    v.sync();
    v.collapse_all();
    CHECK(v.count() == 2);

    t.clear();
    CHECK(v.sync() == false); // n(0) < built(2) -> rebuild path, nothing added back
    CHECK(v.count() == 0);
    CHECK(v.component()->ChildCount() == 0);
    CHECK_FALSE(v.is_collapsed(0)); // collapse state reset

    t.user("fresh");
    CHECK(v.sync());
    CHECK(v.count() == 1);
}

TEST_CASE("transcript view: a proposal segment renders as kernel chrome") {
    Transcript t;
    bool md = false, idx = false;
    TranscriptView v(t, &md, &idx);
    astrite::runtime::ComponentProposed p;
    p.name = "shout";
    p.kind = "tool";
    p.risk = "none";
    p.caps = {};
    t.apply(p);
    v.sync();
    std::string out = render_to_string(v.component(), 60, 8);
    CHECK(out.find("shout") != std::string::npos);
    CHECK(out.find("proposal") != std::string::npos);
}
