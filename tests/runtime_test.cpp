#include <doctest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "runtime/runtime.hpp"
#include "runtime/session.hpp"
#include "schema/load.hpp"
#include "schema/value_json.hpp"

using namespace astrite;
using namespace astrite::schema;
namespace fs = std::filesystem;

namespace {

fs::path temp_session() {
    static int counter = 0;
    fs::path p = fs::temp_directory_path() /
                 ("astrite_test_session_" + std::to_string(++counter) + ".jsonl");
    fs::remove(p);
    return p;
}

Value user_msg(std::string text) {
    Value m = Value::object();
    m.set("role", Value::str("user"));
    m.set("content", Value::str(std::move(text)));
    return m;
}

} // namespace

TEST_CASE("session: append/read round-trips records; missing file reads empty") {
    fs::path file = temp_session();
    runtime::Session session(file);
    auto empty = session.read();
    REQUIRE(empty.has_value());
    CHECK(empty->get("messages")->as_list()->empty());

    REQUIRE(session.append(user_msg("one")).has_value());
    REQUIRE(session.append(user_msg("two")).has_value());

    auto ctx = session.read();
    REQUIRE(ctx.has_value());
    const Value::List* msgs = ctx->get("messages")->as_list();
    REQUIRE(msgs != nullptr);
    REQUIRE(msgs->size() == 2);
    CHECK(*(*msgs)[1].get("content") == Value::str("two"));

    // A fresh Session over the same file sees the same records (persistence).
    runtime::Session reopened(file);
    CHECK(reopened.read()->get("messages")->as_list()->size() == 2);
    fs::remove(file);
}

TEST_CASE("session: a corrupt line fails the read closed") {
    fs::path file = temp_session();
    runtime::Session session(file);
    REQUIRE(session.append(user_msg("ok")).has_value());
    {
        std::ofstream out(file, std::ios::app);
        out << "{corrupt\n";
    }
    CHECK_FALSE(session.read().has_value());
    fs::remove(file);
}

namespace {

// The real turn loop, as data — the same shape as components/turn.json.
constexpr std::string_view TURN_JSONC = R"({
  "component": "turn",
  "kind": "agent",
  "caps": ["session.read", "session.append", "llm.stream", "dispatch"],
  "tree": {
    "repeat": {
      "max": 8,
      "until": {"when": "bool.is_true", "with": {"in": {"ref": "done"}}},
      "body": {
        "all": [
          {"do": "session.read", "as": "ctx"},
          {
            "do": "llm.stream",
            "with": {"messages": {"ref": "ctx.messages"}, "tools": {"ref": "cfg.tools"}},
            "as": "resp"
          },
          {"do": "session.append", "with": {"record": {"ref": "resp.message"}}},
          {"do": "coll.is_empty", "with": {"in": {"ref": "resp.tool_calls"}}, "as": "done"},
          {
            "any": [
              {"when": "bool.is_true", "with": {"in": {"ref": "done"}}},
              {
                "foreach": {
                  "over": {"ref": "resp.tool_calls"},
                  "as": "tc",
                  "body": {
                    "all": [
                      {"do": "dispatch", "with": {"call": {"ref": "tc"}}, "as": "out"},
                      {
                        "do": "obj.build",
                        "with": {
                          "role": "tool",
                          "tool_call_id": {"ref": "out.tool_call_id"},
                          "content": {"ref": "out.content"}
                        },
                        "as": "tool_msg"
                      },
                      {"do": "session.append", "with": {"record": {"ref": "tool_msg"}}}
                    ]
                  }
                }
              }
            ]
          }
        ]
      }
    }
  }
})";

constexpr std::string_view ECHO_JSONC = R"({
  "component": "echo",
  "kind": "tool",
  "caps": [],
  "description": "Echo the input back.",
  "parameters": {"type": "object", "properties": {"text": {"type": "string"}}},
  "tree": {"do": "obj.build", "with": {"echoed": {"ref": "input.text"}}, "as": "out"},
  "output": {"ref": "out"}
})";

struct ScriptedChat {
    std::vector<llm::CompletedTurn> script;
    std::vector<size_t> seen_message_counts;
    size_t calls = 0;

    runtime::ChatFn fn() {
        return [this](const Value& messages, const Value&,
                      llm::StreamAccumulator::DeltaSink on_delta)
                   -> std::expected<llm::CompletedTurn, std::string> {
            seen_message_counts.push_back(messages.as_list()->size());
            if (calls >= script.size()) {
                return std::unexpected("script exhausted");
            }
            llm::CompletedTurn turn = script[calls++];
            if (on_delta && !turn.text.empty()) {
                on_delta(turn.text);
            }
            return turn;
        };
    }
};

runtime::Options test_options(fs::path session_file, runtime::ChatFn chat) {
    runtime::Options opts;
    opts.session_file = std::move(session_file);
    opts.system_prompt = "You are astrite++.";
    opts.chat = std::move(chat);
    auto turn = load_component_jsonc(TURN_JSONC);
    auto echo = load_component_jsonc(ECHO_JSONC);
    REQUIRE(turn.has_value());
    REQUIRE(echo.has_value());
    opts.components.emplace("turn", std::move(*turn));
    opts.components.emplace("echo", std::move(*echo));
    return opts;
}

std::vector<runtime::Event> drain(runtime::Runtime& rt) {
    std::vector<runtime::Event> events;
    while (auto e = rt.events().try_pop()) {
        events.push_back(std::move(*e));
    }
    return events;
}

