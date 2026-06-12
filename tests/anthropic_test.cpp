// Anthropic request translation: OpenAI-shaped messages/tools -> Anthropic
// Messages API shape, including system extraction, tool_use/tool_result blocks,
// and cache_control placement. Pure (no network).
#include <doctest.h>

#include <string>

#include "llm/anthropic.hpp"
#include "schema/value_json.hpp"

using namespace astrite;
using namespace astrite::llm;
using namespace astrite::schema;

namespace {
Value msg(std::string role, std::string content) {
    Value m = Value::object();
    m.set("role", Value::str(std::move(role)));
    m.set("content", Value::str(std::move(content)));
    return m;
}
} // namespace

TEST_CASE("anthropic: system messages are pulled out; user/assistant become messages") {
    Value msgs = Value::list({msg("system", "be terse"), msg("user", "hi"),
                              msg("assistant", "hello")});
    std::string system;
    Value out = to_anthropic_messages(msgs, system, /*cache=*/false);
    CHECK(system == "be terse");
    REQUIRE(out.as_list() != nullptr);
    REQUIRE(out.as_list()->size() == 2);
    CHECK(*(*out.as_list())[0].get("role") == Value::str("user"));
    CHECK(*(*out.as_list())[1].get("role") == Value::str("assistant"));
}

TEST_CASE("anthropic: assistant tool_calls become tool_use blocks with parsed input") {
    Value tc = Value::object();
    tc.set("id", Value::str("call_1"));
    tc.set("type", Value::str("function"));
    Value fn = Value::object();
    fn.set("name", Value::str("fs_read"));
    fn.set("arguments", Value::str(R"({"path":"x.txt"})"));
    tc.set("function", std::move(fn));
    Value asst = Value::object();
    asst.set("role", Value::str("assistant"));
    asst.set("content", Value());
    asst.set("tool_calls", Value::list({std::move(tc)}));

    std::string system;
    Value out = to_anthropic_messages(Value::list({std::move(asst)}), system, false);
    const Value& block = (*(*out.as_list())[0].get("content")->as_list())[0];
    CHECK(*block.get("type") == Value::str("tool_use"));
    CHECK(*block.get("id") == Value::str("call_1"));
    CHECK(*block.get("name") == Value::str("fs_read"));
    REQUIRE(block.get("input") != nullptr);
    CHECK(*block.get("input")->get("path") == Value::str("x.txt")); // parsed to an object
}

TEST_CASE("anthropic: consecutive tool results coalesce into one user turn of tool_result blocks") {
    Value t1 = Value::object();
    t1.set("role", Value::str("tool"));
    t1.set("tool_call_id", Value::str("a"));
    t1.set("content", Value::str("ra"));
    Value t2 = Value::object();
    t2.set("role", Value::str("tool"));
    t2.set("tool_call_id", Value::str("b"));
    t2.set("content", Value::str("rb"));

    std::string system;
    Value out = to_anthropic_messages(Value::list({std::move(t1), std::move(t2)}), system, false);
    REQUIRE(out.as_list()->size() == 1); // one user message...
    const Value& m = (*out.as_list())[0];
    CHECK(*m.get("role") == Value::str("user"));
    REQUIRE(m.get("content")->as_list()->size() == 2); // ...with two tool_result blocks
    CHECK(*(*m.get("content")->as_list())[0].get("type") == Value::str("tool_result"));
    CHECK(*(*m.get("content")->as_list())[0].get("tool_use_id") == Value::str("a"));
}

TEST_CASE("anthropic: cache_control marks the last block when caching is on") {
    Value msgs = Value::list({msg("user", "hello")});
    std::string system;
    Value out = to_anthropic_messages(msgs, system, /*cache=*/true);
    // the user's string content is promoted to a block array with cache_control
    const Value& m = (*out.as_list())[0];
    REQUIRE(m.get("content")->as_list() != nullptr);
    const Value& block = m.get("content")->as_list()->back();
    REQUIRE(block.get("cache_control") != nullptr);
    CHECK(*block.get("cache_control")->get("type") == Value::str("ephemeral"));
}

TEST_CASE("anthropic: tools convert to {name, description, input_schema}") {
    Value fn = Value::object();
    fn.set("name", Value::str("echo"));
    fn.set("description", Value::str("echoes"));
    Value params = Value::object();
    params.set("type", Value::str("object"));
    fn.set("parameters", std::move(params));
    Value tool = Value::object();
    tool.set("type", Value::str("function"));
    tool.set("function", std::move(fn));

    Value out = to_anthropic_tools(Value::list({std::move(tool)}));
    REQUIRE(out.as_list()->size() == 1);
    const Value& t = (*out.as_list())[0];
    CHECK(*t.get("name") == Value::str("echo"));
    CHECK(*t.get("description") == Value::str("echoes"));
    REQUIRE(t.get("input_schema") != nullptr);
    CHECK(*t.get("input_schema")->get("type") == Value::str("object"));
}

