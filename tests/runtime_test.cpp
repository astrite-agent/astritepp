#include <doctest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

#include "embedded_components.hpp"
#include "runtime/fs_read_args.hpp"
#include "runtime/proposals.hpp"
#include "runtime/runtime.hpp"
#include "runtime/task_args.hpp"
#include "runtime/trust_store.hpp"
#include "sandbox/sandbox.hpp"
#include "util/sha256.hpp"
#include "runtime/session.hpp"
#include "schema/load.hpp"
#include "schema/value_json.hpp"

using namespace astrite;
using namespace astrite::schema;
namespace fs = std::filesystem;

namespace {
Value with_range(std::optional<std::int64_t> offset, std::optional<std::int64_t> limit) {
    Value v = Value::object();
    if (offset) {
        v.set("offset", Value::integer(*offset));
    }
    if (limit) {
        v.set("limit", Value::integer(*limit));
    }
    return v;
}
} // namespace

namespace {
Value str_list(std::initializer_list<const char*> xs) {
    Value::List v;
    for (const char* x : xs) {
        v.push_back(Value::str(x));
    }
    return Value::list(std::move(v));
}
} // namespace

TEST_CASE("task spec: a valid check spec maps argv, root cwd, and read bases") {
    Value spec = Value::object();
    spec.set("argv", str_list({"ctest", "--test-dir", "build"}));
    auto built = runtime::build_task_spec(spec, "/work/ws");
    REQUIRE(built.has_value());
    CHECK(built->argv == std::vector<std::string>{"ctest", "--test-dir", "build"});
    CHECK(built->cwd == fs::path("/work/ws"));            // default cwd == workspace root
    CHECK(built->write_paths.empty());                    // a pure check writes nothing
    // read bases always include the workspace root (plus system toolchain dirs).
    CHECK(std::find(built->read_paths.begin(), built->read_paths.end(), fs::path("/work/ws")) !=
          built->read_paths.end());
    CHECK(built->timeout_ms > 0);
    CHECK(built->output_cap > 0);
}

TEST_CASE("task spec: cwd and writable subtrees are confined under the workspace") {
    Value spec = Value::object();
    spec.set("argv", str_list({"cmake", "--build", "build"}));
    spec.set("cwd", Value::str("workspace"));
    spec.set("writable", str_list({"build"}));
    auto built = runtime::build_task_spec(spec, "/work/ws");
    REQUIRE(built.has_value());
    CHECK(built->write_paths == std::vector<fs::path>{fs::path("/work/ws/build")});
}

TEST_CASE("task spec: timeout and output cap are read and clamped to ceilings") {
    Value spec = Value::object();
    spec.set("argv", str_list({"true"}));
    spec.set("timeout_ms", Value::integer(10'000'000)); // above the 600s ceiling
    spec.set("output_cap_bytes", Value::integer(99'999'999));
    auto built = runtime::build_task_spec(spec, "/work/ws");
    REQUIRE(built.has_value());
    CHECK(built->timeout_ms == static_cast<int>(runtime::kTaskMaxTimeoutMs));
    CHECK(built->output_cap == runtime::kTaskMaxOutputCap);
}

TEST_CASE("task spec: malformed argv is rejected") {
    Value empty = Value::object();
    empty.set("argv", Value::list({}));
    CHECK_FALSE(runtime::build_task_spec(empty, "/work/ws").has_value());

    Value notstr = Value::object();
    Value::List mixed;
    mixed.push_back(Value::str("cmake"));
    mixed.push_back(Value::integer(7));
    notstr.set("argv", Value::list(std::move(mixed)));
    CHECK_FALSE(runtime::build_task_spec(notstr, "/work/ws").has_value());

    Value missing = Value::object();
    CHECK_FALSE(runtime::build_task_spec(missing, "/work/ws").has_value());
}

TEST_CASE("task spec: a path escape in cwd or writable is rejected") {
    Value bad_cwd = Value::object();
    bad_cwd.set("argv", str_list({"true"}));
    bad_cwd.set("cwd", Value::str("../etc"));
    CHECK_FALSE(runtime::build_task_spec(bad_cwd, "/work/ws").has_value());

    Value bad_w = Value::object();
    bad_w.set("argv", str_list({"true"}));
    bad_w.set("writable", str_list({"../../etc"}));
    CHECK_FALSE(runtime::build_task_spec(bad_w, "/work/ws").has_value());
}

