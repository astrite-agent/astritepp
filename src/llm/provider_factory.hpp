#pragma once

// Select and construct a Provider from the environment. ASTRITE_PROVIDER picks
// the backend (openai-compat | gpt | anthropic); absent, it defaults to
// openai-compat, or infers anthropic when only ANTHROPIC_API_KEY is present.
// Each backend reads its own *_API_KEY / *_MODEL / *_BASE_URL.

#include <expected>
#include <memory>
#include <string>

#include "llm/provider.hpp"

namespace astrite::llm {

// Build a provider by explicit kind ("openai-compat"|"openai"|"gpt"|"codex"|
// "responses"|"anthropic"|"claude"); an empty kind resolves from the environment
// (ASTRITE_PROVIDER, or inference). Keys/base URLs always come from the
// environment; a non-empty `model` overrides the provider's env/default model.
// Errors: unknown kind, or a missing key for the chosen backend.
[[nodiscard]] std::expected<std::shared_ptr<Provider>, std::string>
make_named_provider(const std::string& kind, const std::string& model);

[[nodiscard]] std::expected<std::shared_ptr<Provider>, std::string> make_provider_from_env();

} // namespace astrite::llm