template <class T>
size_t count_of(const std::vector<runtime::Event>& events) {
    size_t n = 0;
    for (const auto& e : events) {
        if (std::holds_alternative<T>(e)) {
            ++n;
        }
    }
    return n;
}

} // namespace

TEST_CASE("a full turn: tool dispatch loops back through the session, then settles") {
    fs::path file = temp_session();
    ScriptedChat chat;
    chat.script.push_back({"", {{"call_1", "echo", R"({"text": "hi"})"}}, "tool_calls"});
    chat.script.push_back({"Echoed it for you.", {}, "stop"});

    auto rt = runtime::Runtime::create(test_options(file, chat.fn()));
    REQUIRE(rt.has_value());
    REQUIRE((*rt)->submit_user_message("please echo hi"));

    // Session: system, user, assistant(tool_calls), tool result, assistant(text).
    runtime::Session session(file);
    const Value::List msgs = *session.read()->get("messages")->as_list();
    REQUIRE(msgs.size() == 5);
    CHECK(*msgs[0].get("role") == Value::str("system"));
    CHECK(*msgs[1].get("role") == Value::str("user"));
    CHECK(*msgs[2].get("role") == Value::str("assistant"));
    CHECK(msgs[2].get("content")->is_null()); // pure tool-call turn
    CHECK(*msgs[3].get("role") == Value::str("tool"));
    CHECK(*msgs[3].get("tool_call_id") == Value::str("call_1"));
    CHECK(msgs[3].get("content")->as_str()->find("echoed") != std::string::npos);
    CHECK(*msgs[4].get("content") == Value::str("Echoed it for you."));

    // The second model call saw the tool result (4 messages by then).
    CHECK(chat.seen_message_counts == std::vector<size_t>{2, 4});

    auto events = drain(**rt);
    CHECK(count_of<runtime::Started>(events) == 1);
    CHECK(count_of<runtime::ToolStarted>(events) == 1);
    CHECK(count_of<runtime::ToolFinished>(events) == 1);
    CHECK(count_of<runtime::AssistantDelta>(events) == 1);
    CHECK(count_of<runtime::TurnFinished>(events) == 1);
    CHECK(count_of<runtime::TurnError>(events) == 0);
    CHECK(std::holds_alternative<runtime::TurnFinished>(events.back()));
    fs::remove(file);
}

TEST_CASE("transcript continuity across submits; system prompt seeded exactly once") {
    fs::path file = temp_session();
    ScriptedChat chat;
    chat.script.push_back({"first answer", {}, "stop"});
    chat.script.push_back({"second answer", {}, "stop"});

    auto rt = runtime::Runtime::create(test_options(file, chat.fn()));
    REQUIRE(rt.has_value());
    REQUIRE((*rt)->submit_user_message("one"));
    REQUIRE((*rt)->submit_user_message("two"));

    // system, user, assistant, user, assistant — one system record only.
    runtime::Session session(file);
    const Value::List msgs = *session.read()->get("messages")->as_list();
    REQUIRE(msgs.size() == 5);
    CHECK(chat.seen_message_counts == std::vector<size_t>{2, 4});
    fs::remove(file);
}

TEST_CASE("a chat transport error surfaces as TurnError, exactly one terminal event") {
    fs::path file = temp_session();
    auto rt = runtime::Runtime::create(test_options(
        file, [](const Value&, const Value&, llm::StreamAccumulator::DeltaSink)
                  -> std::expected<llm::CompletedTurn, std::string> {
            return std::unexpected("boom");
        }));
    REQUIRE(rt.has_value());
    CHECK_FALSE((*rt)->submit_user_message("hello"));
    auto events = drain(**rt);
    CHECK(count_of<runtime::TurnError>(events) == 1);
    CHECK(count_of<runtime::TurnFinished>(events) == 0);
    CHECK(std::holds_alternative<runtime::TurnError>(events.back()));
    fs::remove(file);
}

TEST_CASE("an under-declared turn component is rejected at construction, fail closed") {
    fs::path file = temp_session();
    ScriptedChat chat;
    runtime::Options opts = test_options(file, chat.fn());
    opts.components.find("turn")->second.caps = {"session.read"}; // strip authority
    auto rt = runtime::Runtime::create(std::move(opts));
    REQUIRE_FALSE(rt.has_value());
    CHECK(rt.error().find("llm.stream") != std::string::npos);
    fs::remove(file);
}

TEST_CASE("model-callable tool definitions are derived from tool components") {
    fs::path file = temp_session();
    bool saw_tools = false;
    auto rt = runtime::Runtime::create(test_options(
        file, [&](const Value&, const Value& tools, llm::StreamAccumulator::DeltaSink)
                  -> std::expected<llm::CompletedTurn, std::string> {
            const Value::List* list = tools.as_list();
            REQUIRE(list != nullptr);
            REQUIRE(list->size() == 1); // echo only; `turn` is an agent, not a tool
            const Value& def = (*list)[0];
            CHECK(*def.get("type") == Value::str("function"));
            CHECK(*def.get("function")->get("name") == Value::str("echo"));
            CHECK(def.get("function")->get("parameters") != nullptr);
            saw_tools = true;
            return llm::CompletedTurn{"ok", {}, "stop"};
        }));
    REQUIRE(rt.has_value());
    REQUIRE((*rt)->submit_user_message("x"));
    CHECK(saw_tools);
    fs::remove(file);
}