TEST_CASE("task spec: a writable subtree may not be the root or the control plane") {
    // The workspace root would grant write over .astrite; the control plane is
    // off-limits directly. Both must be rejected so a task can't be used to author
    // its own components.
    for (const char* w : {".", "workspace", ".astrite", ".astrite/components"}) {
        Value spec = Value::object();
        spec.set("argv", str_list({"true"}));
        spec.set("writable", str_list({w}));
        CHECK_FALSE(runtime::build_task_spec(spec, "/work/ws").has_value());
    }
    // A proper subtree is still fine.
    Value ok = Value::object();
    ok.set("argv", str_list({"cmake", "--build", "build"}));
    ok.set("writable", str_list({"build"}));
    CHECK(runtime::build_task_spec(ok, "/work/ws").has_value());
}

TEST_CASE("task spec: net access is rejected for now (deferred)") {
    Value spec = Value::object();
    spec.set("argv", str_list({"true"}));
    spec.set("net", Value::boolean(true));
    CHECK_FALSE(runtime::build_task_spec(spec, "/work/ws").has_value());
}

TEST_CASE("task result: status derives from spawn/exit/timeout") {
    sandbox::TaskResult passed;
    passed.spawned = true;
    passed.code = 0;
    passed.out = "ok\n";
    CHECK(*runtime::task_result_value(passed).get("status") == Value::str("passed"));
    CHECK(*runtime::task_result_value(passed).get("stdout") == Value::str("ok\n"));
    CHECK(*runtime::task_result_value(passed).get("code") == Value::integer(0));

    sandbox::TaskResult failed;
    failed.spawned = true;
    failed.code = 1;
    failed.err = "boom";
    CHECK(*runtime::task_result_value(failed).get("status") == Value::str("failed"));
    CHECK(*runtime::task_result_value(failed).get("stderr") == Value::str("boom"));

    sandbox::TaskResult to;
    to.spawned = true;
    to.code = -9;
    to.timed_out = true;
    CHECK(*runtime::task_result_value(to).get("status") == Value::str("timed_out"));

    sandbox::TaskResult nospawn;
    nospawn.spawned = false;
    nospawn.spawn_error = "task: exec failed";
    Value v = runtime::task_result_value(nospawn);
    CHECK(*v.get("status") == Value::str("error"));
    CHECK(*v.get("error") == Value::str("task: exec failed"));
}

TEST_CASE("fs.read range spec: absent / zero range means whole file (empty spec)") {
    CHECK(*runtime::read_range_spec(Value::object()) == "");
    CHECK(*runtime::read_range_spec(with_range(0, 0)) == "");
    CHECK(*runtime::read_range_spec(with_range(std::nullopt, std::nullopt)) == "");
}

TEST_CASE("fs.read range spec: offset and/or limit format as offset:limit") {
    CHECK(*runtime::read_range_spec(with_range(2, 3)) == "2:3");
    CHECK(*runtime::read_range_spec(with_range(5, std::nullopt)) == "5:0"); // to end
    CHECK(*runtime::read_range_spec(with_range(std::nullopt, 10)) == "1:10"); // from start
}

TEST_CASE("fs.read range spec: negatives clamp, not error") {
    CHECK(*runtime::read_range_spec(with_range(-4, 2)) == "1:2");
    CHECK(*runtime::read_range_spec(with_range(3, -1)) == "3:0");
}

TEST_CASE("fs.read range spec: a wrong-typed offset/limit is rejected") {
    Value bad = Value::object();
    bad.set("offset", Value::str("nope"));
    CHECK_FALSE(runtime::read_range_spec(bad).has_value());

    Value bad2 = Value::object();
    bad2.set("limit", Value::real(2.5));
    CHECK_FALSE(runtime::read_range_spec(bad2).has_value());
}

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
                      llm::StreamAccumulator::DeltaSink on_delta, std::stop_token)
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
    chat.script.push_back({"", {{"call_1", "echo", R"({"text": "hi"})"}}, "tool_calls", {}});
    chat.script.push_back({"Echoed it for you.", {}, "stop", {}});

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

