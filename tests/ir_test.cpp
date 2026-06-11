#include <doctest.h>

#include "schema/ir.hpp"
#include "schema/load.hpp"

using namespace astrite::schema;

namespace {
Component must_load(std::string_view jsonc) {
    auto c = load_component_jsonc(jsonc);
    if (!c.has_value()) {
        FAIL(c.error().message);
    }
    return std::move(*c);
}

std::string must_fail(std::string_view jsonc) {
    auto c = load_component_jsonc(jsonc);
    REQUIRE_FALSE(c.has_value());
    return c.error().message;
}
} // namespace

TEST_CASE("a minimal tool component decodes: action leaf, refs, default, bind, output") {
    Component c = must_load(R"({
  // exact-match replacement over a sandboxed primitive
  "component": "fs_edit",
  "kind": "tool",
  "caps": ["fs.edit"],
  "tree": {
    "all": [
      {
        "do": "fs.edit",
        "with": {
          "path": {"ref": "input.path"},
          "expected_replacements": {"ref": "input.expected", "default": 1},
          "mode": "exact"
        },
        "as": "out"
      }
    ]
  },
  "output": {"ref": "out"}
})");
    CHECK(c.name == "fs_edit");
    CHECK(c.kind == "tool");
    REQUIRE(c.caps == std::vector<std::string>{"fs.edit"});

    const All* all = node_as<All>(c.tree);
    REQUIRE(all != nullptr);
    REQUIRE(all->children.size() == 1);
    const Action* act = node_as<Action>(all->children[0]);
    REQUIRE(act != nullptr);
    CHECK(act->primitive == "fs.edit");
    REQUIRE(act->bind.has_value());
    CHECK(*act->bind == "out");

    const ArgValue* path = find_arg(act->with, "path");
    REQUIRE(path != nullptr);
    const Ref* ref = std::get_if<Ref>(&path->v);
    REQUIRE(ref != nullptr);
    CHECK(ref->path == "input.path");

    const ArgValue* exp = find_arg(act->with, "expected_replacements");
    REQUIRE(exp != nullptr);
    const RefDefault* rd = std::get_if<RefDefault>(&exp->v);
    REQUIRE(rd != nullptr);
    CHECK(rd->path == "input.expected");
    CHECK(rd->fallback == Value::integer(1));

    const ArgValue* mode = find_arg(act->with, "mode");
    REQUIRE(mode != nullptr);
    CHECK(std::get_if<Value>(&mode->v) != nullptr);
    CHECK(*std::get_if<Value>(&mode->v) == Value::str("exact"));

    REQUIRE(c.output.has_value());
    const Ref* out_ref = std::get_if<Ref>(&c.output->v);
    REQUIRE(out_ref != nullptr);
    CHECK(out_ref->path == "out");
}

TEST_CASE("all six node kinds decode, repeat.max defaults to 16") {
    Component c = must_load(R"({
  "component": "kitchen_sink",
  "kind": "agent",
  "caps": ["dispatch"],
  "tree": {
    "repeat": {
      "until": {"when": "bool.is_true", "with": {"in": {"ref": "done"}}},
      "body": {
        "any": [
          {"when": "coll.is_empty", "with": {"in": {"ref": "calls"}}},
          {
            "foreach": {
              "over": {"ref": "calls"},
              "as": "tc",
              "body": {
                "all": [
                  {"do": "dispatch", "with": {"call": {"ref": "tc"}}}
                ]
              }
            }
          }
        ]
      }
    }
  }
})");
    const Repeat* rep = node_as<Repeat>(c.tree);
    REQUIRE(rep != nullptr);
    CHECK(rep->max == 16); // default when omitted

    const Condition* until = node_as<Condition>(*rep->until);
    REQUIRE(until != nullptr);
    CHECK(until->predicate == "bool.is_true");

    const Any* any = node_as<Any>(*rep->body);
    REQUIRE(any != nullptr);
    REQUIRE(any->children.size() == 2);
    CHECK(node_as<Condition>(any->children[0]) != nullptr);

    const Foreach* fe = node_as<Foreach>(any->children[1]);
    REQUIRE(fe != nullptr);
    CHECK(fe->item == "tc");
    CHECK(std::get_if<Ref>(&fe->over.v) != nullptr);
    const All* body = node_as<All>(*fe->body);
    REQUIRE(body != nullptr);
    CHECK(node_as<Action>(body->children[0]) != nullptr);
}

TEST_CASE("explicit repeat.max parses") {
    Component c = must_load(R"({
  "component": "bounded", "kind": "agent", "caps": [],
  "tree": {
    "repeat": {
      "max": 3,
      "until": {"when": "bool.is_true", "with": {"in": {"ref": "done"}}},
      "body": {"do": "noop"}
    }
  }
})");
    CHECK(node_as<Repeat>(c.tree)->max == 3);
}

