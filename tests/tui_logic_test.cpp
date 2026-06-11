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

TEST_CASE("view state: modal open/close (focus/selection/collapse are now FTXUI-managed)") {
    ViewState v;
    CHECK_FALSE(v.modal_open());
    v.open_modal(Modal::help);
    CHECK(v.modal_open());
    CHECK(v.modal() == Modal::help);
    v.close_modal();
    CHECK_FALSE(v.modal_open());
    CHECK(v.modal() == Modal::none);
}