namespace {
// A workspace with one multi-line file, plus a Runtime that ships the real
// embedded fs_read component so a scripted tool call exercises the whole chain:
// args -> RefDefault -> fs.read effect -> read_range_spec -> confined helper ->
// slice -> dispatch content.
struct ReadFixture {
    fs::path ws;
    fs::path session;
    explicit ReadFixture(std::string tag) {
        ws = fs::temp_directory_path() / ("astrite_rt_read_" + tag);
        fs::remove_all(ws);
        fs::create_directories(ws);
        std::ofstream(ws / "f.txt") << "one\ntwo\nthree\nfour\nfive";
        session = temp_session();
    }
    ~ReadFixture() {
        fs::remove_all(ws);
        fs::remove(session);
    }
    runtime::Options options(runtime::ChatFn chat) {
        runtime::Options opts = test_options(session, std::move(chat));
        opts.workspace_root = ws;
        auto fsr = load_component_jsonc(components::FS_READ_JSONC);
        REQUIRE(fsr.has_value());
        opts.components.emplace("fs_read", std::move(*fsr));
        return opts;
    }
};

std::string tool_result(const fs::path& session_file) {
    runtime::Session session(session_file);
    for (const Value& m : *session.read()->get("messages")->as_list()) {
        const Value* role = m.get("role");
        if (role != nullptr && *role == Value::str("tool")) {
            return *m.get("content")->as_str();
        }
    }
    return "<no tool message>";
}
} // namespace

TEST_CASE("fs.read tool: an offset/limit line range flows end to end") {
    ReadFixture fx("range");
    ScriptedChat chat;
    chat.script.push_back(
        {"", {{"c1", "fs_read", R"({"path":"f.txt","offset":2,"limit":2})"}}, "tool_calls", {}});
    chat.script.push_back({"done", {}, "stop", {}});

    auto rt = runtime::Runtime::create(fx.options(chat.fn()));
    REQUIRE(rt.has_value());
    REQUIRE((*rt)->submit_user_message("read lines 2-3"));
    CHECK(tool_result(fx.session) == "two\nthree\n");
}

TEST_CASE("fs.read tool: omitting offset/limit reads the whole file (back-compat)") {
    ReadFixture fx("full");
    ScriptedChat chat;
    chat.script.push_back({"", {{"c1", "fs_read", R"({"path":"f.txt"})"}}, "tool_calls", {}});
    chat.script.push_back({"done", {}, "stop", {}});

    auto rt = runtime::Runtime::create(fx.options(chat.fn()));
    REQUIRE(rt.has_value());
    REQUIRE((*rt)->submit_user_message("read it all"));
    CHECK(tool_result(fx.session) == "one\ntwo\nthree\nfour\nfive");
}

TEST_CASE("fs.read tool: a wrong-typed offset surfaces as recoverable tool content") {
    ReadFixture fx("badarg");
    ScriptedChat chat;
    chat.script.push_back(
        {"", {{"c1", "fs_read", R"({"path":"f.txt","offset":"nope"})"}}, "tool_calls", {}});
    chat.script.push_back({"done", {}, "stop", {}});

    auto rt = runtime::Runtime::create(fx.options(chat.fn()));
    REQUIRE(rt.has_value());
    REQUIRE((*rt)->submit_user_message("bad offset"));
    CHECK(tool_result(fx.session).find("`offset` must be an integer") != std::string::npos);
}

namespace {
// A check-only task component: pinned argv, no holes. printf is a real tool in
// the system bases run_task grants, so this exercises the whole chain.
constexpr std::string_view SAY_TASK_JSONC = R"({
  "component": "say",
  "kind": "task",
  "caps": ["task.run"],
  "description": "Print a fixed greeting.",
  "task": {"argv": ["printf", "hello from task"], "cwd": "workspace", "effect": "check"}
})";

runtime::Options say_options(const fs::path& session, const fs::path& ws, runtime::ChatFn chat) {
    runtime::Options opts = test_options(session, std::move(chat));
    opts.workspace_root = ws;
    auto say = load_component_jsonc(SAY_TASK_JSONC);
    REQUIRE(say.has_value());
    opts.components.emplace("say", std::move(*say));
    return opts;
}
} // namespace

