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
    CHECK_FALSE(llm::GptConfig::from_env_or("k", "gpt-5.2", "", "").reasoning_summary); // off default
}

// ---- reasoning: request, capture (summary for display + items for round-trip) ----

namespace {
std::string sse(std::initializer_list<std::string> events) {
    std::string out;
    for (const std::string& e : events) {
        out += "data: " + e + "\n";
    }
    return out;
}

// An assistant tool-call turn carrying a captured Responses reasoning item.
Value gpt_asst_with_reasoning(const std::string& model) {
    Value st = Value::object();
    st.set("type", Value::str("summary_text"));
    st.set("text", Value::str("weigh it"));
    Value b = Value::object();
    b.set("type", Value::str("reasoning"));
    b.set("id", Value::str("rs_1"));
    b.set("summary", Value::list({std::move(st)}));
    b.set("encrypted_content", Value::str("ENC=="));
    Value reasoning = Value::object();
    reasoning.set("provider", Value::str("gpt"));
    reasoning.set("model", Value::str(model));
    reasoning.set("blocks", Value::list({std::move(b)}));

    Value fn = Value::object();
    fn.set("name", Value::str("get_weather"));
    fn.set("arguments", Value::str(R"({"city":"Paris"})"));
    Value tc = Value::object();
    tc.set("id", Value::str("call_1"));
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

TEST_CASE("gpt body: summary requests a reasoning summary only when reasoning is on") {
    Value msgs = Value::list({msg("user", "hi")});
    std::string body =
        build_responses_body("gpt-5.2", "medium", msgs, Value::list({}), /*summary=*/true);
    auto parsed = parse_json(body);
    REQUIRE(parsed->get("reasoning") != nullptr);
    CHECK(*parsed->get("reasoning")->get("summary") == Value::str("auto"));

    // No reasoning block at all (no effort) => no summary either.
    std::string body2 = build_responses_body("gpt-4o", "", msgs, Value::list({}), /*summary=*/true);
    CHECK(parse_json(body2)->get("reasoning") == nullptr);
}

TEST_CASE("gpt fold: reasoning summary is captured separately from the visible output") {
    std::string stream = sse({
        R"({"type":"response.reasoning_summary_text.delta","delta":"plan: "})",
        R"({"type":"response.reasoning_summary_text.delta","delta":"add them"})",
        R"({"type":"response.output_text.delta","delta":"The total is 5."})",
        R"({"type":"response.completed","response":{"usage":{"input_tokens":3,"output_tokens":4,"total_tokens":7}}})",
    });
    CompletedTurn turn = fold_responses_sse(stream);
    CHECK(turn.text == "The total is 5.");        // visible output only
    CHECK(turn.reasoning.text == "plan: add them"); // summary captured for display
    CHECK(turn.reasoning.provider == "gpt");
    // Summary deltas alone (no reasoning *item*) carry no round-trip payload.
    CHECK_FALSE(turn.reasoning.blocks.has_value());
}

TEST_CASE("gpt fold: a reasoning item is captured verbatim for round-trip") {
    std::string stream = sse({
        R"({"type":"response.output_item.added","item":{"type":"reasoning","id":"rs_1"}})",
        R"({"type":"response.output_item.done","item":{"type":"reasoning","id":"rs_1","summary":[{"type":"summary_text","text":"weigh it"}],"encrypted_content":"ENC==","content":[]}})",
        R"({"type":"response.output_item.added","item":{"type":"function_call","id":"fc_1","call_id":"call_1","name":"get_weather"}})",
        R"({"type":"response.function_call_arguments.delta","item_id":"fc_1","delta":"{\"city\":\"Paris\"}"})",
        R"({"type":"response.completed","response":{"usage":{"input_tokens":5,"output_tokens":6,"total_tokens":11}}})",
    });
    CompletedTurn turn = fold_responses_sse(stream);
    REQUIRE(turn.tool_calls.size() == 1);
    CHECK(turn.reasoning.provider == "gpt");
    CHECK(turn.reasoning.text == "weigh it"); // display text falls back to the item summary
    REQUIRE(turn.reasoning.blocks.has_value());
    const auto* blks = turn.reasoning.blocks->as_list();
    REQUIRE(blks != nullptr);
    REQUIRE(blks->size() == 1);
    const Value& b = (*blks)[0];
    CHECK(*b.get("type") == Value::str("reasoning"));
    CHECK(*b.get("id") == Value::str("rs_1"));
    CHECK(*b.get("encrypted_content") == Value::str("ENC==")); // the carrier, verbatim
    REQUIRE(b.get("summary") != nullptr);                       // required when echoed back
    REQUIRE(b.get("summary")->as_list() != nullptr);
    CHECK(b.get("content") == nullptr); // dropped: not needed and not accepted on input
}

TEST_CASE("gpt body: reasoning round-trip goes stateless (store:false + encrypted_content)") {
    Value msgs = Value::list({msg("user", "hi")});
    std::string body =
        build_responses_body("gpt-5.2", "medium", msgs, Value::list({}), /*summary=*/true);
    auto parsed = parse_json(body);
    REQUIRE(parsed.has_value());
    CHECK(*parsed->get("store") == Value::boolean(false));
    REQUIRE(parsed->get("include") != nullptr);
    REQUIRE(parsed->get("include")->as_list() != nullptr);
    CHECK((*parsed->get("include")->as_list())[0] == Value::str("reasoning.encrypted_content"));

    // Reasoning off => no stateless override (today's behavior preserved).
    std::string body2 =
        build_responses_body("gpt-5.2", "medium", msgs, Value::list({}), /*summary=*/false);
    auto parsed2 = parse_json(body2);
    CHECK(parsed2->get("store") == nullptr);
    CHECK(parsed2->get("include") == nullptr);
}

TEST_CASE("gpt round-trip: the reasoning item leads the assistant items on the same model") {
    std::string instr;
    Value out = to_responses_input(Value::list({gpt_asst_with_reasoning("gpt-5.2")}), instr,
                                   /*model=*/"gpt-5.2", /*reasoning=*/true);
    const Value::List& items = *out.as_list();
    REQUIRE(items.size() == 2);
    CHECK(*items[0].get("type") == Value::str("reasoning")); // reasoning FIRST
    CHECK(*items[0].get("id") == Value::str("rs_1"));
    CHECK(*items[0].get("encrypted_content") == Value::str("ENC=="));
    CHECK(*items[1].get("type") == Value::str("function_call")); // then the call
}

TEST_CASE("gpt round-trip: a model switch drops the reasoning item") {
    std::string instr;
    Value out = to_responses_input(Value::list({gpt_asst_with_reasoning("gpt-5.2")}), instr,
                                   /*model=*/"gpt-4o", /*reasoning=*/true);
    const Value::List& items = *out.as_list();
    REQUIRE(items.size() == 1);
    CHECK(*items[0].get("type") == Value::str("function_call")); // dropped — other model
}

TEST_CASE("gpt round-trip: disabled reasoning drops the item even on the same model") {
    std::string instr;
    Value out = to_responses_input(Value::list({gpt_asst_with_reasoning("gpt-5.2")}), instr,
                                   /*model=*/"gpt-5.2", /*reasoning=*/false);
    REQUIRE(out.as_list()->size() == 1);
    CHECK(*(*out.as_list())[0].get("type") == Value::str("function_call"));
}

TEST_CASE("gpt fold: summary deltas never reach the visible delta sink") {
    std::string streamed;
    std::string stream = sse({
        R"({"type":"response.reasoning_summary_text.delta","delta":"hidden"})",
        R"({"type":"response.output_text.delta","delta":"shown"})",
    });
    (void)fold_responses_sse(stream, [&](std::string_view d) { streamed += d; });
    CHECK(streamed == "shown");
}
