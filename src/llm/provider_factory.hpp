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

[[nodiscard]] std::expected<std::shared_ptr<Provider>, std::string> make_provider_from_env();

} // namespace astrite::llm
