#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>

#include "kernel/error.hpp"
#include "schema/ir.hpp"
#include "schema/value.hpp"

namespace astrite::kernel {

enum class Status : std::uint8_t { success, failure };

// The closed primitive catalog at runtime. Effects are registered as closures by
// the runtime (capturing their confined context) and carry the capability id a
// component must declare to reach them; pure primitives carry none. The kernel
// only ever sees args-object → result-or-error.
class Registry {
public:
    using Fn = std::function<std::expected<schema::Value, KernelError>(const schema::Value& args)>;

    // A pure primitive: no capability required.
    void add(std::string name, Fn fn) {
        prims_.insert_or_assign(std::move(name), Entry{std::move(fn), std::nullopt});
    }

    // An effect primitive: reachable only by components granting `cap`.
    void add_effect(std::string name, std::string cap, Fn fn) {
        prims_.insert_or_assign(std::move(name), Entry{std::move(fn), std::move(cap)});
    }

    [[nodiscard]] const Fn* find(std::string_view name) const {
        auto it = prims_.find(name);
        return it == prims_.end() ? nullptr : &it->second.fn;
    }

    [[nodiscard]] bool has(std::string_view name) const { return prims_.contains(name); }

    // The capability an effect requires; nullptr for pure or unknown primitives.
    [[nodiscard]] const std::string* cap_of(std::string_view name) const {
        auto it = prims_.find(name);
        if (it == prims_.end() || !it->second.cap.has_value()) {
            return nullptr;
        }
        return &*it->second.cap;
    }

private:
    struct Entry {
        Fn fn;
        std::optional<std::string> cap;
    };
    std::map<std::string, Entry, std::less<>> prims_;
};

// Optional evaluator observer used by frontends/runtimes for progress events.
// It receives stable kernel facts, not UI instructions.
class EvalObserver {
public:
    virtual ~EvalObserver() = default;
    virtual void tool_started(std::string_view /*id*/, std::string_view /*name*/,
                              std::string_view /*arguments*/) {}
    virtual void tool_finished(std::string_view /*id*/, std::string_view /*name*/,
                               std::string_view /*content*/) {}
};

using ComponentMap = std::map<std::string, schema::Component, std::less<>>;

// The evaluation context: the primitive registry, plus the component registry
// the `dispatch` builtin resolves tool-call names against.
struct Context {
    const Registry* primitives;
    const ComponentMap* components;
    EvalObserver* observer = nullptr;
};

} // namespace astrite::kernel
