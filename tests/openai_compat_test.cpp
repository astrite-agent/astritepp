#include <doctest.h>

#include "llm/openai_compat.hpp"
#include "schema/value_json.hpp"

using namespace astrite;
using namespace astrite::schema;

TEST_CASE("build_request_body carries model, messages verbatim, and stream flag") {
    Value msgs = Value::list({[] {
        Value m = Value::object();
        m.set("role", Value::str("user"));
        m.set("content", Value::str("hi"));
        return m;
    }()});
    std::string body = llm::build_request_body("gpt-test", msgs, Value::list({}));
    auto parsed = parse_json(body);
    REQUIRE(parsed.has_value());
    CHECK(*parsed->get("model") == Value::str("gpt-test"));
    CHECK(*parsed->get("stream") == Value::boolean(true));
    REQUIRE(parsed->get("messages") != nullptr);
    CHECK(*(*parsed->get("messages")->as_list())[0].get("content") == Value::str("hi"));
    // Empty tools list is omitted entirely (strict servers reject tools: null/[]).
    CHECK(parsed->get("tools") == nullptr);
}

TEST_CASE("build_request_body includes tools only when present") {
    Value tool = Value::object();
    tool.set("type", Value::str("function"));
    std::string body =
        llm::build_request_body("m", Value::list({}), Value::list({std::move(tool)}));
    auto parsed = parse_json(body);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->get("tools") != nullptr);
    CHECK(parsed->get("tools")->as_list()->size() == 1);
}

TEST_CASE("config falls back to defaults, requires the key") {
    auto cfg = llm::Config::from_env_or("key123", "", "");
    CHECK(cfg.api_key == "key123");
    CHECK(cfg.model == "gpt-5.2");
    CHECK(cfg.base_url == "https://api.openai.com/v1");

    auto custom = llm::Config::from_env_or("k", "my-model", "http://localhost:8080/v1/");
    CHECK(custom.model == "my-model");
    CHECK(custom.base_url == "http://localhost:8080/v1"); // trailing slash trimmed
}
