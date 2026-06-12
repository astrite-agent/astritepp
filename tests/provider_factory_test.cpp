// Building a provider by explicit name + model (the seam live model/provider
// switching uses), and the env-driven default path. No network — name()/model()
// are local; we only check construction + selection, never a request.
#include <doctest.h>

#include <cstdlib>
#include <string>

#include "llm/provider_factory.hpp"

using namespace astrite::llm;

namespace {
// Set the provider keys so construction can succeed; clear selection/model env so
// each case controls them explicitly.
void clean_env() {
    ::setenv("OPENAI_API_KEY", "test-openai", 1);
    ::setenv("ANTHROPIC_API_KEY", "test-anthropic", 1);
    ::unsetenv("ASTRITE_PROVIDER");
    ::unsetenv("OPENAI_MODEL");
    ::unsetenv("ANTHROPIC_MODEL");
}
} // namespace

TEST_CASE("factory: builds a named provider with a model override") {
    clean_env();
    auto a = make_named_provider("anthropic", "claude-opus-4-8");
    REQUIRE(a.has_value());
    CHECK((*a)->name() == "anthropic");
    CHECK((*a)->model() == "claude-opus-4-8");

    auto g = make_named_provider("gpt", "gpt-5.2-codex");
    REQUIRE(g.has_value());
    CHECK((*g)->name() == "gpt");
    CHECK((*g)->model() == "gpt-5.2-codex");
}

TEST_CASE("factory: an empty model keeps the provider's default") {
    clean_env();
    auto a = make_named_provider("anthropic", "");
    REQUIRE(a.has_value());
    CHECK((*a)->name() == "anthropic");
    CHECK((*a)->model() == "claude-haiku-4-5-20251001"); // the Anthropic default
}

TEST_CASE("factory: an unknown provider name is an error") {
    clean_env();
    auto r = make_named_provider("bogus", "");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().find("bogus") != std::string::npos);
}

TEST_CASE("factory: a missing key for the named provider is a clear error") {
    clean_env();
    ::unsetenv("ANTHROPIC_API_KEY");
    auto r = make_named_provider("anthropic", "claude-opus-4-8");
    ::setenv("ANTHROPIC_API_KEY", "test-anthropic", 1); // restore for later cases
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().find("ANTHROPIC_API_KEY") != std::string::npos);
}

TEST_CASE("factory: an empty name resolves the provider from the environment") {
    clean_env();
    ::setenv("ASTRITE_PROVIDER", "anthropic", 1);
    auto r = make_named_provider("", "");
    ::unsetenv("ASTRITE_PROVIDER");
    REQUIRE(r.has_value());
    CHECK((*r)->name() == "anthropic");
}

TEST_CASE("factory: make_provider_from_env still honors ASTRITE_PROVIDER") {
    clean_env();
    ::setenv("ASTRITE_PROVIDER", "gpt", 1);
    auto r = make_provider_from_env();
    ::unsetenv("ASTRITE_PROVIDER");
    REQUIRE(r.has_value());
    CHECK((*r)->name() == "gpt");
}
