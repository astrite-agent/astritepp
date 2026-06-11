#include <doctest.h>

#include <string>
#include <vector>

#include "kernel/eval.hpp"
#include "kernel/prims.hpp"
#include "schema/value_json.hpp"

using namespace astrite;
using namespace astrite::schema;
using kernel::Blackboard;
using kernel::ComponentMap;
using kernel::Context;
using kernel::Registry;
using kernel::Status;
using kernel::walk;

namespace {

struct Fixture {
    Registry reg = kernel::default_registry();
    ComponentMap components;
    Context ctx{&reg, &components, nullptr};
    Blackboard bb;

    // A counting primitive: every call appends its `tag` arg to calls.
    std::vector<std::string> calls;
    Fixture() {
        reg.add("echo", [](const Value& args) -> std::expected<Value, kernel::KernelError> {
            return args;
        });
        reg.add("fail", [](const Value&) -> std::expected<Value, kernel::KernelError> {
            return std::unexpected(kernel::KernelError{"fail primitive always errors"});
        });
        reg.add("count", [this](const Value& args) -> std::expected<Value, kernel::KernelError> {
            const std::string* tag = args.get("tag") ? args.get("tag")->as_str() : nullptr;
            calls.push_back(tag ? *tag : "?");
            return Value();
        });
        // A capability-bearing effect so component.check can report reached/risk.
        reg.add_effect("fs.write", "fs.write",
                       [](const Value&) -> std::expected<Value, kernel::KernelError> {
                           return Value();
                       });
    }
};

Node action(std::string prim, ArgMap with = {}, std::optional<std::string> bind = {}) {
    return Node{Action{std::move(prim), std::move(with), std::move(bind)}};
}

Node condition(std::string pred, ArgMap with = {}) {
    return Node{Condition{std::move(pred), std::move(with)}};
}

ArgMap args1(std::string key, ArgValue v) {
    ArgMap m;
    m.emplace(std::move(key), std::move(v));
    return m;
}

} // namespace

TEST_CASE("action resolves args, invokes the primitive, and binds the result") {
    Fixture f;
    f.bb.bind("x", Value::integer(7));
    Node n = action("echo", args1("a", ArgValue{Ref{"x"}}), "out");
    auto s = walk(n, f.bb, f.ctx);
    REQUIRE(s.has_value());
    CHECK(*s == Status::success);
    REQUIRE(f.bb.get("out") != nullptr);
    CHECK(*f.bb.get("out")->get("a") == Value::integer(7));
}

TEST_CASE("component.catalog lists the loaded components, sorted") {
    Fixture f;
    Component a;
    a.name = "alpha";
    a.kind = "tool";
    a.caps = {"fs.read"};
    a.description = "reads files";
    Component b;
    b.name = "beta";
    b.kind = "task";
    b.caps = {"task.run"};
    f.components.emplace("alpha", std::move(a));
    f.components.emplace("beta", std::move(b));

    REQUIRE(walk(action("component.catalog", {}, "out"), f.bb, f.ctx).has_value());
    const Value* list = f.bb.get("out")->get("components");
    REQUIRE(list != nullptr);
    REQUIRE(list->as_list() != nullptr);
    REQUIRE(list->as_list()->size() == 2);
    CHECK(*(*list->as_list())[0].get("name") == Value::str("alpha"));
    CHECK(*(*list->as_list())[0].get("kind") == Value::str("tool"));
    CHECK(*(*list->as_list())[0].get("description") == Value::str("reads files"));
    CHECK(*(*list->as_list())[1].get("name") == Value::str("beta"));
}

TEST_CASE("component.check validates a good candidate and reports its authority") {
    Fixture f;
    const std::string src = R"({"component":"w","kind":"tool","caps":["fs.write"],
      "tree":{"do":"fs.write"},"output":{"ref":"x"}})";
    REQUIRE(walk(action("component.check", args1("source", ArgValue{Value::str(src)}), "out"), f.bb,
                 f.ctx)
                .has_value());
    const Value* out = f.bb.get("out");
    REQUIRE(out != nullptr);
    CHECK(*out->get("ok") == Value::boolean(true));
    CHECK(*out->get("name") == Value::str("w"));
    CHECK(*out->get("kind") == Value::str("tool"));
    CHECK(*out->get("risk") == Value::str("write"));
    REQUIRE(out->get("reached")->as_list() != nullptr);
    REQUIRE(out->get("reached")->as_list()->size() == 1);
    CHECK((*out->get("reached")->as_list())[0] == Value::str("fs.write"));
}

