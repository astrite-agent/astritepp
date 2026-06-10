#include <doctest.h>

#include "kernel/prims.hpp"
#include "kernel/trust.hpp"
#include "schema/value.hpp"

using namespace astrite::schema;
using astrite::kernel::coerce_json;
using astrite::kernel::resolve_path;

namespace {
Value args_path(std::string path, std::string base) {
    Value v = Value::object();
    v.set("path", Value::str(std::move(path)));
    v.set("base", Value::str(std::move(base)));
    return v;
}
bool ok(const Value& r) {
    const Value* o = r.get("ok");
    return o != nullptr && o->as_bool() != nullptr && *o->as_bool();
}
} // namespace

TEST_CASE("path.resolve: a clean relative path is confined and normalized") {
    Value r = resolve_path(args_path("src/kernel/eval.cpp", "workspace"));
    REQUIRE(ok(r));
    CHECK(*r.get("base") == Value::str("workspace"));
    CHECK(*r.get("rel") == Value::str("src/kernel/eval.cpp"));
}

TEST_CASE("path.resolve: . and redundant slashes collapse") {
    CHECK(*resolve_path(args_path("./a//b/./c", "workspace")).get("rel") ==
          Value::str("a/b/c"));
    CHECK(*resolve_path(args_path("a/b/../c", "workspace")).get("rel") == Value::str("a/c"));
    CHECK(*resolve_path(args_path("a/b/..", "workspace")).get("rel") == Value::str("a"));
}

TEST_CASE("path.resolve: escapes are rejected as structured failure, not exceptions") {
    for (const char* bad : {"../etc/passwd", "a/../../b", "..", "a/b/../../.."}) {
        Value r = resolve_path(args_path(bad, "workspace"));
        CHECK_FALSE(ok(r));
        CHECK(r.get("reason")->as_str()->find("escape") != std::string::npos);
    }
}

TEST_CASE("path.resolve: absolute paths, empty, and NUL are rejected") {
    CHECK_FALSE(ok(resolve_path(args_path("/etc/passwd", "workspace"))));
    CHECK_FALSE(ok(resolve_path(args_path("", "workspace"))));
    CHECK_FALSE(ok(resolve_path(args_path(std::string("a\0b", 3), "workspace"))));
}

TEST_CASE("path.resolve: only known base anchors are accepted") {
    CHECK(ok(resolve_path(args_path("x", "workspace"))));
    CHECK(ok(resolve_path(args_path("x", "scratch"))));
    CHECK_FALSE(ok(resolve_path(args_path("x", "/etc"))));
    CHECK_FALSE(ok(resolve_path(args_path("x", "elsewhere"))));
}

TEST_CASE("path.resolve: a path normalizing to root (empty rel) is rejected") {
    CHECK_FALSE(ok(resolve_path(args_path(".", "workspace"))));
    CHECK_FALSE(ok(resolve_path(args_path("a/..", "workspace"))));
}

TEST_CASE("path.resolve: malformed args fail closed, never throw") {
    CHECK_FALSE(ok(resolve_path(Value::object())));           // no path/base
    CHECK_FALSE(ok(resolve_path(Value::integer(3))));         // not even an object
    Value missing_base = Value::object();
    missing_base.set("path", Value::str("a"));
    CHECK_FALSE(ok(resolve_path(missing_base)));
}

namespace {
Value args_coerce(Value value, std::string as) {
    Value v = Value::object();
    v.set("value", std::move(value));
    v.set("as", Value::str(std::move(as)));
    return v;
}
} // namespace

TEST_CASE("json.coerce: exact-type values pass through") {
    CHECK(*coerce_json(args_coerce(Value::str("hi"), "string")).get("value") == Value::str("hi"));
    CHECK(*coerce_json(args_coerce(Value::integer(5), "int")).get("value") == Value::integer(5));
    CHECK(*coerce_json(args_coerce(Value::boolean(true), "bool")).get("value") ==
          Value::boolean(true));
    CHECK(ok(coerce_json(args_coerce(Value::list({}), "list"))));
    CHECK(ok(coerce_json(args_coerce(Value::object(), "object"))));
}

TEST_CASE("json.coerce: int widens to float; float does not narrow to int") {
    Value widened = coerce_json(args_coerce(Value::integer(3), "float"));
    REQUIRE(ok(widened));
    CHECK(*widened.get("value") == Value::real(3.0));
    CHECK_FALSE(ok(coerce_json(args_coerce(Value::real(2.5), "int"))));
}

TEST_CASE("json.coerce: type mismatches are conservative structured failures") {
    CHECK_FALSE(ok(coerce_json(args_coerce(Value::str("42"), "int"))));   // no string parsing
    CHECK_FALSE(ok(coerce_json(args_coerce(Value::integer(1), "bool")))); // no truthiness
    CHECK_FALSE(ok(coerce_json(args_coerce(Value::str("x"), "list"))));
    CHECK_FALSE(ok(coerce_json(args_coerce(Value(), "string"))));         // null is not a string
}

TEST_CASE("json.coerce: unknown target type and malformed args fail closed") {
    CHECK_FALSE(ok(coerce_json(args_coerce(Value::str("x"), "widget"))));
    CHECK_FALSE(ok(coerce_json(Value::object()))); // no value/as
}

TEST_CASE("both are registered as zero-cap prims in the default registry") {
    astrite::kernel::Registry reg = astrite::kernel::default_registry();
    CHECK(reg.has("path.resolve"));
    CHECK(reg.has("json.coerce"));
    CHECK(reg.cap_of("path.resolve") == nullptr); // pure: no capability
    CHECK(reg.cap_of("json.coerce") == nullptr);
}
