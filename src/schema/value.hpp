#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace astrite::schema {

// The kernel's only data type: Null, Bool, Int, Float, Str, List, Object.
// Int and Float are distinct types — no numeric coercion ever happens silently.
// Objects are sorted-by-key unique vectors: deterministic iteration, incomplete-type
// friendly, and cheap at the small sizes the blackboard sees.
class Value {
public:
    using List = std::vector<Value>;
    using Object = std::vector<std::pair<std::string, Value>>;

    Value() = default; // null

    static Value boolean(bool b) { return Value(Repr{b}); }
    static Value integer(std::int64_t i) { return Value(Repr{i}); }
    static Value real(double d) { return Value(Repr{d}); }
    static Value str(std::string s) { return Value(Repr{std::move(s)}); }
    static Value list(List xs) { return Value(Repr{std::move(xs)}); }
    static Value object() { return Value(Repr{Object{}}); }

    [[nodiscard]] bool is_null() const { return std::holds_alternative<std::nullptr_t>(v_); }
    [[nodiscard]] bool is_bool() const { return std::holds_alternative<bool>(v_); }
    [[nodiscard]] bool is_int() const { return std::holds_alternative<std::int64_t>(v_); }
    [[nodiscard]] bool is_float() const { return std::holds_alternative<double>(v_); }
    [[nodiscard]] bool is_str() const { return std::holds_alternative<std::string>(v_); }
    [[nodiscard]] bool is_list() const { return std::holds_alternative<List>(v_); }
    [[nodiscard]] bool is_object() const { return std::holds_alternative<Object>(v_); }

    // Wrong-type access returns nullptr, never UB.
    [[nodiscard]] const bool* as_bool() const { return std::get_if<bool>(&v_); }
    [[nodiscard]] const std::int64_t* as_int() const { return std::get_if<std::int64_t>(&v_); }
    [[nodiscard]] const double* as_float() const { return std::get_if<double>(&v_); }
    [[nodiscard]] const std::string* as_str() const { return std::get_if<std::string>(&v_); }
    [[nodiscard]] const List* as_list() const { return std::get_if<List>(&v_); }
    [[nodiscard]] const Object* as_object() const { return std::get_if<Object>(&v_); }

    // Insert or replace; requires is_object().
    void set(std::string key, Value val) {
        Object* obj = std::get_if<Object>(&v_);
        assert(obj && "Value::set requires an object");
        auto it = std::lower_bound(
            obj->begin(), obj->end(), key,
            [](const auto& kv, const std::string& k) { return kv.first < k; });
        if (it != obj->end() && it->first == key) {
            it->second = std::move(val);
        } else {
            obj->insert(it, {std::move(key), std::move(val)});
        }
    }

    // Key lookup; nullptr when missing or when this is not an object.
    [[nodiscard]] const Value* get(std::string_view key) const {
        const Object* obj = std::get_if<Object>(&v_);
        if (obj == nullptr) {
            return nullptr;
        }
        auto it = std::lower_bound(
            obj->begin(), obj->end(), key,
            [](const auto& kv, std::string_view k) { return kv.first < k; });
        if (it != obj->end() && it->first == key) {
            return &it->second;
        }
        return nullptr;
    }

    friend bool operator==(const Value&, const Value&) = default;

private:
    using Repr =
        std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, List, Object>;
    explicit Value(Repr r) : v_(std::move(r)) {}
    Repr v_;
};

} // namespace astrite::schema
