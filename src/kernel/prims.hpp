#pragma once

// The pure default primitives every kernel carries. Strict by doctrine: a
// security-oriented kernel does not guess truthiness — wrong-typed input is an
// error, never a coerced bool.

#include <expected>

#include "kernel/error.hpp"
#include "kernel/primitive.hpp"
#include "schema/value.hpp"

namespace astrite::kernel {

[[nodiscard]] inline Registry default_registry() {
    using schema::Value;
    Registry reg;
    reg.add("bool.is_true", [](const Value& args) -> std::expected<Value, KernelError> {
        const Value* in = args.get("in");
        if (in == nullptr || !in->is_bool()) {
            return std::unexpected(KernelError{"bool.is_true: `in` must be a bool"});
        }
        return *in;
    });
    reg.add("coll.is_empty", [](const Value& args) -> std::expected<Value, KernelError> {
        const Value* in = args.get("in");
        if (in == nullptr || !in->is_list()) {
            return std::unexpected(KernelError{"coll.is_empty: `in` must be a list"});
        }
        return Value::boolean(in->as_list()->empty());
    });
    reg.add("obj.build", [](const Value& args) -> std::expected<Value, KernelError> {
        return args; // the resolved with-map IS the built object
    });
    return reg;
}

} // namespace astrite::kernel
