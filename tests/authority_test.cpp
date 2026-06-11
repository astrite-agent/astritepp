#include <doctest.h>

#include "kernel/authority.hpp"
#include "kernel/eval.hpp"
#include "kernel/prims.hpp"

using namespace astrite;
using namespace astrite::schema;
using kernel::check_component_authority;
using kernel::Registry;

namespace {

Registry test_registry() {
    Registry reg = kernel::default_registry(); // pure prims: no capability
    auto noop = [](const Value&) -> std::expected<Value, kernel::KernelError> { return Value(); };
    reg.add_effect("fs.edit", "fs.edit", noop);
    reg.add_effect("fs.read", "fs.read", noop);
    return reg;
}

Component component(std::vector<std::string> caps, Node tree) {
    Component c;
    c.name = "t";
    c.kind = "tool";
    c.caps = std::move(caps);
    c.tree = std::move(tree);
    return c;
}

Node act(std::string prim) { return Node{Action{std::move(prim), {}, {}}}; }

} // namespace

TEST_CASE("risk_tier classifies a capability set worst-first") {
    using kernel::risk_tier;
    CHECK(risk_tier({}) == "none");
    CHECK(risk_tier({"fs.read", "session.read"}) == "read");
    CHECK(risk_tier({"fs.read", "fs.write"}) == "write");
    CHECK(risk_tier({"session.append"}) == "write");
    CHECK(risk_tier({"fs.write", "task.run"}) == "exec");
    CHECK(risk_tier({"task.run", "llm.stream"}) == "network");
    CHECK(risk_tier({"http.fetch"}) == "network");
}

TEST_CASE("component.check/catalog are zero-cap builtins the fold accepts") {
    Registry reg = test_registry();
    Component c = component({}, Node{All{{act("component.catalog"), act("component.check")}}});
    auto trace = check_component_authority(c, reg);
    REQUIRE(trace.has_value());
    CHECK(trace->reached.empty()); // recognized, but grant nothing
}

TEST_CASE("pure-only components need no capabilities") {
    Registry reg = test_registry();
    Component c = component({}, Node{All{{act("obj.build"),
                                          Node{Condition{"bool.is_true", {}}}}}});
    auto trace = check_component_authority(c, reg);
    REQUIRE(trace.has_value());
    CHECK(trace->reached.empty());
}

TEST_CASE("declared effect capability passes and is traced") {
    Registry reg = test_registry();
    Component c = component({"fs.edit"}, act("fs.edit"));
    auto trace = check_component_authority(c, reg);
    REQUIRE(trace.has_value());
    CHECK(trace->reached == std::vector<std::string>{"fs.edit"});
}

TEST_CASE("an undeclared reachable capability is rejected, naming the cap") {
    Registry reg = test_registry();
    Component c = component({}, act("fs.edit"));
    auto trace = check_component_authority(c, reg);
    REQUIRE_FALSE(trace.has_value());
    CHECK(trace.error().message.find("fs.edit") != std::string::npos);
}

TEST_CASE("the fold reaches through composites and iteration bodies") {
    Registry reg = test_registry();
    // fs.read buried inside repeat→any→foreach→all
    Node deep{Repeat{
        box<Node>(Node{Any{{Node{Foreach{ArgValue{Ref{"xs"}}, "i",
                                         box<Node>(Node{All{{act("fs.read")}}})}}}}}),
        box<Node>(Node{Condition{"bool.is_true", {}}}), 4}};
    Component undeclared = component({}, deep);
    auto bad = check_component_authority(undeclared, reg);
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().message.find("fs.read") != std::string::npos);

    Component declared = component({"fs.read"}, std::move(undeclared.tree));
    auto ok = check_component_authority(declared, reg);
    REQUIRE(ok.has_value());
    CHECK(ok->reached == std::vector<std::string>{"fs.read"});
}

TEST_CASE("dispatch is a routing capability of its own") {
    Registry reg = test_registry();
    Component router = component({}, act("dispatch"));
    auto bad = check_component_authority(router, reg);
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().message.find("dispatch") != std::string::npos);

    Component ok = component({"dispatch"}, act("dispatch"));
    auto trace = check_component_authority(ok, reg);
    REQUIRE(trace.has_value());
    CHECK(trace->reached == std::vector<std::string>{"dispatch"});
}

TEST_CASE("unknown primitives fail the fold closed") {
    Registry reg = test_registry();
    Component c = component({}, act("totally.unknown"));
    auto trace = check_component_authority(c, reg);
    REQUIRE_FALSE(trace.has_value());
    CHECK(trace.error().message.find("totally.unknown") != std::string::npos);
}

TEST_CASE("declared-but-unreached capabilities are allowed; trace reports reached only") {
    Registry reg = test_registry();
    Component c = component({"fs.edit", "fs.read"}, act("fs.edit"));
    auto trace = check_component_authority(c, reg);
    REQUIRE(trace.has_value());
    CHECK(trace->reached == std::vector<std::string>{"fs.edit"});
}

TEST_CASE("dispatch enforces the selected tool's own authority at dispatch time") {
    Registry reg = test_registry();
    kernel::ComponentMap components;
    // The tool reaches fs.edit but declares nothing.
    components.emplace("sneaky", component({}, act("fs.edit")));
    kernel::Context ctx{&reg, &components, nullptr};
    kernel::Blackboard bb;
    Value call = Value::object();
    call.set("id", Value::str("c1"));
    call.set("name", Value::str("sneaky"));
    bb.bind("tc", call);
    ArgMap with;
    with.emplace("call", ArgValue{Ref{"tc"}});
    Node n{Action{"dispatch", std::move(with), "r"}};

    auto s = kernel::walk(n, bb, ctx);
    REQUIRE(s.has_value()); // recoverable: error comes back as content
    const std::string* content = bb.get("r")->get("content")->as_str();
    REQUIRE(content != nullptr);
    CHECK(content->find("error") != std::string::npos);
    CHECK(content->find("fs.edit") != std::string::npos);
}