TEST_CASE("fs.write tool: the model cannot write the control plane (.astrite)") {
    fs::path file = temp_session();
    fs::path ws = fs::temp_directory_path() / "astrite_cp_e2e";
    fs::remove_all(ws);
    fs::create_directories(ws);

    ScriptedChat chat;
    chat.script.push_back(
        {"",
         {{"c1", "fs_write",
           R"({"path":".astrite/components/evil.json","content":"{\"component\":\"evil\"}"})"}},
         "tool_calls", {}});
    chat.script.push_back({"blocked", {}, "stop", {}});

    runtime::Options opts = test_options(file, chat.fn());
    opts.workspace_root = ws;
    auto fsw = load_component_jsonc(components::FS_WRITE_JSONC);
    REQUIRE(fsw.has_value());
    opts.components.emplace("fs_write", std::move(*fsw));

    auto rt = runtime::Runtime::create(std::move(opts));
    REQUIRE(rt.has_value());
    REQUIRE((*rt)->submit_user_message("write a component"));
    CHECK(tool_result(file).find(".astrite") != std::string::npos); // rejected, reason surfaced
    CHECK_FALSE(fs::exists(ws / ".astrite" / "components" / "evil.json"));
    fs::remove_all(ws);
    fs::remove(file);
}

TEST_CASE("component.propose: a valid candidate is staged and announced, not installed") {
    fs::path file = temp_session();
    fs::path ws = fs::temp_directory_path() / "astrite_propose_e2e";
    fs::remove_all(ws);
    fs::create_directories(ws);

    // The model drafts a zero-cap echo-like tool and proposes it.
    const std::string candidate =
        R"({\"component\":\"greet\",\"kind\":\"tool\",\"caps\":[],)"
        R"(\"tree\":{\"do\":\"obj.build\",\"with\":{\"hi\":{\"ref\":\"input.name\"}},\"as\":\"o\"},)"
        R"(\"output\":{\"ref\":\"o\"}})";
    ScriptedChat chat;
    chat.script.push_back(
        {"", {{"c1", "component_propose", R"({"source":")" + candidate + R"("})"}}, "tool_calls",
         {}});
    chat.script.push_back({"proposed it", {}, "stop", {}});

    runtime::Options opts = test_options(file, chat.fn());
    opts.workspace_root = ws;
    auto cp = load_component_jsonc(components::COMPONENT_PROPOSE_JSONC);
    REQUIRE(cp.has_value());
    opts.components.emplace("component_propose", std::move(*cp));

    auto rt = runtime::Runtime::create(std::move(opts));
    REQUIRE(rt.has_value());
    REQUIRE((*rt)->submit_user_message("propose a greet tool"));

    // A ComponentProposed event was emitted naming the candidate.
    auto events = drain(**rt);
    CHECK(count_of<runtime::ComponentProposed>(events) == 1);
    bool found = false;
    for (const auto& e : events) {
        if (const auto* p = std::get_if<runtime::ComponentProposed>(&e)) {
            CHECK(p->name == "greet");
            CHECK(p->risk == "none");
            CHECK_FALSE(p->replaces_existing);
            found = true;
        }
    }
    CHECK(found);

    // It was staged for review — NOT installed into the live components dir.
    auto staged = runtime::read_staged_proposal(ws, "greet");
    REQUIRE(staged.has_value());
    CHECK(staged->find("greet") != std::string::npos);
    CHECK_FALSE(fs::exists(ws / ".astrite" / "components" / "greet.json"));

    // The tool result tells the model it is staged, pending review.
    CHECK(tool_result(file).find("staged") != std::string::npos);
    fs::remove_all(ws);
    fs::remove(file);
}

TEST_CASE("component.propose: an invalid candidate reports errors, stages nothing") {
    fs::path file = temp_session();
    fs::path ws = fs::temp_directory_path() / "astrite_propose_bad";
    fs::remove_all(ws);
    fs::create_directories(ws);

    ScriptedChat chat;
    chat.script.push_back(
        {"", {{"c1", "component_propose", R"({"source":"{ not a component"})"}}, "tool_calls", {}});
    chat.script.push_back({"nope", {}, "stop", {}});

    runtime::Options opts = test_options(file, chat.fn());
    opts.workspace_root = ws;
    auto cp = load_component_jsonc(components::COMPONENT_PROPOSE_JSONC);
    REQUIRE(cp.has_value());
    opts.components.emplace("component_propose", std::move(*cp));

    auto rt = runtime::Runtime::create(std::move(opts));
    REQUIRE(rt.has_value());
    REQUIRE((*rt)->submit_user_message("propose garbage"));

    auto events = drain(**rt);
    CHECK(count_of<runtime::ComponentProposed>(events) == 0); // nothing announced
    CHECK(runtime::list_staged_proposals(ws).empty());        // nothing staged
    fs::remove_all(ws);
    fs::remove(file);
}

namespace {
constexpr std::string_view GREET_SRC = R"({
  "component": "greet", "kind": "tool", "caps": [],
  "description": "Greet by name.",
  "tree": {"do": "obj.build", "with": {"hi": {"ref": "input.name"}}, "as": "o"},
  "output": {"ref": "o"}
})";
} // namespace

