#pragma once

// The Anthropic provider: POST {base}/messages (Messages API), streaming SSE
// content blocks. astrite's internal message/tool shape is OpenAI-flavored, so
// this provider translates both ways — OpenAI messages/tools -> Anthropic
// request, Anthropic content blocks -> the internal CompletedTurn — and marks
// cache_control on the stable prefix so repeated context is served from cache.

#include <expected>
#include <stop_token>
#include <string>
#include <vector>

#include "llm/provider.hpp"
#include "llm/sse.hpp"
#include "schema/value.hpp"

namespace astrite::llm {

struct AnthropicConfig {
    std::string api_key;
    std::string model;
    std::string base_url;
    int max_tokens = 4096;

    // Reads ANTHROPIC_API_KEY / ANTHROPIC_MODEL / ANTHROPIC_BASE_URL /
    // ANTHROPIC_MAX_TOKENS; empty key is an error.
    [[nodiscard]] static std::expected<AnthropicConfig, std::string> from_env();
    [[nodiscard]] static AnthropicConfig from_env_or(std::string api_key, std::string model,
                                                     std::string base_url, int max_tokens);
};

// Pure translation (testable without the network):
// Convert OpenAI-shaped messages into Anthropic {system, messages}. System
// messages are pulled into `system_out`; consecutive tool results coalesce into
// one user turn of tool_result blocks; assistant tool_calls become tool_use
// blocks (arguments parsed to an object). cache_control is placed on the system
// and the last content block when `cache` is set.
[[nodiscard]] schema::Value to_anthropic_messages(const schema::Value& messages,
                                                  std::string& system_out, bool cache);
// Convert OpenAI tool definitions to Anthropic tools ({name, description, input_schema}).
[[nodiscard]] schema::Value to_anthropic_tools(const schema::Value& tools);
// Full request body.
[[nodiscard]] std::string build_anthropic_body(const std::string& model, int max_tokens,
                                               const schema::Value& messages,
                                               const schema::Value& tools, bool cache);

class AnthropicProvider : public Provider {
public:
    explicit AnthropicProvider(AnthropicConfig cfg) : cfg_(std::move(cfg)) {}

    [[nodiscard]] std::string name() const override { return "anthropic"; }
    [[nodiscard]] std::string model() const override { return cfg_.model; }

    [[nodiscard]] std::expected<CompletedTurn, std::string>
    stream_chat(const schema::Value& messages, const schema::Value& tools, DeltaSink on_delta,
                std::stop_token cancel) override;

    [[nodiscard]] std::expected<std::vector<ModelInfo>, std::string> list_models() override;
    [[nodiscard]] int context_limit(const std::string& model_id) const override;

private:
    AnthropicConfig cfg_;
};

} // namespace astrite::llm
