#include <doctest.h>

#include <string>

#include "frontends/tui/commands.hpp"
#include "frontends/tui/history.hpp"
#include "frontends/tui/view_state.hpp"

using namespace astrite::tui;

namespace {
bool has_command(const std::vector<const SlashCommand*>& v, std::string_view name) {
    for (const SlashCommand* c : v) {
        if (c->name == name) {
            return true;
        }
    }
    return false;
}
} // namespace

TEST_CASE("the command registry exposes the core local commands") {
    const auto& all = all_commands();
    CHECK(!all.empty());
    auto names = complete("/");
    CHECK(has_command(names, "/help"));
    CHECK(has_command(names, "/commands"));
    CHECK(has_command(names, "/quit"));
    CHECK(has_command(names, "/clear"));
}

TEST_CASE("completion is a prefix match on the leading token") {
    auto c = complete("/c");
    CHECK(has_command(c, "/commands"));
    CHECK(has_command(c, "/clear"));
    CHECK(has_command(c, "/collapse"));
    CHECK_FALSE(has_command(c, "/help"));

    CHECK(complete("/help").size() == 1);
    CHECK(complete("/zzz").empty());
}

TEST_CASE("completion only triggers for a bare slash token (no spaces, leading slash)") {
    CHECK(complete("help").empty());     // no leading slash
    CHECK(complete("/help arg").empty()); // has args — not a completion context
    CHECK(complete("").empty());
    CHECK(complete("/").size() == all_commands().size());
}

TEST_CASE("lookup resolves a command line's leading token") {
    CHECK(lookup("/help") != nullptr);
    CHECK(lookup("/help") == lookup("/help me"));   // first token only
    CHECK(lookup("/nope") == nullptr);
    CHECK(lookup("not a command") == nullptr);
}

TEST_CASE("the consent commands are registered and resolve with an argument") {
    CHECK(has_command(complete("/"), "/proposals"));
    CHECK(has_command(complete("/"), "/approve"));
    CHECK(has_command(complete("/"), "/reject"));
    // /approve takes a proposal name; lookup resolves the leading token only.
    CHECK(lookup("/approve shout") != nullptr);
    CHECK(lookup("/approve shout") == lookup("/approve"));
    CHECK(lookup("/reject shout") == lookup("/reject"));
    // prefix completion offers them
    CHECK(has_command(complete("/appr"), "/approve"));
    CHECK(has_command(complete("/prop"), "/proposals"));
}

TEST_CASE("history: empty history recall returns the current text") {
    History h;
    CHECK(h.up("x") == "x");
    CHECK(h.down("x") == "x");
}

TEST_CASE("history: up/down walk entries and preserve the draft") {
    History h;
    h.push("a");
    h.push("b");
    CHECK(h.up("draft") == "b"); // draft captured
    CHECK(h.up("b") == "a");
    CHECK(h.up("a") == "a"); // clamps at oldest
    CHECK(h.down("a") == "b");
    CHECK(h.down("b") == "draft"); // draft restored at the end
}

TEST_CASE("history: empties skipped, consecutive duplicates collapsed") {
    History h;
    h.push("");      // ignored
    h.push("a");
    h.push("a");     // duplicate of last, collapsed
    h.push("b");
    CHECK(h.items().size() == 2); // [a, b]
}

TEST_CASE("history: reverse search finds the most recent containing match") {
    History h;
    h.push("read foo");
    h.push("write bar");
    h.push("read baz");
    CHECK(h.search_backward("read", 2) == 2); // newest "read*" is index 2
    CHECK(h.search_backward("read", 1) == 0); // older search skips index 2
    CHECK(h.search_backward("bar", 2) == 1);
    CHECK(h.search_backward("zzz", 2) == -1);
    CHECK(h.search_backward("", 2) == -1); // empty query never matches
}

TEST_CASE("view state: focus toggles between input and transcript") {
    ViewState v;
    CHECK(v.focus() == Focus::input);
    v.toggle_focus(5);
    CHECK(v.focus() == Focus::transcript);
    CHECK(v.selected() == 4); // entering transcript selects the last segment
    v.toggle_focus(5);
    CHECK(v.focus() == Focus::input);
}

TEST_CASE("view state: entering an empty transcript selects nothing") {
    ViewState v;
    v.focus_transcript(0);
    CHECK(v.selected() == -1);
}

TEST_CASE("view state: selection moves and clamps") {
    ViewState v;
    v.focus_transcript(3); // selected = 2
    v.select_prev(3);
    CHECK(v.selected() == 1);
    v.select_prev(3);
    v.select_prev(3); // clamps at 0
    CHECK(v.selected() == 0);
    v.select_next(3);
    CHECK(v.selected() == 1);
    v.select_last(3);
    CHECK(v.selected() == 2);
    v.select_first(3);
    CHECK(v.selected() == 0);
}

TEST_CASE("view state: relative selection clamps (half-page jumps)") {
    ViewState v;
    v.focus_transcript(20); // selected = 19
    v.select_relative(-10, 20);
    CHECK(v.selected() == 9);
    v.select_relative(-100, 20);
    CHECK(v.selected() == 0);
    v.select_relative(10, 20);
    CHECK(v.selected() == 10);
    v.select_relative(100, 20);
    CHECK(v.selected() == 19);
}

TEST_CASE("view state: collapse toggles per segment and in bulk") {
    ViewState v;
    v.focus_transcript(4); // selected = 3
    CHECK_FALSE(v.is_collapsed(3));
    v.toggle_collapse_selected();
    CHECK(v.is_collapsed(3));
    v.toggle_collapse_selected();
    CHECK_FALSE(v.is_collapsed(3));

    v.collapse_all(4);
    CHECK(v.is_collapsed(0));
    CHECK(v.is_collapsed(3));
    v.expand_all();
    CHECK_FALSE(v.is_collapsed(0));
    CHECK_FALSE(v.is_collapsed(3));
}

TEST_CASE("view state: toggle by index + focus_segment (mouse-click path)") {
    ViewState v;
    v.toggle_collapse(2);
    CHECK(v.is_collapsed(2));
    v.toggle_collapse(2);
    CHECK_FALSE(v.is_collapsed(2));
    v.toggle_collapse(-1); // out of range: no-op, no crash
    CHECK_FALSE(v.is_collapsed(-1));

    v.focus_segment(3, 10);
    CHECK(v.focus() == Focus::transcript);
    CHECK(v.selected() == 3);
    v.focus_segment(99, 10); // clamps to last
    CHECK(v.selected() == 9);
}

TEST_CASE("view state: modal open/close, and nav is inert with no selection") {
    ViewState v;
    CHECK_FALSE(v.modal_open());
    v.open_modal(Modal::help);
    CHECK(v.modal_open());
    CHECK(v.modal() == Modal::help);
    v.close_modal();
    CHECK_FALSE(v.modal_open());
    CHECK(v.modal() == Modal::none);

    ViewState w; // input focus, no selection
    w.toggle_collapse_selected(); // must not crash or collapse anything
    CHECK_FALSE(w.is_collapsed(0));
}
