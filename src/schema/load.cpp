// YAML → typed IR, serde-style: the schema lives in the wire-type definitions and
// their glz::meta declarations. glaze derives the field plumbing — unknown keys
// rejected (yaml_opts default), required fields enforced (error_on_missing_keys),
// and the node sum type auto-deduced from which discriminator key is present.
// Exactly two custom readers are hand-written, same as the Rust original: ArgValue
// (the {ref}/{ref,default}/literal split is custom syntax) and Value (generic data).
// The wire→IR conversion below is typed field moves, visited exhaustively.

#include "schema/load.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <glaze/yaml.hpp>

#include "schema/value_json.hpp" // detail::Gen + from_gen (format-agnostic)
#include "util/overloaded.hpp"

namespace astrite::schema::wire {

// Decode shapes. Required-ness is in the types: std::optional and pointers may be
// absent, everything else must be present. Recursive positions that the IR makes
// mandatory (foreach/repeat bodies) are pointers here so the document decodes
// first and the conversion below reports the canonical error.
struct All;
struct Any;
struct Do;
struct When;
struct Foreach;
struct Repeat;
using Node = std::variant<All, Any, Do, When, Foreach, Repeat>;

struct All {
    std::vector<Node> children;
};
struct Any {
    std::vector<Node> children;
};
struct Do {
    std::string primitive;
    std::optional<ArgMap> with;
    std::optional<std::string> bind;
};
struct When {
    std::string predicate;
    std::optional<ArgMap> with;
};
struct ForeachSpec {
    ArgValue over;
    std::string item;
    std::unique_ptr<Node> body;
};
struct Foreach {
    ForeachSpec spec;
};
struct RepeatSpec {
    std::unique_ptr<Node> body;
    std::unique_ptr<Node> until;
    std::optional<std::uint32_t> max;
};
struct Repeat {
    RepeatSpec spec;
};

struct Component {
    std::string name;
    std::string kind;
    std::optional<std::vector<std::string>> caps;
    std::optional<std::string> description;
    std::optional<Value> parameters;
    Node tree;
    std::optional<ArgValue> output;
};

} // namespace astrite::schema::wire

// The schema, as data about the types: YAML key ↔ member mappings.
template <>
struct glz::meta<astrite::schema::wire::All> {
    using T = astrite::schema::wire::All;
    static constexpr auto value = object("all", &T::children);
};
template <>
struct glz::meta<astrite::schema::wire::Any> {
    using T = astrite::schema::wire::Any;
    static constexpr auto value = object("any", &T::children);
};
template <>
struct glz::meta<astrite::schema::wire::Do> {
    using T = astrite::schema::wire::Do;
    static constexpr auto value = object("do", &T::primitive, "with", &T::with, "as", &T::bind);
};
template <>
struct glz::meta<astrite::schema::wire::When> {
    using T = astrite::schema::wire::When;
    static constexpr auto value = object("when", &T::predicate, "with", &T::with);
};
template <>
struct glz::meta<astrite::schema::wire::ForeachSpec> {
    using T = astrite::schema::wire::ForeachSpec;
    static constexpr auto value = object("over", &T::over, "as", &T::item, "body", &T::body);
};
template <>
struct glz::meta<astrite::schema::wire::Foreach> {
    using T = astrite::schema::wire::Foreach;
    static constexpr auto value = object("foreach", &T::spec);
};
template <>
struct glz::meta<astrite::schema::wire::RepeatSpec> {
    using T = astrite::schema::wire::RepeatSpec;
    static constexpr auto value = object("body", &T::body, "until", &T::until, "max", &T::max);
};
template <>
struct glz::meta<astrite::schema::wire::Repeat> {
    using T = astrite::schema::wire::Repeat;
    static constexpr auto value = object("repeat", &T::spec);
};
template <>
struct glz::meta<astrite::schema::wire::Component> {
    using T = astrite::schema::wire::Component;
    static constexpr auto value =
        object("component", &T::name, "kind", &T::kind, "caps", &T::caps, "description",
               &T::description, "parameters", &T::parameters, "tree", &T::tree, "output",
               &T::output);
};

// Custom reader: Value is generic data — delegate to glaze's generic reader (i64
// number mode: unquoted integral scalars become Int, quoted scalars stay Str).
template <>
struct glz::from<glz::JSON, astrite::schema::Value> {
    template <auto Opts>
    static void op(auto&& value, is_context auto&& ctx, auto&& it, auto&& end) {
        astrite::schema::detail::Gen g{};
        from<JSON, astrite::schema::detail::Gen>::template op<Opts>(g, ctx, it, end);
        if (bool(ctx.error)) {
            return;
        }
        value = astrite::schema::detail::from_gen(g);
    }
};

