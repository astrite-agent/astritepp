#include <doctest.h>

#include "kernel/blackboard.hpp"

using namespace astrite;
using namespace astrite::schema;
using kernel::Blackboard;
using kernel::resolve;
using kernel::resolve_args;

namespace {
Value sample_ctx() {
    Value user = Value::object();
    user.set("name", Value::str("eve"));
    user.set("admin", Value::boolean(true));
    Value msgs = Value::list({Value::str("a"), Value::str("b")});
    Value ctx = Value::object();
    ctx.set("user", std::move(user));
    ctx.set("messages", std::move(msgs));
    return ctx;
}
} // namespace

TEST_CASE("bind, get, overwrite, remove") {
    Blackboard bb;
    CHECK(bb.get("x") == nullptr);
    bb.bind("x", Value::integer(1));
    REQUIRE(bb.get("x") != nullptr);
    CHECK(*bb.get("x") == Value::integer(1));
    bb.bind("x", Value::str("two"));
    CHECK(*bb.get("x") == Value::str("two"));

    auto prior = bb.remove("x");
    REQUIRE(prior.has_value());
    CHECK(*prior == Value::str("two"));
    CHECK(bb.get("x") == nullptr);
    CHECK_FALSE(bb.remove("x").has_value());
}

TEST_CASE("resolve: literals pass through untouched") {
    Blackboard bb;
    auto v = resolve(ArgValue{Value::integer(7)}, bb);
    REQUIRE(v.has_value());
    CHECK(*v == Value::integer(7));
}

TEST_CASE("resolve: whole-slot ref") {
    Blackboard bb;
    bb.bind("ctx", sample_ctx());
    auto v = resolve(ArgValue{Ref{"ctx"}}, bb);
    REQUIRE(v.has_value());
    CHECK(*v == sample_ctx());
}

TEST_CASE("resolve: structural projection through fields and literal indices") {
    Blackboard bb;
    bb.bind("ctx", sample_ctx());
    auto name = resolve(ArgValue{Ref{"ctx.user.name"}}, bb);
    REQUIRE(name.has_value());
    CHECK(*name == Value::str("eve"));

    auto second = resolve(ArgValue{Ref{"ctx.messages.1"}}, bb);
    REQUIRE(second.has_value());
    CHECK(*second == Value::str("b"));
}

TEST_CASE("resolve: failures are errors, never silent null") {
    Blackboard bb;
    bb.bind("ctx", sample_ctx());
    CHECK_FALSE(resolve(ArgValue{Ref{"missing"}}, bb).has_value());
    CHECK_FALSE(resolve(ArgValue{Ref{"ctx.nope"}}, bb).has_value());
    CHECK_FALSE(resolve(ArgValue{Ref{"ctx.messages.9"}}, bb).has_value());
    CHECK_FALSE(resolve(ArgValue{Ref{"ctx.user.name.deeper"}}, bb).has_value()); // through scalar
    CHECK_FALSE(resolve(ArgValue{Ref{""}}, bb).has_value());
    CHECK_FALSE(resolve(ArgValue{Ref{"ctx..user"}}, bb).has_value()); // empty segment
    // Errors carry the offending path for diagnostics.
    auto err = resolve(ArgValue{Ref{"ctx.nope"}}, bb);
    CHECK(err.error().message.find("ctx.nope") != std::string::npos);
}

TEST_CASE("resolve: explicit default fills only unresolvable refs") {
    Blackboard bb;
    bb.bind("ctx", sample_ctx());
    auto missing = resolve(ArgValue{RefDefault{"ctx.retries", Value::integer(3)}}, bb);
    REQUIRE(missing.has_value());
    CHECK(*missing == Value::integer(3));

    auto present = resolve(ArgValue{RefDefault{"ctx.user.name", Value::str("nobody")}}, bb);
    REQUIRE(present.has_value());
    CHECK(*present == Value::str("eve"));
}

TEST_CASE("resolve_args builds one object from a with-map") {
    Blackboard bb;
    bb.bind("ctx", sample_ctx());
    ArgMap with;
    with.emplace("who", ArgValue{Ref{"ctx.user.name"}});
    with.emplace("count", ArgValue{Value::integer(2)});
    auto args = resolve_args(with, bb);
    REQUIRE(args.has_value());
    REQUIRE(args->is_object());
    CHECK(*args->get("who") == Value::str("eve"));
    CHECK(*args->get("count") == Value::integer(2));

    ArgMap bad;
    bad.emplace("oops", ArgValue{Ref{"ctx.absent"}});
    CHECK_FALSE(resolve_args(bad, bb).has_value());
}
