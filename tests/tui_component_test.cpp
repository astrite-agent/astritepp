// The literal-component transcript: focusable, collapsible segment components in
// a Container, driven off-screen via OnEvent + Render into a fixed Screen.
#include <doctest.h>

#include <string>

#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

#include "frontends/tui/config.hpp"
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
    UiConfig cfg;
    TranscriptView v(t, &cfg);

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
    UiConfig cfg;
    cfg.collapse_tool_calls = false; // start expanded so the toggle is observable
    TranscriptView v(t, &cfg);
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
    UiConfig cfg;
    TranscriptView v(t, &cfg);
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
    UiConfig cfg;
    TranscriptView v(t, &cfg);
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

TEST_CASE("transcript view: collapse-by-default follows config per segment kind") {
    Transcript t;
    UiConfig cfg; // defaults: collapse_tool_calls = collapse_reasoning = true
    TranscriptView v(t, &cfg);
    t.user("hi");                                                         // 0 user
    t.apply(astrite::runtime::AssistantSettled{"answer", 0, 0, 0, false}); // 1 assistant
    t.apply(astrite::runtime::ReasoningSettled{"because"});               // 2 reasoning
    t.apply(astrite::runtime::ToolStarted{"id", "fs_read", "{}"});
    t.apply(astrite::runtime::ToolFinished{"id", "fs_read", "data"}); // 3 tool
    v.sync();
    CHECK_FALSE(v.is_collapsed(0)); // user: expanded
    CHECK_FALSE(v.is_collapsed(1)); // assistant: expanded
    CHECK(v.is_collapsed(2));        // reasoning: collapsed by default
    CHECK(v.is_collapsed(3));        // tool: collapsed by default
}

TEST_CASE("transcript view: config can disable collapse-by-default") {
    Transcript t;
    UiConfig cfg;
    cfg.collapse_tool_calls = false;
    cfg.collapse_reasoning = false;
    TranscriptView v(t, &cfg);
    t.apply(astrite::runtime::ReasoningSettled{"r"});
    t.apply(astrite::runtime::ToolStarted{"id", "x", "{}"});
    v.sync();
    CHECK_FALSE(v.is_collapsed(0));
    CHECK_FALSE(v.is_collapsed(1));
}

TEST_CASE("a focused empty input shows a cursor; an unfocused one does not") {
    using namespace ftxui;
    // FTXUI's Input draws no cursor for empty content (input.cpp), so the empty
    // cursor is supplied via the transform hook — but only when FTXUI reports the
    // input as Focused(). This proves the cursor is driven by real FTXUI focus.
    std::string content;
    InputOption opt;
    opt.placeholder = " ";
    opt.multiline = true;
    opt.transform = [](InputState s) {
        if (s.is_placeholder && s.focused) {
            return s.element | focusCursorBarBlinking;
        }
        return s.element;
    };
    Component input = Input(&content, opt);

    // Focused: input is the active child of its container.
    Component focused_root = Container::Vertical({input});
    auto s1 = Screen::Create(Dimension::Fixed(20), Dimension::Fixed(1));
    Render(s1, focused_root->Render());
    CHECK(s1.cursor().shape == Screen::Cursor::Shape::BarBlinking);

    // Not focused: a focusable peer is active instead (selector 0), input is 1.
    int sel = 0;
    Component peer = Renderer([](bool) { return text("peer"); });
    Component two = Container::Vertical({peer, input}, &sel);
    auto s2 = Screen::Create(Dimension::Fixed(20), Dimension::Fixed(1));
    Render(s2, two->Render());
    CHECK(s2.cursor().shape == Screen::Cursor::Shape::Hidden);
}

TEST_CASE("transcript view: a proposal card's Approve/Reject buttons fire the action") {
    Transcript t;
    UiConfig cfg;
    TranscriptView v(t, &cfg);
    std::string name;
    bool approve = false;
    int calls = 0;
    v.set_proposal_action([&](const std::string& n, bool a) {
        name = n;
        approve = a;
        ++calls;
    });
    astrite::runtime::ComponentProposed p;
    p.name = "wave";
    p.kind = "tool";
    p.risk = "none";
    t.apply(p);
    v.sync();

    // selected_ defaults to 0 (the proposal). Enter routes through the segment's
    // button container to the focused Approve button.
    CHECK(v.component()->OnEvent(Event::Return));
    CHECK(calls == 1);
    CHECK(name == "wave");
    CHECK(approve == true);

    // After approving, the card shows its resolved state instead of buttons.
    std::string after = render_to_string(v.component(), 60, 8);
    CHECK(after.find("approved") != std::string::npos);
}

TEST_CASE("transcript view: clicking a segment toggles its collapse") {
    Transcript t;
    UiConfig cfg;
    cfg.collapse_tool_calls = false; // start expanded so the click-toggle is observable
    TranscriptView v(t, &cfg);
    t.apply(astrite::runtime::ToolStarted{"id", "fs_read", "{}"});
    t.apply(astrite::runtime::ToolFinished{"id", "fs_read", "a\nb\nc"});
    v.sync();
    render_to_string(v.component(), 60, 8); // fills the per-segment boxes (reflect)

    CHECK_FALSE(v.is_collapsed(0));
    ftxui::Mouse m;
    m.button = ftxui::Mouse::Left;
    m.motion = ftxui::Mouse::Released;
    m.x = 1;
    m.y = 0;
    v.component()->OnEvent(Event::Mouse("", m));
    CHECK(v.is_collapsed(0));
}

TEST_CASE("transcript view: a restored, already-approved proposal shows its outcome") {
    Transcript t;
    UiConfig cfg;
    Segment s; // as if loaded from disk
    s.kind = Segment::Kind::proposal;
    s.tool_name = "wave";
    s.risk = "none";
    s.resolution = 1; // approved
    s.done = true;
    t.restore({s});

    TranscriptView v(t, &cfg);
    v.set_proposal_action([](const std::string&, bool) {});
    v.sync();
    std::string out = render_to_string(v.component(), 60, 6);
    CHECK(out.find("approved") != std::string::npos);
    CHECK(out.find("Approve") == std::string::npos); // the button is gone
}

TEST_CASE("transcript view: a proposal segment renders as kernel chrome") {
    Transcript t;
    UiConfig cfg;
    TranscriptView v(t, &cfg);
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