TEST_CASE("accept_proposal installs a staged component and pins a trust record") {
    fs::path file = temp_session();
    fs::path ws = fs::temp_directory_path() / "astrite_accept";
    fs::remove_all(ws);
    fs::create_directories(ws);
    REQUIRE(runtime::stage_proposal(ws, "greet", std::string(GREET_SRC)));

    ScriptedChat chat;
    runtime::Options opts = test_options(file, chat.fn());
    opts.workspace_root = ws;
    auto rt = runtime::Runtime::create(std::move(opts));
    REQUIRE(rt.has_value());

    CHECK((*rt)->pending_proposals() == std::vector<std::string>{"greet"});

    auto summary = (*rt)->accept_proposal("greet");
    REQUIRE(summary.has_value());
    CHECK(summary->name == "greet");
    CHECK(summary->kind == "tool");
    CHECK(summary->risk == "none");

    // The kernel wrote the component file and a hash-pinned trust record.
    CHECK(fs::exists(ws / ".astrite" / "components" / "greet.json"));
    auto rec = runtime::read_trust_record(ws / ".astrite" / "trust", "greet");
    REQUIRE(rec.has_value());
    CHECK(rec->sha256 == astrite::util::sha256_hex(std::string(GREET_SRC)));
    CHECK(runtime::evaluate_trust(std::string(GREET_SRC), rec) == runtime::TrustState::trusted);

    // Staging is cleared; nothing left pending.
    CHECK((*rt)->pending_proposals().empty());
    CHECK_FALSE(runtime::read_staged_proposal(ws, "greet").has_value());
    fs::remove_all(ws);
    fs::remove(file);
}

TEST_CASE("accept_proposal installs live: the accepted tool is immediately callable") {
    fs::path file = temp_session();
    fs::path ws = fs::temp_directory_path() / "astrite_accept_live";
    fs::remove_all(ws);
    fs::create_directories(ws);
    REQUIRE(runtime::stage_proposal(ws, "greet", std::string(GREET_SRC)));

    ScriptedChat chat;
    chat.script.push_back({"", {{"c1", "greet", R"({"name":"bob"})"}}, "tool_calls", {}});
    chat.script.push_back({"done", {}, "stop", {}});

    runtime::Options opts = test_options(file, chat.fn());
    opts.workspace_root = ws;
    auto rt = runtime::Runtime::create(std::move(opts));
    REQUIRE(rt.has_value());
    REQUIRE((*rt)->accept_proposal("greet").has_value());

    REQUIRE((*rt)->submit_user_message("greet bob"));
    // The just-accepted component handled the call.
    runtime::Session session(file);
    bool saw = false;
    for (const Value& m : *session.read()->get("messages")->as_list()) {
        const Value* role = m.get("role");
        if (role != nullptr && *role == Value::str("tool")) {
            CHECK(m.get("content")->as_str()->find("bob") != std::string::npos);
            saw = true;
        }
    }
    CHECK(saw);
    fs::remove_all(ws);
    fs::remove(file);
}

