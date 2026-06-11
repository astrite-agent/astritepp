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

TEST_CASE("anthropic config: defaults to Haiku, requires the key") {
    auto cfg = llm::AnthropicConfig::from_env_or("k", "", "", 0);
    CHECK(cfg.model == "claude-haiku-4-5-20251001");
    CHECK(cfg.base_url == "https://api.anthropic.com/v1");
    CHECK(cfg.max_tokens == 4096);
}