TEST_CASE("anthropic: the request body carries model, max_tokens, stream, cached system") {
    Value msgs = Value::list({msg("system", "sys"), msg("user", "hi")});
    std::string body = build_anthropic_body("claude-haiku-4-5", 4096, msgs, Value::list({}), true);
    auto parsed = parse_json(body);
    REQUIRE(parsed.has_value());
    CHECK(*parsed->get("model") == Value::str("claude-haiku-4-5"));
    CHECK(*parsed->get("max_tokens") == Value::integer(4096));
    CHECK(*parsed->get("stream") == Value::boolean(true));
    // system rendered as a cache-marked block array
    REQUIRE(parsed->get("system")->as_list() != nullptr);
    const Value& sys = (*parsed->get("system")->as_list())[0];
    CHECK(*sys.get("text") == Value::str("sys"));
    CHECK(*sys.get("cache_control")->get("type") == Value::str("ephemeral"));
    CHECK(parsed->get("tools") == nullptr); // empty tools omitted
}

// ---- extended thinking: capture, sink isolation, round-trip --------------------

namespace {
// Wrap JSON payloads as an Anthropic SSE byte stream (one `data:` line each).
std::string sse(std::initializer_list<std::string> events) {
    std::string out;
    for (const std::string& e : events) {
        out += "data: " + e + "\n";
    }
    return out;
}

// An assistant tool-call turn carrying a captured anthropic thinking block.
Value asst_with_reasoning(const std::string& model) {
    Value tb = Value::object();
    tb.set("type", Value::str("thinking"));
    tb.set("thinking", Value::str("weigh options"));
    tb.set("signature", Value::str("SIG=="));
    Value reasoning = Value::object();
    reasoning.set("provider", Value::str("anthropic"));
    reasoning.set("model", Value::str(model));
    reasoning.set("blocks", Value::list({std::move(tb)}));

    Value fn = Value::object();
    fn.set("name", Value::str("fs_read"));
    fn.set("arguments", Value::str(R"({"path":"x"})"));
    Value tc = Value::object();
    tc.set("id", Value::str("tu_1"));
    tc.set("type", Value::str("function"));
    tc.set("function", std::move(fn));

    Value asst = Value::object();
    asst.set("role", Value::str("assistant"));
    asst.set("content", Value());
    asst.set("tool_calls", Value::list({std::move(tc)}));
    asst.set("_reasoning", std::move(reasoning));
    return asst;
}
} // namespace

TEST_CASE("anthropic fold: thinking is captured separately from the visible text") {
    std::string stream = sse({
        R"({"type":"message_start","message":{"usage":{"input_tokens":10}}})",
        R"({"type":"content_block_start","index":0,"content_block":{"type":"thinking"}})",
        R"({"type":"content_block_delta","index":0,"delta":{"type":"thinking_delta","thinking":"weigh "}})",
        R"({"type":"content_block_delta","index":0,"delta":{"type":"thinking_delta","thinking":"options"}})",
        R"({"type":"content_block_delta","index":0,"delta":{"type":"signature_delta","signature":"SIG=="}})",
        R"({"type":"content_block_start","index":1,"content_block":{"type":"text"}})",
        R"({"type":"content_block_delta","index":1,"delta":{"type":"text_delta","text":"Here you go."}})",
        R"({"type":"content_block_start","index":2,"content_block":{"type":"tool_use","id":"tu_1","name":"fs_read"}})",
        R"({"type":"content_block_delta","index":2,"delta":{"type":"input_json_delta","partial_json":"{\"path\":\"x\"}"}})",
        R"({"type":"message_delta","delta":{"stop_reason":"tool_use"},"usage":{"output_tokens":5}})",
    });
    CompletedTurn turn = fold_anthropic_sse(stream);
    CHECK(turn.text == "Here you go.");            // visible text only — thinking excluded
    CHECK(turn.reasoning.text == "weigh options"); // thinking captured for display
    REQUIRE(turn.tool_calls.size() == 1);
    CHECK(turn.tool_calls[0].name == "fs_read");
    REQUIRE(turn.reasoning.blocks.has_value());
    REQUIRE(turn.reasoning.blocks->as_list() != nullptr);
    REQUIRE(turn.reasoning.blocks->as_list()->size() == 1);
    const Value& tb = (*turn.reasoning.blocks->as_list())[0];
    CHECK(*tb.get("type") == Value::str("thinking"));
    CHECK(*tb.get("thinking") == Value::str("weigh options")); // verbatim for round-trip
    CHECK(*tb.get("signature") == Value::str("SIG=="));
}

