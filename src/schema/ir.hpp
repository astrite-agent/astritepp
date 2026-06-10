#pragma once

// The behavior-tree IR — which *is* the component schema (DESIGN §5). The loader
// decodes YAML into exactly these shapes and rejects everything else. There are no
// operators and no expression language: an argument is a literal, a structural
// {ref} lookup, or an explicit {ref, default} lookup.

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "schema/value.hpp"
#include "util/box.hpp"

namespace astrite::schema {

// {ref: "slot.path"} — pure lookup + structural projection, never computes.
struct Ref {
    std::string path;
    friend bool operator==(const Ref&, const Ref&) = default;
};

// {ref: "slot.path", default: literal} — fallback used only when unresolvable.
struct RefDefault {
    std::string path;
    Value fallback;
    friend bool operator==(const RefDefault&, const RefDefault&) = default;
};

struct ArgValue {
    std::variant<Ref, RefDefault, Value> v;
    friend bool operator==(const ArgValue&, const ArgValue&) = default;
};

using ArgMap = std::map<std::string, ArgValue, std::less<>>;

[[nodiscard]] inline const ArgValue* find_arg(const ArgMap& args, std::string_view key) {
    auto it = args.find(key);
    return it == args.end() ? nullptr : &it->second;
}

struct Node;

// AND — run children in order, fail-fast.
struct All {
    std::vector<Node> children;
    friend bool operator==(const All&, const All&) = default;
};

// OR — run children in order; first success wins.
struct Any {
    std::vector<Node> children;
    friend bool operator==(const Any&, const Any&) = default;
};

// do: invoke a primitive, optionally binding its result to a blackboard slot.
struct Action {
    std::string primitive;
    ArgMap with;
    std::optional<std::string> bind;
    friend bool operator==(const Action&, const Action&) = default;
};

// when: invoke a predicate that must return a bool; Success/Failure, no bind.
struct Condition {
    std::string predicate;
    ArgMap with;
    friend bool operator==(const Condition&, const Condition&) = default;
};

// Walk body once per element of a finite collection; element bound in a child scope.
struct Foreach {
    ArgValue over;
    std::string item;
    box<Node> body;
    friend bool operator==(const Foreach&, const Foreach&) = default;
};

// Walk body until `until` succeeds, bounded by max.
struct Repeat {
    box<Node> body;
    box<Node> until;
    std::uint32_t max = 16;
    friend bool operator==(const Repeat&, const Repeat&) = default;
};

using NodeVariant = std::variant<All, Any, Action, Condition, Foreach, Repeat>;

struct Node : NodeVariant {
    using NodeVariant::NodeVariant;
    [[nodiscard]] const NodeVariant& base() const { return *this; }
    [[nodiscard]] NodeVariant& base() { return *this; }
    friend bool operator==(const Node&, const Node&) = default;
};

template <class T>
[[nodiscard]] const T* node_as(const Node& n) {
    return std::get_if<T>(&n.base());
}

struct Component {
    std::string name;
    std::string kind; // tool | command | agent | view | persona | policy | profile
    std::vector<std::string> caps;
    std::optional<std::string> description;
    std::optional<Value> parameters; // model-facing JSON Schema, passed through verbatim
    Node tree;
    std::optional<ArgValue> output;
    friend bool operator==(const Component&, const Component&) = default;
};

} // namespace astrite::schema
