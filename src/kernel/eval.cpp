// The evaluator. The `dispatch` builtin is kernel machinery (it needs the
// component registry and recursion), not a registered primitive: a tool failure
// is returned AS content (the model can recover); a corrupt envelope is a hard
// error; an unknown tool reports as content.

#include "kernel/eval.hpp"

#include <string>
#include <utility>

#include "schema/value_json.hpp"
#include "util/overloaded.hpp"

namespace astrite::kernel {
namespace {

using schema::Value;

// Cap on foreach iterations — the collection is finite, but defend against a
// pathologically large one (the same untrusted-count lesson as everywhere else).
constexpr size_t MAX_FOREACH_ITEMS = 1024;

std::expected<Value, KernelError> invoke(const std::string& name, Value args, const Context& ctx);

std::expected<Value, KernelError> dispatch(const Value& args, const Context& ctx) {
    auto fail = [](std::string m) {
        return std::unexpected(KernelError{"dispatch: " + std::move(m)});
    };
    const Value* call = args.get("call");
    if (call == nullptr) {
        return fail("missing `call`");
    }
    const Value* name_v = call->get("name");
    const std::string* name = name_v != nullptr ? name_v->as_str() : nullptr;
    if (name == nullptr) {
        return fail("tool call has no `name`");
    }
    const Value* id_v = call->get("id");
    const std::string id = id_v != nullptr && id_v->as_str() != nullptr ? *id_v->as_str() : "";
    const Value* args_v = call->get("arguments");
    const std::string arguments =
        args_v != nullptr && args_v->as_str() != nullptr ? *args_v->as_str() : "{}";

    if (ctx.observer != nullptr) {
        ctx.observer->tool_started(id, *name, arguments);
    }

    std::string content;
    auto component_it = ctx.components->find(*name);
    if (component_it == ctx.components->end()) {
        content = "error: unknown tool `" + *name + "`";
    } else {
        const schema::Component& component = component_it->second;
        // TODO(M2e): check_component_authority(component, ctx) before walking.
        auto input = schema::parse_json(arguments);
        Blackboard child;
        child.bind("input", input.has_value() ? std::move(*input) : Value::object());

        auto status = walk(component.tree, child, ctx);
        if (!status.has_value()) {
            content = "error: " + status.error().message;
        } else if (component.output.has_value()) {
            auto out = resolve(*component.output, child);
            if (!out.has_value()) {
                content = "error: " + out.error().message;
            } else if (const std::string* s = out->as_str()) {
                content = *s;
            } else {
                content = schema::to_json(*out);
            }
        }
    }

    if (ctx.observer != nullptr) {
        ctx.observer->tool_finished(id, *name, content);
    }
    Value result = Value::object();
    result.set("tool_call_id", Value::str(id));
    result.set("content", Value::str(std::move(content)));
    return result;
}

std::expected<Value, KernelError> invoke(const std::string& name, Value args, const Context& ctx) {
    if (name == "dispatch") {
        return dispatch(args, ctx);
    }
    const Registry::Fn* prim = ctx.primitives->find(name);
    if (prim == nullptr) {
        return std::unexpected(KernelError{"unknown primitive `" + name + "`"});
    }
    return (*prim)(args);
}

} // namespace

std::expected<Status, KernelError> walk(const schema::Node& node, Blackboard& bb,
                                        const Context& ctx) {
    using R = std::expected<Status, KernelError>;
    return std::visit(
        overloaded{
            [&](const schema::All& n) -> R {
                for (const schema::Node& child : n.children) {
                    auto s = walk(child, bb, ctx);
                    if (!s.has_value() || *s == Status::failure) {
                        return s;
                    }
                }
                return Status::success;
            },
            [&](const schema::Any& n) -> R {
                for (const schema::Node& child : n.children) {
                    auto s = walk(child, bb, ctx);
                    if (!s.has_value() || *s == Status::success) {
                        return s;
                    }
                }
                return Status::failure;
            },
            [&](const schema::Action& n) -> R {
                auto args = resolve_args(n.with, bb);
                if (!args.has_value()) {
                    return std::unexpected(args.error());
                }
                auto result = invoke(n.primitive, std::move(*args), ctx);
                if (!result.has_value()) {
                    return std::unexpected(result.error());
                }
                if (n.bind.has_value()) {
                    bb.bind(*n.bind, std::move(*result));
                }
                return Status::success;
            },
            [&](const schema::Condition& n) -> R {
                auto args = resolve_args(n.with, bb);
                if (!args.has_value()) {
                    return std::unexpected(args.error());
                }
                auto result = invoke(n.predicate, std::move(*args), ctx);
                if (!result.has_value()) {
                    return std::unexpected(result.error());
                }
                if (const bool* b = result->as_bool()) {
                    return *b ? Status::success : Status::failure;
                }
                return std::unexpected(KernelError{
                    "condition predicate `" + n.predicate + "` must return a bool, got " +
                    schema::to_json(*result)});
            },
            [&](const schema::Foreach& n) -> R {
                auto collection = resolve(n.over, bb);
                if (!collection.has_value()) {
                    return std::unexpected(collection.error());
                }
                const Value::List* elements = collection->as_list();
                if (elements == nullptr) {
                    return std::unexpected(KernelError{
                        "foreach: `over` must resolve to a list, got " +
                        schema::to_json(*collection)});
                }
                const size_t count = std::min(elements->size(), MAX_FOREACH_ITEMS);
                for (size_t i = 0; i < count; ++i) {
                    // Scope the per-element binding: save any prior slot, bind, restore.
                    std::optional<Value> prior = bb.remove(n.item);
                    bb.bind(n.item, (*elements)[i]);
                    auto status = walk(*n.body, bb, ctx);
                    if (prior.has_value()) {
                        bb.bind(n.item, std::move(*prior));
                    } else {
                        bb.remove(n.item);
                    }
                    if (!status.has_value()) {
                        return status; // side-effecting iteration: errors propagate,
                    } //                  Success/Failure of the body is ignored
                }
                return Status::success;
            },
            [&](const schema::Repeat& n) -> R {
                for (std::uint32_t i = 0; i < n.max; ++i) {
                    auto body = walk(*n.body, bb, ctx);
                    if (!body.has_value()) {
                        return body;
                    }
                    auto until = walk(*n.until, bb, ctx);
                    if (!until.has_value()) {
                        return until;
                    }
                    if (*until == Status::success) {
                        break;
                    }
                }
                return Status::success; // bounded-out is normal termination
            },
        },
        node.base());
}

} // namespace astrite::kernel
