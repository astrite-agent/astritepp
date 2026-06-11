#include "llm/provider_factory.hpp"

#include <cstdlib>

#include "llm/anthropic.hpp"
#include "llm/gpt.hpp"
#include "llm/openai_compat.hpp"

namespace astrite::llm {

namespace {
bool env_set(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && *v != '\0';
}
} // namespace

std::expected<std::shared_ptr<Provider>, std::string> make_provider_from_env() {
    const char* selected = std::getenv("ASTRITE_PROVIDER");
    std::string kind = selected != nullptr ? selected : "";
    if (kind.empty()) {
        // Infer: Anthropic key only -> anthropic; otherwise the compat default.
        kind = (env_set("ANTHROPIC_API_KEY") && !env_set("OPENAI_API_KEY")) ? "anthropic"
                                                                            : "openai-compat";
    }

    if (kind == "openai-compat" || kind == "openai") {
        auto cfg = Config::from_env();
        if (!cfg.has_value()) {
            return std::unexpected(cfg.error());
        }
        return std::make_shared<OpenAiCompatProvider>(std::move(*cfg));
    }
    if (kind == "anthropic" || kind == "claude") {
        auto cfg = AnthropicConfig::from_env();
        if (!cfg.has_value()) {
            return std::unexpected(cfg.error());
        }
        return std::make_shared<AnthropicProvider>(std::move(*cfg));
    }
    if (kind == "gpt" || kind == "codex" || kind == "responses") {
        auto cfg = GptConfig::from_env();
        if (!cfg.has_value()) {
            return std::unexpected(cfg.error());
        }
        return std::make_shared<GptProvider>(std::move(*cfg));
    }
    return std::unexpected("unknown ASTRITE_PROVIDER `" + kind + "`");
}

} // namespace astrite::llm
