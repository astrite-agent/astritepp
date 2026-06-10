#include <doctest.h>

#include "frontends/tui/transcript.hpp"

using astrite::tui::Segment;
using astrite::tui::Transcript;
namespace rt = astrite::runtime;

TEST_CASE("user input opens a user segment; Started/TurnFinished toggle activity") {
    Transcript t;
    CHECK_FALSE(t.turn_active());
    t.user("hello");
    REQUIRE(t.segments().size() == 1);
    CHECK(t.segments()[0].kind == Segment::Kind::user);
    CHECK(t.segments()[0].text == "hello");

    t.apply(rt::Started{});
    CHECK(t.turn_active());
    t.apply(rt::TurnFinished{});
    CHECK_FALSE(t.turn_active());
}

TEST_CASE("assistant deltas accumulate into one segment; settled marks it done") {
    Transcript t;
    t.apply(rt::Started{});
    t.apply(rt::AssistantDelta{"Hel"});
    t.apply(rt::AssistantDelta{"lo"});
    REQUIRE(t.segments().size() == 1);
    CHECK(t.segments()[0].kind == Segment::Kind::assistant);
    CHECK(t.segments()[0].text == "Hello");
    CHECK_FALSE(t.segments()[0].done);

    t.apply(rt::AssistantSettled{"Hello"});
    CHECK(t.segments()[0].done);
}

TEST_CASE("a settled turn with no deltas still surfaces its text once") {
    Transcript t;
    t.apply(rt::AssistantSettled{"scripted answer"});
    REQUIRE(t.segments().size() == 1);
    CHECK(t.segments()[0].text == "scripted answer");
    CHECK(t.segments()[0].done);
}

TEST_CASE("a pure tool-call settle (empty text) creates no empty segment") {
    Transcript t;
    t.apply(rt::AssistantSettled{""});
    CHECK(t.segments().empty());
}

TEST_CASE("tool segments carry name/args, then complete with the result") {
    Transcript t;
    t.apply(rt::ToolStarted{"c1", "echo", R"({"text":"hi"})"});
    REQUIRE(t.segments().size() == 1);
    const Segment& s = t.segments()[0];
    CHECK(s.kind == Segment::Kind::tool);
    CHECK(s.tool_name == "echo");
    CHECK_FALSE(s.done);

    t.apply(rt::ToolFinished{"c1", "echo", R"({"echoed":"hi"})"});
    CHECK(t.segments()[0].done);
    CHECK(t.segments()[0].tool_result == R"({"echoed":"hi"})");
}

TEST_CASE("a delta after a tool segment opens a fresh assistant segment") {
    Transcript t;
    t.apply(rt::AssistantDelta{"thinking"});
    t.apply(rt::AssistantSettled{"thinking"});
    t.apply(rt::ToolStarted{"c1", "echo", "{}"});
    t.apply(rt::ToolFinished{"c1", "echo", "ok"});
    t.apply(rt::AssistantDelta{"done"});
    REQUIRE(t.segments().size() == 3);
    CHECK(t.segments()[2].kind == Segment::Kind::assistant);
    CHECK(t.segments()[2].text == "done");
}

TEST_CASE("turn errors land as an error segment and end the turn") {
    Transcript t;
    t.apply(rt::Started{});
    t.apply(rt::TurnError{"boom"});
    REQUIRE(t.segments().size() == 1);
    CHECK(t.segments()[0].kind == Segment::Kind::error);
    CHECK(t.segments()[0].text == "boom");
    CHECK_FALSE(t.turn_active());
}