TEST_CASE("anthropic fold: thinking deltas never reach the visible delta sink") {
    std::string streamed;
    std::string stream = sse({
        R"({"type":"content_block_start","index":0,"content_block":{"type":"thinking"}})",
        R"({"type":"content_block_delta","index":0,"delta":{"type":"thinking_delta","thinking":"secret"}})",
        R"({"type":"content_block_start","index":1,"content_block":{"type":"text"}})",
        R"({"type":"content_block_delta","index":1,"delta":{"type":"text_delta","text":"visible"}})",
    });
    (void)fold_anthropic_sse(stream, [&](std::string_view d) { streamed += d; });
    CHECK(streamed == "visible"); // the human stream stays clean
}

TEST_CASE("anthropic fold: redacted_thinking blocks are preserved verbatim") {
    std::string stream = sse({
        R"({"type":"content_block_start","index":0,"content_block":{"type":"redacted_thinking","data":"ENC"}})",
        R"({"type":"content_block_start","index":1,"content_block":{"type":"text"}})",
        R"({"type":"content_block_delta","index":1,"delta":{"type":"text_delta","text":"ok"}})",
    });
    CompletedTurn turn = fold_anthropic_sse(stream);
    REQUIRE(turn.reasoning.blocks.has_value());
    const Value& b = (*turn.reasoning.blocks->as_list())[0];
    CHECK(*b.get("type") == Value::str("redacted_thinking"));
    CHECK(*b.get("data") == Value::str("ENC"));
}

TEST_CASE("anthropic round-trip: thinking blocks lead the assistant turn on the same model") {
    std::string system;
    Value out = to_anthropic_messages(Value::list({asst_with_reasoning("claude-haiku-4-5")}), system,
                                      /*cache=*/false, /*model=*/"claude-haiku-4-5",
                                      /*thinking=*/true);
    const Value::List& blocks = *(*out.as_list())[0].get("content")->as_list();
    REQUIRE(blocks.size() == 2);
    CHECK(*blocks[0].get("type") == Value::str("thinking"));  // thinking must come FIRST
    CHECK(*blocks[0].get("signature") == Value::str("SIG=="));// re-emitted verbatim
    CHECK(*blocks[1].get("type") == Value::str("tool_use"));  // then the tool_use
}

TEST_CASE("anthropic round-trip: a model switch drops the thinking blocks") {
    std::string system;
    Value out = to_anthropic_messages(Value::list({asst_with_reasoning("claude-haiku-4-5")}), system,
                                      false, /*model=*/"claude-opus-4-8", /*thinking=*/true);
    const Value::List& blocks = *(*out.as_list())[0].get("content")->as_list();
    REQUIRE(blocks.size() == 1);
    CHECK(*blocks[0].get("type") == Value::str("tool_use")); // dropped — other model
}

TEST_CASE("anthropic round-trip: thinking disabled drops the blocks even on the same model") {
    std::string system;
    Value out = to_anthropic_messages(Value::list({asst_with_reasoning("claude-haiku-4-5")}), system,
                                      false, "claude-haiku-4-5", /*thinking=*/false);
    const Value::List& blocks = *(*out.as_list())[0].get("content")->as_list();
    REQUIRE(blocks.size() == 1);
    CHECK(*blocks[0].get("type") == Value::str("tool_use"));
}

TEST_CASE("anthropic body: a thinking budget adds the thinking param and lifts max_tokens above it") {
    Value msgs = Value::list({msg("user", "hi")});
    std::string body = build_anthropic_body("claude-haiku-4-5", 1024, msgs, Value::list({}),
                                            /*cache=*/true, /*thinking_budget=*/2048);
    auto parsed = parse_json(body);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->get("thinking") != nullptr);
    CHECK(*parsed->get("thinking")->get("type") == Value::str("enabled"));
    CHECK(*parsed->get("thinking")->get("budget_tokens") == Value::integer(2048));
    REQUIRE(parsed->get("max_tokens")->as_int() != nullptr);
    CHECK(*parsed->get("max_tokens")->as_int() > 2048); // max_tokens must exceed the budget
}

TEST_CASE("anthropic config: defaults to Haiku, requires the key") {
    auto cfg = llm::AnthropicConfig::from_env_or("k", "", "", 0, 0);
    CHECK(cfg.model == "claude-haiku-4-5-20251001");
    CHECK(cfg.base_url == "https://api.anthropic.com/v1");
    CHECK(cfg.max_tokens == 4096);
    CHECK(cfg.thinking_budget == 0); // off by default
}