TEST_CASE("component.check reports malformed source and undeclared authority as ok:false") {
    Fixture f;
    REQUIRE(walk(action("component.check", args1("source", ArgValue{Value::str("{ not json")}),
                        "bad"),
                 f.bb, f.ctx)
                .has_value());
    CHECK(*f.bb.get("bad")->get("ok") == Value::boolean(false));
    REQUIRE(f.bb.get("bad")->get("errors") != nullptr);

    // reaches fs.write but declares no caps — the fold rejects it
    const std::string undeclared = R"({"component":"w","kind":"tool","caps":[],
      "tree":{"do":"fs.write"},"output":{"ref":"x"}})";
    REQUIRE(walk(action("component.check", args1("source", ArgValue{Value::str(undeclared)}),
                        "u"),
                 f.bb, f.ctx)
                .has_value());
    CHECK(*f.bb.get("u")->get("ok") == Value::boolean(false));
}

TEST_CASE("action without bind leaves the blackboard untouched") {
    Fixture f;
    auto s = walk(action("echo"), f.bb, f.ctx);
    REQUIRE(s.has_value());
    CHECK(*s == Status::success);
    CHECK(f.bb.get("out") == nullptr);
}

TEST_CASE("unknown primitive is a kernel error") {
    Fixture f;
    auto s = walk(action("nope"), f.bb, f.ctx);
    REQUIRE_FALSE(s.has_value());
    CHECK(s.error().message.find("nope") != std::string::npos);
}

TEST_CASE("conditions are strict: bool decides, non-bool is an error") {
    Fixture f;
    auto yes = walk(condition("bool.is_true", args1("in", ArgValue{Value::boolean(true)})), f.bb,
                    f.ctx);
    REQUIRE(yes.has_value());
    CHECK(*yes == Status::success);

    auto no = walk(condition("bool.is_true", args1("in", ArgValue{Value::boolean(false)})), f.bb,
                   f.ctx);
    REQUIRE(no.has_value());
    CHECK(*no == Status::failure);

    // echo returns an object — never a valid guard
    auto bad = walk(condition("echo", args1("in", ArgValue{Value::integer(1)})), f.bb, f.ctx);
    CHECK_FALSE(bad.has_value());
}

TEST_CASE("all is ordered AND with fail-fast") {
    Fixture f;
    Node n{All{{action("count", args1("tag", ArgValue{Value::str("a")})),
                condition("bool.is_true", args1("in", ArgValue{Value::boolean(false)})),
                action("count", args1("tag", ArgValue{Value::str("never")}))}}};
    auto s = walk(n, f.bb, f.ctx);
    REQUIRE(s.has_value());
    CHECK(*s == Status::failure);
    CHECK(f.calls == std::vector<std::string>{"a"}); // third child never ran

    Node ok{All{{action("count", args1("tag", ArgValue{Value::str("b")})),
                 action("count", args1("tag", ArgValue{Value::str("c")}))}}};
    auto s2 = walk(ok, f.bb, f.ctx);
    REQUIRE(s2.has_value());
    CHECK(*s2 == Status::success);
    CHECK(f.calls == std::vector<std::string>{"a", "b", "c"});
}

TEST_CASE("any is ordered OR: first success wins, all-fail fails") {
    Fixture f;
    Node n{Any{{condition("bool.is_true", args1("in", ArgValue{Value::boolean(false)})),
                action("count", args1("tag", ArgValue{Value::str("won")})),
                action("count", args1("tag", ArgValue{Value::str("never")}))}}};
    auto s = walk(n, f.bb, f.ctx);
    REQUIRE(s.has_value());
    CHECK(*s == Status::success);
    CHECK(f.calls == std::vector<std::string>{"won"});

    Node allfail{Any{{condition("bool.is_true", args1("in", ArgValue{Value::boolean(false)}))}}};
    auto s2 = walk(allfail, f.bb, f.ctx);
    REQUIRE(s2.has_value());
    CHECK(*s2 == Status::failure);
}

TEST_CASE("errors propagate through composites immediately") {
    Fixture f;
    Node n{All{{action("fail"), action("count", args1("tag", ArgValue{Value::str("never")}))}}};
    CHECK_FALSE(walk(n, f.bb, f.ctx).has_value());
    CHECK(f.calls.empty());
}

