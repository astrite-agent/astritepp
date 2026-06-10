#pragma once

// The trust-boundary pair (DESIGN §6): the only entry points from untrusted
// model output into the typed, confined world. Both are PURE — no filesystem,
// no exceptions — and return a structured {ok, ...} value rather than a
// KernelError, so components branch on the outcome with a `when:` guard.
//
//   path.resolve  untrusted string + base anchor -> {ok, base, rel} | {ok:false, reason}
//   json.coerce   untrusted value + declared type -> {ok, value}     | {ok:false, reason}

#include <string>
#include <string_view>
#include <vector>

#include "schema/value.hpp"

namespace astrite::kernel {

namespace detail {

inline astrite::schema::Value resolve_ok(std::string base, std::string rel) {
    using astrite::schema::Value;
    Value v = Value::object();
    v.set("ok", Value::boolean(true));
    v.set("base", Value::str(std::move(base)));
    v.set("rel", Value::str(std::move(rel)));
    return v;
}

inline astrite::schema::Value failure(std::string reason) {
    using astrite::schema::Value;
    Value v = Value::object();
    v.set("ok", Value::boolean(false));
    v.set("reason", Value::str(std::move(reason)));
    return v;
}

} // namespace detail

// Lexically confine an untrusted path under a base anchor. Never touches the
// filesystem (the Landlock floor is the ground-truth suspenders); this is the
// belt. Rejects absolute paths, NUL bytes, and any `..` that would climb above
// the base. A path normalizing to the base itself (empty rel) is rejected.
[[nodiscard]] inline astrite::schema::Value resolve_path(const astrite::schema::Value& args) {
    using astrite::schema::Value;
    const Value* path_v = args.get("path");
    const Value* base_v = args.get("base");
    if (path_v == nullptr || path_v->as_str() == nullptr || base_v == nullptr ||
        base_v->as_str() == nullptr) {
        return detail::failure("path.resolve requires string `path` and `base`");
    }
    const std::string& base = *base_v->as_str();
    if (base != "workspace" && base != "scratch") {
        return detail::failure("unknown base anchor `" + base + "`");
    }
    const std::string& path = *path_v->as_str();
    if (path.empty()) {
        return detail::failure("empty path");
    }
    if (path.find('\0') != std::string::npos) {
        return detail::failure("path contains NUL");
    }
    if (path.front() == '/') {
        return detail::failure("absolute path is an escape");
    }

    std::vector<std::string_view> stack;
    size_t i = 0;
    while (i < path.size()) {
        size_t slash = path.find('/', i);
        std::string_view seg = std::string_view(path).substr(
            i, slash == std::string::npos ? std::string::npos : slash - i);
        i = (slash == std::string::npos) ? path.size() : slash + 1;
        if (seg.empty() || seg == ".") {
            continue;
        }
        if (seg == "..") {
            if (stack.empty()) {
                return detail::failure("path escapes the base anchor");
            }
            stack.pop_back();
            continue;
        }
        stack.push_back(seg);
    }
    if (stack.empty()) {
        return detail::failure("path resolves to the base itself");
    }
    std::string rel;
    for (std::string_view seg : stack) {
        if (!rel.empty()) {
            rel += '/';
        }
        rel += seg;
    }
    return detail::resolve_ok(base, std::move(rel));
}

// Conservatively assert an untrusted value's type. No lossy coercion: strings
// are not parsed into numbers, numbers are not read as bools. The single
// widening allowed is int -> float.
[[nodiscard]] inline astrite::schema::Value coerce_json(const astrite::schema::Value& args) {
    using astrite::schema::Value;
    const Value* value = args.get("value");
    const Value* as_v = args.get("as");
    if (value == nullptr || as_v == nullptr || as_v->as_str() == nullptr) {
        return detail::failure("json.coerce requires `value` and string `as`");
    }
    const std::string& as = *as_v->as_str();

    auto ok = [](Value v) {
        Value out = Value::object();
        out.set("ok", Value::boolean(true));
        out.set("value", std::move(v));
        return out;
    };

    if (as == "string") {
        return value->is_str() ? ok(*value) : detail::failure("expected string");
    }
    if (as == "int") {
        return value->is_int() ? ok(*value) : detail::failure("expected int");
    }
    if (as == "float") {
        if (value->is_float()) {
            return ok(*value);
        }
        if (const std::int64_t* i = value->as_int()) {
            return ok(Value::real(static_cast<double>(*i))); // int widens to float
        }
        return detail::failure("expected float");
    }
    if (as == "bool") {
        return value->is_bool() ? ok(*value) : detail::failure("expected bool");
    }
    if (as == "list") {
        return value->is_list() ? ok(*value) : detail::failure("expected list");
    }
    if (as == "object") {
        return value->is_object() ? ok(*value) : detail::failure("expected object");
    }
    return detail::failure("unknown target type `" + as + "`");
}

} // namespace astrite::kernel
