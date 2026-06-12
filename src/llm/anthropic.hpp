#pragma once

// The Anthropic provider: POST {base}/messages (Messages API), streaming SSE
// content blocks. astrite's internal message/tool shape is OpenAI-flavored, so
// this provider translates both ways — OpenAI messages/tools -> Anthropic
// request, Anthropic content blocks -> the internal CompletedTurn — and marks
// cache_control on the stable prefix so repeated context is served from cache.

#include <expected>
#include <stop_token>
#include <string>
#include <string_view>
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
    int thinking_budget = 0; // >0 enables extended thinking with that token budget

    // Reads ANTHROPIC_API_KEY / ANTHROPIC_MODEL / ANTHROPIC_BASE_URL /
    // ANTHROPIC_MAX_TOKENS / ANTHROPIC_THINKING_BUDGET; empty key is an error.
    [[nodiscard]] static std::expected<AnthropicConfig, std::string> from_env();
    [[nodiscard]] static AnthropicConfig from_env_or(std::string api_key, std::string model,
                                                     std::string base_url, int max_tokens,
                                                     int thinking_budget);
};

// Pure translation (testable without the network):
// Convert OpenAI-shaped messages into Anthropic {system, messages}. System
// messages are pulled into `system_out`; consecutive tool results coalesce into
// one user turn of tool_result blocks; assistant tool_calls become tool_use
// blocks (arguments parsed to an object). cache_control is placed on the system
// and the last content block when `cache` is set.
//
// Reasoning round-trip: when `thinking` is on and an assistant message carries a
// `_reasoning` sidecar produced by anthropic on this same `model`, its opaque
// thinking blocks are re-emitted *first* in the assistant turn (Anthropic
// requires the thinking block to precede tool_use). A model switch or disabled
// thinking drops them — exactly the API's own contract.
[[nodiscard]] schema::Value to_anthropic_messages(const schema::Value& messages,
                                                  std::string& system_out, bool cache,
                                                  const std::string& model = "",
                                                  bool thinking = false);
// Convert OpenAI tool definitions to Anthropic tools ({name, description, input_schema}).
[[nodiscard]] schema::Value to_anthropic_tools(const schema::Value& tools);
// Full request body. A `thinking_budget` > 0 enables extended thinking (and lifts
// max_tokens above the budget, which the API requires).
[[nodiscard]] std::string build_anthropic_body(const std::string& model, int max_tokens,
                                               const schema::Value& messages,
                                               const schema::Value& tools, bool cache,
                                               int thinking_budget = 0);

// Fold a complete Anthropic SSE byte stream into a CompletedTurn (a test seam
// over the streaming accumulator; the network path uses the same fold). Thinking
// is captured into `reasoning` (text + opaque blocks), never into `text`, and is
// not forwarded to `on_delta`.
[[nodiscard]] CompletedTurn fold_anthropic_sse(std::string_view sse, const DeltaSink& on_delta = {});

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