TEST_CASE("literal arguments are typed by JSON syntax") {
    Component c = must_load(R"({
  "component": "lits", "kind": "tool", "caps": [],
  "tree": {
    "do": "obj.build",
    "with": {
      "n": 42,
      "f": 2.5,
      "t": true,
      "s": "true",
      "snum": "42",
      "z": null,
      "nested": {"a": [1, "two"]}
    }
  }
})");
    const Action* act = node_as<Action>(c.tree);
    REQUIRE(act != nullptr);
    auto lit = [&](std::string_view key) -> const Value& {
        const ArgValue* arg = find_arg(act->with, key);
        REQUIRE(arg != nullptr);
        const Value* v = std::get_if<Value>(&arg->v);
        REQUIRE(v != nullptr);
        return *v;
    };
    CHECK(lit("n") == Value::integer(42)); // integral number → Int
    CHECK(lit("f") == Value::real(2.5));   // fractional → Float
    CHECK(lit("t") == Value::boolean(true));
    CHECK(lit("s") == Value::str("true")); // strings are strings by syntax
    CHECK(lit("snum") == Value::str("42"));
    CHECK(lit("z") == Value());

    const Value& nested = lit("nested");
    REQUIRE(nested.is_object());
    const Value* a = nested.get("a");
    REQUIRE(a != nullptr);
    REQUIRE(a->is_list());
    CHECK((*a->as_list())[0] == Value::integer(1));
    CHECK((*a->as_list())[1] == Value::str("two"));
}

TEST_CASE("components and nodes are value types: deep copy, deep equality") {
    Component a =
        must_load(R"({"component": "c", "kind": "tool", "caps": [], "tree": {"do": "x"}})");
    Component b = a;
    CHECK(a.tree == b.tree);
    b.tree = Node{Action{.primitive = "y", .with = {}, .bind = {}}};
    CHECK_FALSE(a.tree == b.tree);
    CHECK(node_as<Action>(a.tree)->primitive == "x"); // a untouched
}

TEST_CASE("unknown top-level component field is rejected") {
    std::string err = must_fail(
        R"({"component": "c", "kind": "tool", "caps": [], "tree": {"do": "x"}, "bogus": 1})");
    CHECK(err.find("bogus") != std::string::npos);
}

TEST_CASE("missing required component fields are rejected") {
    CHECK(must_fail(R"({"kind": "tool", "caps": [], "tree": {"do": "x"}})").find("component") !=
          std::string::npos);
    CHECK(must_fail(R"({"component": "c", "caps": [], "tree": {"do": "x"}})").find("kind") !=
          std::string::npos);
    CHECK(must_fail(R"({"component": "c", "kind": "tool", "caps": []})").find("tree") !=
          std::string::npos);
}

TEST_CASE("a node with zero or two discriminators is rejected") {
    // Wording comes from glaze's variant deduction today (missing_key/unknown_key
    // against the best-matching alternative); a canonical "exactly one of" message
    // is loader polish tracked for M3. The contract is fail-closed rejection.
    CHECK_FALSE(must_fail(R"({
  "component": "c", "kind": "tool", "caps": [],
  "tree": {"with": {"a": 1}}
})")
                    .empty());

    CHECK_FALSE(must_fail(R"({
  "component": "c", "kind": "tool", "caps": [],
  "tree": {"do": "x", "when": "y"}
})")
                    .empty());
}

TEST_CASE("unknown node fields are rejected, including as/with where they make no sense") {
    CHECK_FALSE(must_fail(R"({
  "component": "c", "kind": "tool", "caps": [],
  "tree": {"do": "x", "withx": {}}
})")
                    .empty());

    // `as` is only legal on `do`
    CHECK_FALSE(must_fail(R"({
  "component": "c", "kind": "tool", "caps": [],
  "tree": {"all": [{"do": "x"}], "as": "out"}
})")
                    .empty());

    // `when` takes no `as`
    CHECK_FALSE(must_fail(R"({
  "component": "c", "kind": "tool", "caps": [],
  "tree": {"when": "bool.is_true", "as": "out"}
})")
                    .empty());
}

TEST_CASE("ref maps are strict: ref must be a string, only ref+default allowed") {
    CHECK_FALSE(must_fail(R"({
  "component": "c", "kind": "tool", "caps": [],
  "tree": {"do": "x", "with": {"a": {"ref": [1]}}}
})")
                    .empty());

    CHECK_FALSE(must_fail(R"({
  "component": "c", "kind": "tool", "caps": [],
  "tree": {"do": "x", "with": {"a": {"ref": "p", "bogus": 1}}}
})")
                    .empty());
}

