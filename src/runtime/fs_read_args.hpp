#pragma once

// Pure translation of resolved fs.read arguments into the helper's range spec.
// Kept out of runtime.cpp so the offset/limit validation is unit-testable
// without standing up a Runtime, and so the sandbox layer stays free of any
// schema::Value dependency (it speaks only the "offset:limit" string).

#include <expected>
#include <string>
#include <string_view>

#include "kernel/error.hpp"
#include "schema/value.hpp"

namespace astrite::runtime {

// Build the "offset:limit" range spec for the fs.read helper from resolved args.
// `offset`/`limit` are optional non-negative integers; absent, null, or both
// non-positive means "whole file" and yields an empty spec (the back-compat
// default). Negatives clamp (offset up to 1, limit down to 0 == to-end). A
// present-but-wrong-typed value is a hard error — the kernel never coerces.
[[nodiscard]] inline std::expected<std::string, kernel::KernelError>
read_range_spec(const schema::Value& args) {
    auto read_int = [&](std::string_view key,
                        long& out) -> std::expected<void, kernel::KernelError> {
        const schema::Value* v = args.get(key);
        if (v == nullptr || v->is_null()) {
            return {};
        }
        if (!v->is_int()) {
            return std::unexpected(kernel::KernelError{
                "fs.read: `" + std::string(key) + "` must be an integer"});
        }
        out = static_cast<long>(*v->as_int());
        return {};
    };

    long offset = 0;
    long limit = 0;
    if (auto r = read_int("offset", offset); !r) {
        return std::unexpected(r.error());
    }
    if (auto r = read_int("limit", limit); !r) {
        return std::unexpected(r.error());
    }
    if (offset <= 0 && limit <= 0) {
        return std::string{}; // whole file
    }
    if (offset < 1) {
        offset = 1;
    }
    if (limit < 0) {
        limit = 0;
    }
    return std::to_string(offset) + ":" + std::to_string(limit);
}

} // namespace astrite::runtime
