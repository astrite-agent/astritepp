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

// Resolve an empty kind from the environment: ASTRITE_PROVIDER, or inference.
std::string resolve_kind_from_env() {
    const char* selected = std::getenv("ASTRITE_PROVIDER");
    std::string kind = selected != nullptr ? selected : "";
    if (kind.empty()) {
        // Infer: Anthropic key only -> anthropic; otherwise the compat default.
        kind = (env_set("ANTHROPIC_API_KEY") && !env_set("OPENAI_API_KEY")) ? "anthropic"
                                                                            : "openai-compat";
    }
    return kind;
}
} // namespace

std::expected<std::shared_ptr<Provider>, std::string>
make_named_provider(const std::string& kind, const std::string& model) {
    const std::string k = kind.empty() ? resolve_kind_from_env() : kind;

    if (k == "openai-compat" || k == "openai") {
        auto cfg = Config::from_env();
        if (!cfg.has_value()) {
            return std::unexpected(cfg.error());
        }
        if (!model.empty()) {
            cfg->model = model;
        }
        return std::make_shared<OpenAiCompatProvider>(std::move(*cfg));
    }
    if (k == "anthropic" || k == "claude") {
        auto cfg = AnthropicConfig::from_env();
        if (!cfg.has_value()) {
            return std::unexpected(cfg.error());
        }
        if (!model.empty()) {
            cfg->model = model;
        }
        return std::make_shared<AnthropicProvider>(std::move(*cfg));
    }
    if (k == "gpt" || k == "codex" || k == "responses") {
        auto cfg = GptConfig::from_env();
        if (!cfg.has_value()) {
            return std::unexpected(cfg.error());
        }
        if (!model.empty()) {
            cfg->model = model;
        }
        return std::make_shared<GptProvider>(std::move(*cfg));
    }
    return std::unexpected("unknown provider `" + k + "`");
}

std::expected<std::shared_ptr<Provider>, std::string> make_provider_from_env() {
    return make_named_provider("", "");
}

} // namespace astrite::llm
