#pragma once

// JSON bridge for Value. Parsing untrusted bytes goes through glaze (mature, fuzzed —
// DESIGN §15 rule 2: never hand-roll the parsers); i64 number mode gives the Int/Float
// split natively. This is the only schema header that includes glaze.

#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include <glaze/json.hpp>

#include "schema/value.hpp"

namespace astrite::schema {

namespace detail {

// Numbers parse as int64 when integral, double otherwise — matches Value's Int/Float split.
using Gen = glz::generic_json<glz::num_mode::i64>;

inline Value from_gen(const Gen& g) {
    if (g.is_boolean()) {
        return Value::boolean(g.get<bool>());
    }
    if (g.holds<std::int64_t>()) {
        return Value::integer(g.get<std::int64_t>());
    }
    if (g.holds<double>()) {
        return Value::real(g.get<double>());
    }
    if (g.is_string()) {
        return Value::str(g.get<std::string>());
    }
    if (g.is_array()) {
        Value::List xs;
        xs.reserve(g.get_array().size());
        for (const Gen& child : g.get_array()) {
            xs.push_back(from_gen(child));
        }
        return Value::list(std::move(xs));
    }
    if (g.is_object()) {
        Value obj = Value::object();
        for (const auto& [key, child] : g.get_object()) {
            obj.set(key, from_gen(child));
        }
        return obj;
    }
    return Value(); // null
}

inline Gen to_gen(const Value& v) {
    Gen g{};
    if (const bool* b = v.as_bool()) {
        g = *b;
    } else if (const std::int64_t* i = v.as_int()) {
        g = *i;
    } else if (const double* d = v.as_float()) {
        g = *d;
    } else if (const std::string* s = v.as_str()) {
        g = *s;
    } else if (const Value::List* xs = v.as_list()) {
        Gen::array_t arr;
        arr.reserve(xs->size());
        for (const Value& child : *xs) {
            arr.push_back(to_gen(child));
        }
        g = std::move(arr);
    } else if (const Value::Object* obj = v.as_object()) {
        Gen::object_t out;
        for (const auto& [key, child] : *obj) {
            out.insert({key, to_gen(child)});
        }
        g = std::move(out);
    }
    return g; // null stays default
}

} // namespace detail

inline std::expected<Value, std::string> parse_json(std::string_view text) {
    detail::Gen g{};
    if (auto err = glz::read_json(g, text)) {
        return std::unexpected(glz::format_error(err, text));
    }
    return detail::from_gen(g);
}

inline std::string to_json(const Value& v) {
    auto out = glz::write_json(detail::to_gen(v));
    // Writing a well-formed generic cannot fail; empty string would only mean a glaze bug.
    return out ? std::move(*out) : std::string{};
}

} // namespace astrite::schema