TEST_CASE("accept_proposal rejects an unknown or invalid proposal, installing nothing") {
    fs::path file = temp_session();
    fs::path ws = fs::temp_directory_path() / "astrite_accept_bad";
    fs::remove_all(ws);
    fs::create_directories(ws);
    REQUIRE(runtime::stage_proposal(ws, "broken", "{ not a component"));

    ScriptedChat chat;
    runtime::Options opts = test_options(file, chat.fn());
    opts.workspace_root = ws;
    auto rt = runtime::Runtime::create(std::move(opts));
    REQUIRE(rt.has_value());

    CHECK_FALSE((*rt)->accept_proposal("does_not_exist").has_value());
    CHECK_FALSE((*rt)->accept_proposal("broken").has_value()); // staged but invalid
    CHECK_FALSE(fs::exists(ws / ".astrite" / "components" / "broken.json"));

    // reject discards the staged file.
    CHECK((*rt)->reject_proposal("broken"));
    CHECK((*rt)->pending_proposals().empty());
    fs::remove_all(ws);
    fs::remove(file);
}

TEST_CASE("task components are model-callable tools (with an empty arg schema)") {
    fs::path file = temp_session();
    bool saw_say = false;
    runtime::Options opts = say_options(
        file, fs::temp_directory_path(),
        [&](const Value&, const Value& tools, llm::StreamAccumulator::DeltaSink,
            std::stop_token) -> std::expected<llm::CompletedTurn, std::string> {
            for (const Value& def : *tools.as_list()) {
                if (*def.get("function")->get("name") == Value::str("say")) {
                    saw_say = true;
                    CHECK(def.get("function")->get("parameters") != nullptr);
                }
            }
            return llm::CompletedTurn{"ok", {}, "stop", {}};
        });
    auto rt = runtime::Runtime::create(std::move(opts));
    REQUIRE(rt.has_value());
    REQUIRE((*rt)->submit_user_message("x"));
    CHECK(saw_say);
    fs::remove(file);
}

TEST_CASE("task component: a tool call runs the confined task, structured result returns") {
    fs::path file = temp_session();
    fs::path ws = fs::temp_directory_path() / "astrite_task_e2e";
    fs::remove_all(ws);
    fs::create_directories(ws);

    ScriptedChat chat;
    chat.script.push_back({"", {{"c1", "say", "{}"}}, "tool_calls", {}});
    chat.script.push_back({"ran it", {}, "stop", {}});

    auto rt = runtime::Runtime::create(say_options(file, ws, chat.fn()));
    REQUIRE(rt.has_value());
    REQUIRE((*rt)->submit_user_message("run say"));
    std::string content = tool_result(file);
    CHECK(content.find("passed") != std::string::npos);          // status: passed
    CHECK(content.find("hello from task") != std::string::npos); // captured stdout
    fs::remove_all(ws);
    fs::remove(file);
}

TEST_CASE("transcript continuity across submits; system prompt seeded exactly once") {
    fs::path file = temp_session();
    ScriptedChat chat;
    chat.script.push_back({"first answer", {}, "stop", {}});
    chat.script.push_back({"second answer", {}, "stop", {}});

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
        file, [](const Value&, const Value&, llm::StreamAccumulator::DeltaSink, std::stop_token)
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
        file, [&](const Value&, const Value& tools, llm::StreamAccumulator::DeltaSink,
                  std::stop_token) -> std::expected<llm::CompletedTurn, std::string> {
            const Value::List* list = tools.as_list();
            REQUIRE(list != nullptr);
            REQUIRE(list->size() == 1); // echo only; `turn` is an agent, not a tool
            const Value& def = (*list)[0];
            CHECK(*def.get("type") == Value::str("function"));
            CHECK(*def.get("function")->get("name") == Value::str("echo"));
            CHECK(def.get("function")->get("parameters") != nullptr);
            saw_tools = true;
            return llm::CompletedTurn{"ok", {}, "stop", {}};
        }));
    REQUIRE(rt.has_value());
    REQUIRE((*rt)->submit_user_message("x"));
    CHECK(saw_tools);
    fs::remove(file);
}
