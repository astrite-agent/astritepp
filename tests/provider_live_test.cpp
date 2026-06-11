// Key-gated live smokes: each does one real round-trip when its API key is in
// the environment, and skips cleanly (passes with no assertions) when it isn't —
// so the default `ctest` run costs nothing, but `export $(cat .env) && run` (or
// CI with secrets) exercises each backend end to end including caching where
// supported. Kept out of the always-on suite's cost by the env gate, not #ifdef.
#include <doctest.h>

#include <cstdlib>
#include <string>

#include "llm/anthropic.hpp"
#include "llm/gpt.hpp"
#include "llm/openai_compat.hpp"
#include "schema/value.hpp"

using namespace astrite;
using namespace astrite::schema;

namespace {
Value one_user(std::string text) {
    Value m = Value::object();
    m.set("role", Value::str("user"));
    m.set("content", Value::str(std::move(text)));
    return Value::list({std::move(m)});
}
bool has(const char* k) {
    const char* v = std::getenv(k);
    return v != nullptr && *v != '\0';
}
} // namespace

TEST_CASE("live: openai-compat round-trips (skipped without OPENAI_API_KEY)") {
    if (!has("ASTRITE_LIVE_TESTS") || !has("OPENAI_API_KEY")) {
        return;
    }
    auto cfg = llm::Config::from_env();
    REQUIRE(cfg.has_value());
    llm::OpenAiCompatProvider p(*cfg);
    auto r = p.stream_chat(one_user("Reply with exactly: ok"), Value::list({}), {}, {});
    REQUIRE(r.has_value());
    CHECK_FALSE(r->text.empty());
}

TEST_CASE("live: gpt (Responses API) round-trips (skipped without OPENAI_API_KEY)") {
    if (!has("ASTRITE_LIVE_TESTS") || !has("OPENAI_API_KEY")) {
        return;
    }
    auto cfg = llm::GptConfig::from_env();
    REQUIRE(cfg.has_value());
    llm::GptProvider p(*cfg);
    auto r = p.stream_chat(one_user("Reply with exactly: ok"), Value::list({}), {}, {});
    REQUIRE(r.has_value());
    CHECK_FALSE(r->text.empty());
}

TEST_CASE("live: anthropic round-trips + reports usage (skipped without ANTHROPIC_API_KEY)") {
    if (!has("ASTRITE_LIVE_TESTS") || !has("ANTHROPIC_API_KEY")) {
        return;
    }
    auto cfg = llm::AnthropicConfig::from_env();
    REQUIRE(cfg.has_value());
    llm::AnthropicProvider p(*cfg);
    auto r = p.stream_chat(one_user("Reply with exactly: ok"), Value::list({}), {}, {});
    REQUIRE(r.has_value());
    CHECK_FALSE(r->text.empty());
    CHECK(r->usage.present);
    CHECK(r->usage.prompt_tokens > 0);
}
