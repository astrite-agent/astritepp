#pragma once

// The OpenAI-compatible provider: POST {base}/chat/completions with stream:true,
// SSE deltas folded by StreamAccumulator. Works against any chat-completions
// endpoint (OpenAI, local servers, gateways). OpenAI-native features (Responses
// API, reasoning) live in the separate GptProvider.

#include <expected>
#include <stop_token>
#include <string>
#include <vector>

#include "llm/provider.hpp"
#include "llm/sse.hpp"
#include "schema/value.hpp"

namespace astrite::llm {

struct Config {
    std::string api_key;
    std::string model;
    std::string base_url;

    // Reads OPENAI_API_KEY / OPENAI_MODEL / OPENAI_BASE_URL; empty key is an error.
    [[nodiscard]] static std::expected<Config, std::string> from_env();
    // Testable core: apply defaults and normalization to explicit values.
    [[nodiscard]] static Config from_env_or(std::string api_key, std::string model,
                                            std::string base_url);
};

// Pure request construction: messages and tools pass through verbatim; an empty
// tools list is omitted entirely (strict servers reject tools: null).
[[nodiscard]] std::string build_request_body(const std::string& model,
                                             const schema::Value& messages,
                                             const schema::Value& tools);

class OpenAiCompatProvider : public Provider {
public:
    explicit OpenAiCompatProvider(Config cfg) : cfg_(std::move(cfg)) {}

    [[nodiscard]] std::string name() const override { return "openai-compat"; }
    [[nodiscard]] std::string model() const override { return cfg_.model; }

    [[nodiscard]] std::expected<CompletedTurn, std::string>
    stream_chat(const schema::Value& messages, const schema::Value& tools, DeltaSink on_delta,
                std::stop_token cancel) override;

    [[nodiscard]] std::expected<std::vector<ModelInfo>, std::string> list_models() override;
    [[nodiscard]] int context_limit(const std::string& model_id) const override;

private:
    Config cfg_;
};

} // namespace astrite::llm