TEST_CASE("foreach binds each element in a scoped slot and restores the prior binding") {
    Fixture f;
    f.bb.bind("xs", Value::list({Value::str("p"), Value::str("q")}));
    f.bb.bind("item", Value::str("prior"));
    Node n{Foreach{ArgValue{Ref{"xs"}}, "item",
                   box<Node>(action("count", args1("tag", ArgValue{Ref{"item"}})))}};
    auto s = walk(n, f.bb, f.ctx);
    REQUIRE(s.has_value());
    CHECK(*s == Status::success);
    CHECK(f.calls == std::vector<std::string>{"p", "q"});
    REQUIRE(f.bb.get("item") != nullptr);
    CHECK(*f.bb.get("item") == Value::str("prior")); // restored

    // Without a prior binding the slot is removed afterwards.
    f.bb.remove("item");
    REQUIRE(walk(n, f.bb, f.ctx).has_value());
    CHECK(f.bb.get("item") == nullptr);
}

TEST_CASE("foreach ignores body status but propagates errors; over must be a list") {
    Fixture f;
    f.bb.bind("xs", Value::list({Value::integer(1)}));
    Node failing{Foreach{ArgValue{Ref{"xs"}}, "i",
                         box<Node>(condition("bool.is_true",
                                             args1("in", ArgValue{Value::boolean(false)})))}};
    auto s = walk(failing, f.bb, f.ctx);
    REQUIRE(s.has_value());
    CHECK(*s == Status::success); // body Failure ignored

    Node erroring{Foreach{ArgValue{Ref{"xs"}}, "i", box<Node>(action("fail"))}};
    CHECK_FALSE(walk(erroring, f.bb, f.ctx).has_value());

    f.bb.bind("notalist", Value::integer(3));
    Node bad{Foreach{ArgValue{Ref{"notalist"}}, "i", box<Node>(action("echo"))}};
    CHECK_FALSE(walk(bad, f.bb, f.ctx).has_value());
}

TEST_CASE("foreach caps pathologically large collections at 1024") {
    Fixture f;
    Value::List big(1500, Value::integer(0));
    f.bb.bind("xs", Value::list(std::move(big)));
    Node n{Foreach{ArgValue{Ref{"xs"}}, "i",
                   box<Node>(action("count", args1("tag", ArgValue{Value::str("t")})))}};
    REQUIRE(walk(n, f.bb, f.ctx).has_value());
    CHECK(f.calls.size() == 1024);
}

TEST_CASE("repeat walks body then until, stopping on until-success") {
    Fixture f;
    int rounds = 0;
    f.reg.add("tick", [&](const Value&) -> std::expected<Value, kernel::KernelError> {
        ++rounds;
        return Value();
    });
    f.reg.add("done_yet", [&](const Value&) -> std::expected<Value, kernel::KernelError> {
        return Value::boolean(rounds >= 3);
    });
    Node n{Repeat{box<Node>(action("tick")), box<Node>(condition("done_yet")), 16}};
    auto s = walk(n, f.bb, f.ctx);
    REQUIRE(s.has_value());
    CHECK(*s == Status::success);
    CHECK(rounds == 3);
}

TEST_CASE("repeat bounded out by max is still success") {
    Fixture f;
    int rounds = 0;
    f.reg.add("tick", [&](const Value&) -> std::expected<Value, kernel::KernelError> {
        ++rounds;
        return Value();
    });
    Node n{Repeat{box<Node>(action("tick")),
                  box<Node>(condition("bool.is_true", args1("in", ArgValue{Value::boolean(false)}))),
                  5}};
    auto s = walk(n, f.bb, f.ctx);
    REQUIRE(s.has_value());
    CHECK(*s == Status::success);
    CHECK(rounds == 5);
}

namespace {
Component greet_tool() {
    Component c;
    c.name = "greet";
    c.kind = "tool";
    c.caps = {};
    c.tree = Node{Action{"obj.build", args1("msg", ArgValue{Ref{"input.who"}}), "out"}};
    c.output = ArgValue{Ref{"out"}};
    return c;
}
} // namespace

TEST_CASE("dispatch resolves the tool, seeds input, and returns id+content") {
    Fixture f;
    f.components.emplace("greet", greet_tool());
    Value call = Value::object();
    call.set("id", Value::str("call_1"));
    call.set("name", Value::str("greet"));
    call.set("arguments", Value::str(R"({"who": "eve"})"));
    f.bb.bind("tc", call);

    Node n = action("dispatch", args1("call", ArgValue{Ref{"tc"}}), "result");
    auto s = walk(n, f.bb, f.ctx);
    REQUIRE(s.has_value());
    const Value* result = f.bb.get("result");
    REQUIRE(result != nullptr);
    CHECK(*result->get("tool_call_id") == Value::str("call_1"));
    // Non-string output is JSON-stringified.
    auto content = parse_json(*result->get("content")->as_str());
    REQUIRE(content.has_value());
    CHECK(*content->get("msg") == Value::str("eve"));
}