// Custom reader: a map containing `ref` is a reference form (only `ref` and
// optional `default` allowed, `ref` must be a string); anything else is a literal.
template <>
struct glz::from<glz::JSON, astrite::schema::ArgValue> {
    template <auto Opts>
    static void op(auto&& value, is_context auto&& ctx, auto&& it, auto&& end) {
        namespace as = astrite::schema;
        as::detail::Gen g{};
        from<JSON, as::detail::Gen>::template op<Opts>(g, ctx, it, end);
        if (bool(ctx.error)) {
            return;
        }
        if (g.is_object()) {
            auto& obj = g.get_object();
            auto ref_it = obj.find("ref");
            if (ref_it != obj.end()) {
                if (!ref_it->second.is_string()) {
                    ctx.error = error_code::constraint_violated;
                    ctx.custom_error_message = "`ref` must be a string";
                    return;
                }
                const as::detail::Gen* fallback = nullptr;
                for (const auto& [key, child] : obj) {
                    if (key == "ref") {
                        continue;
                    }
                    if (key == "default") {
                        fallback = &child;
                        continue;
                    }
                    ctx.error = error_code::constraint_violated;
                    ctx.custom_error_message =
                        "ref maps may contain only `ref` and optional `default`";
                    return;
                }
                std::string path = ref_it->second.get<std::string>();
                if (fallback == nullptr) {
                    value.v = as::Ref{std::move(path)};
                } else {
                    value.v = as::RefDefault{std::move(path), as::detail::from_gen(*fallback)};
                }
                return;
            }
        }
        value.v = as::detail::from_gen(g);
    }
};

namespace astrite::schema {
namespace {

template <class T>
using Result = std::expected<T, LoadError>;

std::unexpected<LoadError> fail(std::string msg) { return std::unexpected(LoadError{std::move(msg)}); }

Result<Node> to_ir(const wire::Node& w);

Result<std::vector<Node>> to_ir_children(const std::vector<wire::Node>& children) {
    std::vector<Node> out;
    out.reserve(children.size());
    for (const wire::Node& child : children) {
        auto node = to_ir(child);
        if (!node) {
            return std::unexpected(node.error());
        }
        out.push_back(std::move(*node));
    }
    return out;
}

Result<Node> to_ir(const wire::Node& w) {
    return std::visit(
        overloaded{
            [](const wire::All& n) -> Result<Node> {
                auto children = to_ir_children(n.children);
                if (!children) {
                    return std::unexpected(children.error());
                }
                return Node{All{std::move(*children)}};
            },
            [](const wire::Any& n) -> Result<Node> {
                auto children = to_ir_children(n.children);
                if (!children) {
                    return std::unexpected(children.error());
                }
                return Node{Any{std::move(*children)}};
            },
            [](const wire::Do& n) -> Result<Node> {
                return Node{Action{n.primitive, n.with.value_or(ArgMap{}), n.bind}};
            },
            [](const wire::When& n) -> Result<Node> {
                return Node{Condition{n.predicate, n.with.value_or(ArgMap{})}};
            },
            [](const wire::Foreach& n) -> Result<Node> {
                if (n.spec.body == nullptr) {
                    return fail("foreach requires over, as, and body");
                }
                auto body = to_ir(*n.spec.body);
                if (!body) {
                    return std::unexpected(body.error());
                }
                return Node{Foreach{n.spec.over, n.spec.item, box<Node>(std::move(*body))}};
            },
            [](const wire::Repeat& n) -> Result<Node> {
                if (n.spec.body == nullptr || n.spec.until == nullptr) {
                    return fail("repeat requires body and until");
                }
                const std::uint32_t max = n.spec.max.value_or(16u);
                if (max == 0) {
                    return fail("repeat `max` must be a positive integer");
                }
                auto body = to_ir(*n.spec.body);
                if (!body) {
                    return std::unexpected(body.error());
                }
                auto until = to_ir(*n.spec.until);
                if (!until) {
                    return std::unexpected(until.error());
                }
                return Node{Repeat{box<Node>(std::move(*body)), box<Node>(std::move(*until)), max}};
            },
        },
        w);
}

} // namespace

std::expected<Component, LoadError> load_component_yaml(std::string_view yaml) {
    // Two stages. Stage 1: one document-level generic YAML parse (glaze's typed-member
    // YAML path mishandles nested multi-entry block maps in v7.8.0 — see TODO.md note;
    // the document-level generic path is solid). Stage 2: serialize to JSON and run
    // glaze's mature JSON typed reader: variant key-deduction, unknown keys rejected,
    // required fields enforced. This split is also the confined-loader shape (M3): the
    // sandboxed child will own stage 1 and ship JSON over the pipe.
    std::string buf(yaml);
    detail::Gen g{};
    if (auto ec = glz::read_yaml(g, buf)) {
        return std::unexpected(LoadError{"YAML parse error: " + glz::format_error(ec, buf)});
    }
    auto json = glz::write_json(g);
    if (!json) {
        return std::unexpected(LoadError{"internal: generic→json failed"});
    }

    wire::Component w;
    auto ec = glz::read<glz::opts{.error_on_missing_keys = true}>(w, *json);
    if (ec) {
        if (ec.ec == glz::error_code::no_matching_variant_type) {
            return std::unexpected(LoadError{
                "a node must have exactly one of: all, any, do, when, foreach, repeat\n" +
                glz::format_error(ec, *json)});
        }
        return std::unexpected(LoadError{glz::format_error(ec, *json)});
    }

    Component c;
    c.name = std::move(w.name);
    c.kind = std::move(w.kind);
    c.caps = w.caps.value_or(std::vector<std::string>{});
    c.description = std::move(w.description);
    c.parameters = std::move(w.parameters);
    c.output = std::move(w.output);
    auto tree = to_ir(w.tree);
    if (!tree) {
        return std::unexpected(tree.error());
    }
    c.tree = std::move(*tree);
    return c;
}

} // namespace astrite::schema
