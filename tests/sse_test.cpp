#include <doctest.h>

#include <string>
#include <vector>

#include "llm/sse.hpp"

using astrite::llm::CompletedTurn;
using astrite::llm::StreamAccumulator;

namespace {
std::string chunk(std::string_view payload) { return "data: " + std::string(payload) + "\n\n"; }
} // namespace

TEST_CASE("text deltas accumulate across chunks and fire on_delta") {
    std::vector<std::string> deltas;
    StreamAccumulator acc([&](std::string_view d) { deltas.emplace_back(d); });
    acc.feed(chunk(R"({"choices":[{"delta":{"role":"assistant"},"index":0}]})"));
    acc.feed(chunk(R"({"choices":[{"delta":{"content":"Hel"},"index":0}]})"));
    acc.feed(chunk(R"({"choices":[{"delta":{"content":"lo"},"index":0}]})"));
    acc.feed(chunk(R"({"choices":[{"delta":{},"finish_reason":"stop","index":0}]})"));
    acc.feed("data: [DONE]\n\n");

    CompletedTurn turn = acc.settle();
    CHECK(turn.text == "Hello");
    CHECK(turn.finish_reason == "stop");
    CHECK(turn.tool_calls.empty());
    CHECK(deltas == std::vector<std::string>{"Hel", "lo"});
}

TEST_CASE("bytes split mid-line and CRLF endings are handled") {
    StreamAccumulator acc;
    std::string full = chunk(R"({"choices":[{"delta":{"content":"AB"},"index":0}]})");
    // Feed one byte at a time.
    for (char c : full) {
        acc.feed(std::string_view(&c, 1));
    }
    acc.feed("data: {\"choices\":[{\"delta\":{\"content\":\"C\"},\"index\":0}]}\r\n\r\n");
    CHECK(acc.settle().text == "ABC");
}

TEST_CASE("index-keyed tool-call fragments accumulate: id/name overwrite, arguments append") {
    StreamAccumulator acc;
    acc.feed(chunk(
        R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_a","type":"function","function":{"name":"echo","arguments":""}}]},"index":0}]})"));
    acc.feed(chunk(
        R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"{\"a\""}}]},"index":0}]})"));
    acc.feed(chunk(
        R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":": 1}"}}]},"index":0}]})"));
    acc.feed(chunk(R"({"choices":[{"delta":{},"finish_reason":"tool_calls","index":0}]})"));
    acc.feed("data: [DONE]\n\n");

    CompletedTurn turn = acc.settle();
    CHECK(turn.finish_reason == "tool_calls");
    REQUIRE(turn.tool_calls.size() == 1);
    CHECK(turn.tool_calls[0].id == "call_a");
    CHECK(turn.tool_calls[0].name == "echo");
    CHECK(turn.tool_calls[0].arguments == R"({"a": 1})");
}

TEST_CASE("multiple concurrent tool calls stay separated by index, ordered on settle") {
    StreamAccumulator acc;
    acc.feed(chunk(
        R"({"choices":[{"delta":{"tool_calls":[{"index":1,"id":"b","function":{"name":"second","arguments":"{}"}}]},"index":0}]})"));
    acc.feed(chunk(
        R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"a","function":{"name":"first","arguments":"{}"}}]},"index":0}]})"));
    CompletedTurn turn = acc.settle();
    REQUIRE(turn.tool_calls.size() == 2);
    CHECK(turn.tool_calls[0].name == "first");
    CHECK(turn.tool_calls[1].name == "second");
}

TEST_CASE("hostile input: huge index is a map key, never an allocation size; cap at 64 calls") {
    StreamAccumulator acc;
    acc.feed(chunk(
        R"({"choices":[{"delta":{"tool_calls":[{"index":9223372036854775806,"id":"x","function":{"name":"big","arguments":"{}"}}]},"index":0}]})"));
    CHECK(acc.settle().tool_calls.size() == 1); // huge index fine, one entry

    StreamAccumulator flood;
    for (int i = 0; i < 100; ++i) {
        flood.feed(chunk(R"({"choices":[{"delta":{"tool_calls":[{"index":)" + std::to_string(i) +
                         R"(,"id":"c)" + std::to_string(i) +
                         R"(","function":{"name":"f","arguments":"{}"}}]},"index":0}]})"));
    }
    CHECK(flood.settle().tool_calls.size() == 64);
}

TEST_CASE("incomplete tool calls (no name) are filtered on settle") {
    StreamAccumulator acc;
    acc.feed(chunk(
        R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"only_id"}]},"index":0}]})"));
    acc.feed(chunk(
        R"({"choices":[{"delta":{"tool_calls":[{"index":1,"id":"ok","function":{"name":"good","arguments":"{}"}}]},"index":0}]})"));
    CompletedTurn turn = acc.settle();
    REQUIRE(turn.tool_calls.size() == 1);
    CHECK(turn.tool_calls[0].name == "good");
}

TEST_CASE("malformed chunks and unknown fields are tolerated, never fatal") {
    StreamAccumulator acc;
    acc.feed("data: {not json}\n\n");
    acc.feed(": sse comment line\n\n");
    acc.feed("event: ping\n\n");
    acc.feed(chunk(
        R"({"id":"x","object":"chat.completion.chunk","extra":{"deep":1},"choices":[{"delta":{"content":"ok"},"index":0,"logprobs":null}]})"));
    CHECK(acc.settle().text == "ok");
}

TEST_CASE("assistant_message builds the session record: content null for pure tool turns") {
    StreamAccumulator acc;
    acc.feed(chunk(
        R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"t1","function":{"name":"f","arguments":"{}"}}]},"index":0}]})"));
    CompletedTurn turn = acc.settle();
    astrite::schema::Value msg = astrite::llm::assistant_message(turn);
    CHECK(*msg.get("role") == astrite::schema::Value::str("assistant"));
    CHECK(msg.get("content")->is_null());
    REQUIRE(msg.get("tool_calls") != nullptr);
    const auto* calls = msg.get("tool_calls")->as_list();
    REQUIRE(calls != nullptr);
    REQUIRE(calls->size() == 1);
    CHECK(*(*calls)[0].get("id") == astrite::schema::Value::str("t1"));
    CHECK(*(*calls)[0].get("function")->get("name") == astrite::schema::Value::str("f"));

    StreamAccumulator text_acc;
    text_acc.feed(chunk(R"({"choices":[{"delta":{"content":"hi"},"index":0}]})"));
    astrite::schema::Value text_msg = astrite::llm::assistant_message(text_acc.settle());
    CHECK(*text_msg.get("content") == astrite::schema::Value::str("hi"));
    CHECK(text_msg.get("tool_calls") == nullptr);
}