TEST_CASE("dispatch: unknown tool and tool errors come back as content, not kernel errors") {
    Fixture f;
    Value call = Value::object();
    call.set("id", Value::str("c2"));
    call.set("name", Value::str("nope"));
    f.bb.bind("tc", call);
    Node n = action("dispatch", args1("call", ArgValue{Ref{"tc"}}), "r");
    REQUIRE(walk(n, f.bb, f.ctx).has_value());
    CHECK(f.bb.get("r")->get("content")->as_str()->find("unknown tool") != std::string::npos);

    // A tool whose walk errors also reports as content.
    Component broken;
    broken.name = "broken";
    broken.kind = "tool";
    broken.tree = Node{Action{"fail", {}, {}}};
    f.components.emplace("broken", std::move(broken));
    Value call2 = Value::object();
    call2.set("id", Value::str("c3"));
    call2.set("name", Value::str("broken"));
    f.bb.bind("tc2", call2);
    Node n2 = action("dispatch", args1("call", ArgValue{Ref{"tc2"}}), "r2");
    REQUIRE(walk(n2, f.bb, f.ctx).has_value());
    CHECK(f.bb.get("r2")->get("content")->as_str()->find("error") != std::string::npos);
}

TEST_CASE("dispatch: corrupt envelope is a hard error; bad arguments degrade to {}") {
    Fixture f;
    f.components.emplace("greet", greet_tool());

    Value no_name = Value::object();
    no_name.set("id", Value::str("c4"));
    f.bb.bind("tc", no_name);
    CHECK_FALSE(walk(action("dispatch", args1("call", ArgValue{Ref{"tc"}}), "r"), f.bb, f.ctx)
                    .has_value());

    // Unparseable arguments → input {} → tool's missing ref reports as content error.
    Value bad_args = Value::object();
    bad_args.set("id", Value::str("c5"));
    bad_args.set("name", Value::str("greet"));
    bad_args.set("arguments", Value::str("{not json"));
    f.bb.bind("tc2", bad_args);
    REQUIRE(walk(action("dispatch", args1("call", ArgValue{Ref{"tc2"}}), "r2"), f.bb, f.ctx)
                .has_value());
    CHECK(f.bb.get("r2")->get("content")->as_str()->find("error") != std::string::npos);
}

namespace {
struct Recorder : kernel::EvalObserver {
    std::vector<std::string> events;
    void tool_started(std::string_view id, std::string_view name, std::string_view) override {
        events.push_back("start:" + std::string(id) + ":" + std::string(name));
    }
    void tool_finished(std::string_view id, std::string_view name, std::string_view) override {
        events.push_back("finish:" + std::string(id) + ":" + std::string(name));
    }
};
} // namespace

TEST_CASE("dispatch reports tool start/finish to the observer") {
    Fixture f;
    Recorder rec;
    f.ctx.observer = &rec;
    f.components.emplace("greet", greet_tool());
    Value call = Value::object();
    call.set("id", Value::str("c6"));
    call.set("name", Value::str("greet"));
    call.set("arguments", Value::str(R"({"who": "x"})"));
    f.bb.bind("tc", call);
    REQUIRE(walk(action("dispatch", args1("call", ArgValue{Ref{"tc"}}), "r"), f.bb, f.ctx)
                .has_value());
    REQUIRE(rec.events.size() == 2);
    CHECK(rec.events[0] == "start:c6:greet");
    CHECK(rec.events[1] == "finish:c6:greet");
}

TEST_CASE("pure prims: coll.is_empty and obj.build behave strictly") {
    Fixture f;
    auto empty = walk(condition("coll.is_empty", args1("in", ArgValue{Value::list({})})), f.bb,
                      f.ctx);
    REQUIRE(empty.has_value());
    CHECK(*empty == Status::success);

    auto nonempty = walk(
        condition("coll.is_empty", args1("in", ArgValue{Value::list({Value::integer(1)})})), f.bb,
        f.ctx);
    REQUIRE(nonempty.has_value());
    CHECK(*nonempty == Status::failure);

    // Non-list input is an error, not a truthiness guess.
    CHECK_FALSE(
        walk(condition("coll.is_empty", args1("in", ArgValue{Value::integer(0)})), f.bb, f.ctx)
            .has_value());

    Node build = action("obj.build", args1("role", ArgValue{Value::str("tool")}), "msg");
    REQUIRE(walk(build, f.bb, f.ctx).has_value());
    CHECK(*f.bb.get("msg")->get("role") == Value::str("tool"));
}