TEST_CASE("foreach and repeat shapes are strict") {
    // foreach missing `as`
    CHECK_FALSE(must_fail(R"({
  "component": "c", "kind": "tool", "caps": [],
  "tree": {"foreach": {"over": {"ref": "xs"}, "body": {"do": "x"}}}
})")
                    .empty());

    // repeat missing `until`
    CHECK_FALSE(must_fail(R"({
  "component": "c", "kind": "tool", "caps": [],
  "tree": {"repeat": {"body": {"do": "x"}}}
})")
                    .empty());

    // repeat.max must be a positive integer
    CHECK_FALSE(must_fail(R"({
  "component": "c", "kind": "tool", "caps": [],
  "tree": {"repeat": {"max": -2, "until": {"when": "p"}, "body": {"do": "x"}}}
})")
                    .empty());
}

TEST_CASE("a task component desugars to a pinned task.run action") {
    Component c = must_load(R"({
  "component": "build",
  "kind": "task",
  "caps": ["task.run"],
  "description": "Build the project.",
  "task": {
    "argv": ["cmake", "--build", "build"],
    "cwd": "workspace",
    "effect": "check",
    "net": false,
    "writable": ["build"],
    "timeout_ms": 600000,
    "output_cap_bytes": 262144
  }
})");
    CHECK(c.kind == "task");
    REQUIRE(c.caps == std::vector<std::string>{"task.run"});

    // Desugared tree: a single {do: "task.run", with: {spec: <task block>}, as: "out"}.
    const Action* act = node_as<Action>(c.tree);
    REQUIRE(act != nullptr);
    CHECK(act->primitive == "task.run");
    REQUIRE(act->bind.has_value());
    CHECK(*act->bind == "out");

    const ArgValue* spec = find_arg(act->with, "spec");
    REQUIRE(spec != nullptr);
    const Value* spec_v = std::get_if<Value>(&spec->v); // the pinned spec is a literal
    REQUIRE(spec_v != nullptr);
    REQUIRE(spec_v->is_object());
    const Value* argv = spec_v->get("argv");
    REQUIRE(argv != nullptr);
    REQUIRE(argv->as_list() != nullptr);
    CHECK(argv->as_list()->size() == 3);
    CHECK((*argv->as_list())[0] == Value::str("cmake"));

    // Output is wired to the bound result.
    REQUIRE(c.output.has_value());
    const Ref* out_ref = std::get_if<Ref>(&c.output->v);
    REQUIRE(out_ref != nullptr);
    CHECK(out_ref->path == "out");
}

TEST_CASE("a task component rejects a tree, and a non-task still requires one") {
    CHECK(must_fail(R"({
  "component": "build", "kind": "task", "caps": ["task.run"],
  "task": {"argv": ["true"]},
  "tree": {"do": "x"}
})")
              .find("tree") != std::string::npos);

    // A task component with no task block is rejected.
    CHECK(must_fail(R"({"component": "build", "kind": "task", "caps": ["task.run"]})")
              .find("task") != std::string::npos);
}

TEST_CASE("a task block is structurally validated at load") {
    // argv must be a non-empty list of strings (no holes in the MVP).
    CHECK_FALSE(must_fail(R"({
  "component": "t", "kind": "task", "caps": ["task.run"], "task": {"argv": []}
})")
                    .empty());
    CHECK_FALSE(must_fail(R"({
  "component": "t", "kind": "task", "caps": ["task.run"], "task": {"argv": ["cmake", 7]}
})")
                    .empty());
    // net is deferred and rejected at load.
    CHECK(must_fail(R"({
  "component": "t", "kind": "task", "caps": ["task.run"],
  "task": {"argv": ["true"], "net": true}
})")
              .find("net") != std::string::npos);
}

TEST_CASE("malformed JSONC returns an error, never crashes") {
    CHECK_FALSE(must_fail("{nope").empty());
    CHECK_FALSE(must_fail("{]").empty());
    CHECK_FALSE(must_fail("").empty());
}

TEST_CASE("caps must be a sequence of strings") {
    CHECK_FALSE(
        must_fail(R"({"component": "c", "kind": "tool", "caps": "nope", "tree": {"do": "x"}})")
            .empty());
}

TEST_CASE("comments do not relax strictness") {
    // A comment is fine; an unknown key behind a comment is still rejected.
    Component ok = must_load(R"({
  "component": "c", "kind": "tool", "caps": [],
  /* block comment */
  "tree": {"do": "x"} // trailing comment
})");
    CHECK(ok.name == "c");

    CHECK_FALSE(must_fail(R"({
  "component": "c", "kind": "tool", "caps": [],
  "tree": {"do": "x"},
  // sneaky
  "extra": true
})")
                    .empty());
}
