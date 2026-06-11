#pragma once

// The OpenAI-native provider (GPT/Codex): POST {base}/responses (the Responses
// API), which is distinct from the chat-completions path OpenAiCompatProvider
// uses. It exposes reasoning effort and the Responses streaming-event shape.
// astrite's messages/tools are chat-completions-shaped, so this provider
// translates to Responses `input` items + flat function tools, and folds the
// response.* SSE events back into a CompletedTurn.

#include <expected>
#include <stop_token>
#include <string>
#include <vector>

#include "llm/provider.hpp"
#include "llm/sse.hpp"
#include "schema/value.hpp"

namespace astrite::llm {

struct GptConfig {
    std::string api_key;
    std::string model;
    std::string base_url;
    std::string reasoning_effort; // "", "low", "medium", "high"

    // Reads OPENAI_API_KEY / OPENAI_MODEL / OPENAI_BASE_URL / OPENAI_REASONING_EFFORT.
    [[nodiscard]] static std::expected<GptConfig, std::string> from_env();
    [[nodiscard]] static GptConfig from_env_or(std::string api_key, std::string model,
                                               std::string base_url, std::string reasoning_effort);
};

// Pure translation (testable): chat-completions messages -> Responses `input`
// items, with system text pulled into `instructions_out`, assistant tool_calls
// -> function_call items, and tool results -> function_call_output items.
[[nodiscard]] schema::Value to_responses_input(const schema::Value& messages,
                                               std::string& instructions_out);
// chat-completions tools -> Responses flat function tools.
[[nodiscard]] schema::Value to_responses_tools(const schema::Value& tools);
// Full request body. Reasoning is included only when `effort` is non-empty.
[[nodiscard]] std::string build_responses_body(const std::string& model,
                                               const std::string& effort,
                                               const schema::Value& messages,
                                               const schema::Value& tools);

class GptProvider : public Provider {
public:
    explicit GptProvider(GptConfig cfg) : cfg_(std::move(cfg)) {}

    [[nodiscard]] std::string name() const override { return "gpt"; }
    [[nodiscard]] std::string model() const override { return cfg_.model; }

    [[nodiscard]] std::expected<CompletedTurn, std::string>
    stream_chat(const schema::Value& messages, const schema::Value& tools, DeltaSink on_delta,
                std::stop_token cancel) override;

    [[nodiscard]] std::expected<std::vector<ModelInfo>, std::string> list_models() override;
    [[nodiscard]] int context_limit(const std::string& model_id) const override;

private:
    GptConfig cfg_;
};

} // namespace astrite::llm
