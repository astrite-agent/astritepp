#include <doctest.h>

#include "schema/value.hpp"
#include "schema/value_json.hpp"

using astrite::schema::Value;
using astrite::schema::parse_json;
using astrite::schema::to_json;

TEST_CASE("default Value is null") {
    Value v;
    CHECK(v.is_null());
    CHECK_FALSE(v.is_bool());
    CHECK_FALSE(v.is_object());
}

TEST_CASE("scalar factories carry their type and payload") {
    CHECK(*Value::boolean(true).as_bool() == true);
    CHECK(*Value::integer(-7).as_int() == -7);
    CHECK(*Value::real(2.5).as_float() == 2.5);
    CHECK(*Value::str("hi").as_str() == "hi");
    CHECK(Value::integer(1).as_bool() == nullptr); // wrong-type access is null, never UB
    CHECK(Value::str("x").as_int() == nullptr);
}

TEST_CASE("list holds ordered elements") {
    Value v = Value::list({Value::integer(1), Value::str("two")});
    REQUIRE(v.is_list());
    REQUIRE(v.as_list()->size() == 2);
    CHECK(*(*v.as_list())[1].as_str() == "two");
}

TEST_CASE("object set/get keeps keys sorted and unique") {
    Value obj = Value::object();
    obj.set("b", Value::integer(1));
    obj.set("a", Value::boolean(true));
    obj.set("b", Value::integer(2)); // replace, not duplicate

    REQUIRE(obj.is_object());
    REQUIRE(obj.as_object()->size() == 2);
    CHECK((*obj.as_object())[0].first == "a"); // sorted
    CHECK(*obj.get("b")->as_int() == 2);
    CHECK(obj.get("missing") == nullptr);
    CHECK(Value::integer(1).get("a") == nullptr); // get on non-object is null
}

TEST_CASE("equality is deep") {
    Value a = Value::object();
    a.set("xs", Value::list({Value::integer(1), Value::integer(2)}));
    Value b = Value::object();
    b.set("xs", Value::list({Value::integer(1), Value::integer(2)}));
    CHECK(a == b);
    b.set("xs", Value::list({Value::integer(1)}));
    CHECK(a != b);
    CHECK(Value::integer(1) != Value::real(1.0)); // Int and Float are distinct types
}

TEST_CASE("parse_json decodes objects, arrays, and scalars") {
    auto v = parse_json(R"({"n": 3, "f": 3.5, "s": "x", "t": true, "z": null, "xs": [1, 2]})");
    REQUIRE(v.has_value());
    CHECK(*v->get("n")->as_int() == 3); // integral-valued number decodes as Int
    CHECK(*v->get("f")->as_float() == 3.5);
    CHECK(*v->get("s")->as_str() == "x");
    CHECK(*v->get("t")->as_bool() == true);
    CHECK(v->get("z")->is_null());
    CHECK(*(*v->get("xs")->as_list())[0].as_int() == 1);
}

TEST_CASE("parse_json returns error on malformed input without throwing") {
    auto v = parse_json("{nope");
    CHECK_FALSE(v.has_value());
    CHECK_FALSE(v.error().empty());
}

TEST_CASE("to_json round-trips through parse_json") {
    Value obj = Value::object();
    obj.set("msg", Value::str("he said \"hi\"\n"));
    obj.set("count", Value::integer(42));
    obj.set("items", Value::list({Value::boolean(false), Value()}));

    auto back = parse_json(to_json(obj));
    REQUIRE(back.has_value());
    CHECK(*back == obj);
}
