// Typed transcript persistence: save the full transcript (all segment kinds) to
// JSONL and restore it, so a reopened client shows the prior conversation.
#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "frontends/tui/transcript.hpp"
#include "frontends/tui/transcript_io.hpp"

namespace fs = std::filesystem;
using namespace astrite::tui;

namespace {
fs::path tmp(std::string tag) {
    static int n = 0;
    fs::path p = fs::temp_directory_path() / ("astrite_tx_" + std::to_string(++n) + "_" + tag + ".jsonl");
    fs::remove(p);
    return p;
}
} // namespace

TEST_CASE("transcript io: round-trips every segment kind") {
    Transcript t;
    t.user("hello");
    t.apply(astrite::runtime::AssistantSettled{"hi there", 0, 0, 0, false});
    t.apply(astrite::runtime::ToolStarted{"id", "fs_read", R"({"path":"x"})"});
    t.apply(astrite::runtime::ToolFinished{"id", "fs_read", "file contents"});
    t.apply(astrite::runtime::TurnError{"boom"});
    astrite::runtime::ComponentProposed p;
    p.name = "wave";
    p.kind = "tool";
    p.risk = "exec";
    p.caps = {"task.run"};
    t.apply(p);
    t.notice("approved wave");

    fs::path path = tmp("roundtrip");
    REQUIRE(save_transcript(path, t.segments()));

    std::vector<Segment> loaded = load_transcript(path);
    REQUIRE(loaded.size() == t.segments().size());

    CHECK(loaded[0].kind == Segment::Kind::user);
    CHECK(loaded[0].text == "hello");
    CHECK(loaded[1].kind == Segment::Kind::assistant);
    CHECK(loaded[1].text == "hi there");
    CHECK(loaded[2].kind == Segment::Kind::tool);
    CHECK(loaded[2].tool_name == "fs_read");
    CHECK(loaded[2].tool_result == "file contents");
    CHECK(loaded[3].kind == Segment::Kind::error);
    CHECK(loaded[3].text == "boom");
    CHECK(loaded[4].kind == Segment::Kind::proposal);
    CHECK(loaded[4].tool_name == "wave");
    CHECK(loaded[4].risk == "exec");
    CHECK(loaded[4].caps == std::vector<std::string>{"task.run"});
    CHECK(loaded[5].kind == Segment::Kind::notice);

    for (const Segment& s : loaded) {
        CHECK(s.done);
    }

    Transcript restored;
    restored.restore(loaded);
    CHECK(restored.segments().size() == loaded.size());
    CHECK_FALSE(restored.turn_active());
    fs::remove(path);
}

TEST_CASE("transcript io: a proposal's resolution survives a round-trip") {
    Transcript t;
    astrite::runtime::ComponentProposed p;
    p.name = "wave";
    p.kind = "tool";
    p.risk = "none";
    t.apply(p);
    t.set_resolution(0, 1); // approved

    fs::path path = tmp("resolution");
    REQUIRE(save_transcript(path, t.segments()));
    std::vector<Segment> loaded = load_transcript(path);
    REQUIRE(loaded.size() == 1);
    CHECK(loaded[0].kind == Segment::Kind::proposal);
    CHECK(loaded[0].resolution == 1);
    fs::remove(path);
}

TEST_CASE("transcript io: a missing file restores nothing") {
    CHECK(load_transcript(fs::temp_directory_path() / "astrite_no_such_transcript.jsonl").empty());
}

TEST_CASE("transcript io: blank and corrupt lines are skipped, not fatal") {
    fs::path path = tmp("corrupt");
    {
        std::ofstream o(path, std::ios::binary);
        o << R"({"type":"user","text":"one"})" << "\n";
        o << "\n";              // blank
        o << "{ not json\n";    // corrupt
        o << R"({"type":"notice","text":"two"})" << "\n";
    }
    std::vector<Segment> loaded = load_transcript(path);
    REQUIRE(loaded.size() == 2);
    CHECK(loaded[0].text == "one");
    CHECK(loaded[1].kind == Segment::Kind::notice);
    CHECK(loaded[1].text == "two");
    fs::remove(path);
}

TEST_CASE("transcript io: a streaming (not-done) segment is not persisted") {
    Transcript t;
    t.apply(astrite::runtime::AssistantDelta{"partial"}); // opens an undone assistant segment
    REQUIRE(t.segments().size() == 1);
    CHECK_FALSE(t.segments()[0].done);

    fs::path path = tmp("streaming");
    REQUIRE(save_transcript(path, t.segments()));
    CHECK(load_transcript(path).empty()); // nothing finalized yet
    fs::remove(path);
}
