// GPT (OpenAI Responses API) request translation: chat-completions messages/tools
// -> Responses `input` items + flat function tools, plus reasoning. Pure.
#include <doctest.h>

#include <string>

#include "llm/gpt.hpp"
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

TEST_CASE("gpt: system goes to instructions; user becomes an input message item") {
    Value msgs = Value::list({msg("system", "be terse"), msg("user", "hi")});
    std::string instructions;
    Value input = to_responses_input(msgs, instructions);
    CHECK(instructions == "be terse");
    REQUIRE(input.as_list()->size() == 1);
    const Value& item = (*input.as_list())[0];
    CHECK(*item.get("type") == Value::str("message"));
    CHECK(*item.get("role") == Value::str("user"));
    CHECK(*(*item.get("content")->as_list())[0].get("type") == Value::str("input_text"));
    CHECK(*(*item.get("content")->as_list())[0].get("text") == Value::str("hi"));
}

TEST_CASE("gpt: assistant tool_calls -> function_call items; tool result -> function_call_output") {
    Value tc = Value::object();
    tc.set("id", Value::str("call_1"));
    Value fn = Value::object();
    fn.set("name", Value::str("fs_read"));
    fn.set("arguments", Value::str(R"({"path":"x"})"));
    tc.set("function", std::move(fn));
    Value asst = Value::object();
    asst.set("role", Value::str("assistant"));
    asst.set("content", Value());
    asst.set("tool_calls", Value::list({std::move(tc)}));

    Value tool = Value::object();
    tool.set("role", Value::str("tool"));
    tool.set("tool_call_id", Value::str("call_1"));
    tool.set("content", Value::str("result text"));

    std::string instructions;
    Value input = to_responses_input(Value::list({std::move(asst), std::move(tool)}), instructions);
    REQUIRE(input.as_list()->size() == 2);
    const Value& call = (*input.as_list())[0];
    CHECK(*call.get("type") == Value::str("function_call"));
    CHECK(*call.get("call_id") == Value::str("call_1"));
    CHECK(*call.get("name") == Value::str("fs_read"));
    CHECK(*call.get("arguments") == Value::str(R"({"path":"x"})")); // kept as a JSON string
    const Value& out = (*input.as_list())[1];
    CHECK(*out.get("type") == Value::str("function_call_output"));
    CHECK(*out.get("call_id") == Value::str("call_1"));
    CHECK(*out.get("output") == Value::str("result text"));
}

TEST_CASE("gpt: tools flatten to top-level function shape") {
    Value fn = Value::object();
    fn.set("name", Value::str("echo"));
    fn.set("description", Value::str("d"));
    Value params = Value::object();
    params.set("type", Value::str("object"));
    fn.set("parameters", std::move(params));
    Value tool = Value::object();
    tool.set("type", Value::str("function"));
    tool.set("function", std::move(fn));

    Value out = to_responses_tools(Value::list({std::move(tool)}));
    REQUIRE(out.as_list()->size() == 1);
    const Value& t = (*out.as_list())[0];
    CHECK(*t.get("type") == Value::str("function"));
    CHECK(*t.get("name") == Value::str("echo"));
    CHECK(t.get("parameters") != nullptr); // not nested under "function"
}

TEST_CASE("gpt: body carries model/stream/input/instructions and reasoning when set") {
    Value msgs = Value::list({msg("system", "sys"), msg("user", "hi")});
    std::string body = build_responses_body("gpt-5.2", "medium", msgs, Value::list({}));
    auto parsed = parse_json(body);
    REQUIRE(parsed.has_value());
    CHECK(*parsed->get("model") == Value::str("gpt-5.2"));
    CHECK(*parsed->get("stream") == Value::boolean(true));
    CHECK(*parsed->get("instructions") == Value::str("sys"));
    REQUIRE(parsed->get("reasoning") != nullptr);
    CHECK(*parsed->get("reasoning")->get("effort") == Value::str("medium"));
    CHECK(parsed->get("tools") == nullptr);

    // No reasoning field when effort is empty.
    std::string body2 = build_responses_body("gpt-4o", "", msgs, Value::list({}));
    CHECK(parse_json(body2)->get("reasoning") == nullptr);
}

TEST_CASE("gpt config: reasoning models default to medium effort") {
    CHECK(llm::GptConfig::from_env_or("k", "gpt-5.2", "", "").reasoning_effort == "medium");
    CHECK(llm::GptConfig::from_env_or("k", "gpt-4o", "", "").reasoning_effort.empty());
    CHECK(llm::GptConfig::from_env_or("k", "gpt-5.2", "", "high").reasoning_effort == "high");
}
