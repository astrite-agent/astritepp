#pragma once

// The blackboard: a slot-keyed Value store — the only state a walking component
// sees. References resolve by pure lookup plus structural projection (object
// fields and literal list indices); they never compute. A missing slot, field,
// or index is a KernelError, never a silent null; an explicit {ref, default}
// fallback is used only when the reference cannot be resolved.

#include <charconv>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "kernel/error.hpp"
#include "schema/ir.hpp"
#include "schema/value.hpp"

namespace astrite::kernel {

class Blackboard {
public:
    void bind(std::string slot, schema::Value v) { slots_.insert_or_assign(std::move(slot), std::move(v)); }

    [[nodiscard]] const schema::Value* get(std::string_view slot) const {
        auto it = slots_.find(slot);
        return it == slots_.end() ? nullptr : &it->second;
    }

    // Take a slot out (used by foreach to scope its per-element binding).
    std::optional<schema::Value> remove(std::string_view slot) {
        auto it = slots_.find(slot);
        if (it == slots_.end()) {
            return std::nullopt;
        }
        schema::Value out = std::move(it->second);
        slots_.erase(it);
        return out;
    }

private:
    std::map<std::string, schema::Value, std::less<>> slots_;
};

namespace detail {

inline std::expected<schema::Value, KernelError> project(std::string_view path,
                                                         const Blackboard& bb) {
    auto unresolved = [&](std::string_view why) {
        return std::unexpected(
            KernelError{"unresolved ref `" + std::string(path) + "`: " + std::string(why)});
    };
    if (path.empty()) {
        return unresolved("empty path");
    }

    size_t pos = path.find('.');
    std::string_view slot = path.substr(0, pos == std::string_view::npos ? path.size() : pos);
    const schema::Value* current = bb.get(slot);
    if (current == nullptr) {
        return unresolved("no slot `" + std::string(slot) + "`");
    }

    while (pos != std::string_view::npos) {
        size_t start = pos + 1;
        pos = path.find('.', start);
        std::string_view seg =
            path.substr(start, pos == std::string_view::npos ? path.size() - start : pos - start);
        if (seg.empty()) {
            return unresolved("empty path segment");
        }
        if (const schema::Value::List* xs = current->as_list()) {
            size_t index = 0;
            auto [p, ec] = std::from_chars(seg.data(), seg.data() + seg.size(), index);
            if (ec != std::errc{} || p != seg.data() + seg.size()) {
                return unresolved("`" + std::string(seg) + "` is not a list index");
            }
            if (index >= xs->size()) {
                return unresolved("index " + std::string(seg) + " out of range");
            }
            current = &(*xs)[index];
        } else if (current->is_object()) {
            const schema::Value* field = current->get(seg);
            if (field == nullptr) {
                return unresolved("no field `" + std::string(seg) + "`");
            }
            current = field;
        } else {
            return unresolved("cannot project `" + std::string(seg) + "` through a scalar");
        }
    }
    return *current;
}

} // namespace detail

[[nodiscard]] inline std::expected<schema::Value, KernelError> resolve(const schema::ArgValue& arg,
                                                                       const Blackboard& bb) {
    if (const schema::Ref* ref = std::get_if<schema::Ref>(&arg.v)) {
        return detail::project(ref->path, bb);
    }
    if (const schema::RefDefault* rd = std::get_if<schema::RefDefault>(&arg.v)) {
        auto v = detail::project(rd->path, bb);
        if (v.has_value()) {
            return v;
        }
        return rd->fallback;
    }
    return std::get<schema::Value>(arg.v);
}

// Resolve a whole with-map into one args object for a primitive invocation.
[[nodiscard]] inline std::expected<schema::Value, KernelError>
resolve_args(const schema::ArgMap& with, const Blackboard& bb) {
    schema::Value out = schema::Value::object();
    for (const auto& [key, arg] : with) {
        auto v = resolve(arg, bb);
        if (!v.has_value()) {
            return std::unexpected(v.error());
        }
        out.set(key, std::move(*v));
    }
    return out;
}

} // namespace astrite::kernel
