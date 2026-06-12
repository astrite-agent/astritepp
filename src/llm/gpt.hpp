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
#include <string_view>
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
    bool reasoning_summary = false; // request a reasoning summary to display

    // Reads OPENAI_API_KEY / OPENAI_MODEL / OPENAI_BASE_URL / OPENAI_REASONING_EFFORT /
    // OPENAI_REASONING_SUMMARY.
    [[nodiscard]] static std::expected<GptConfig, std::string> from_env();
    [[nodiscard]] static GptConfig from_env_or(std::string api_key, std::string model,
                                               std::string base_url, std::string reasoning_effort,
                                               bool reasoning_summary = false);
};

// Pure translation (testable): chat-completions messages -> Responses `input`
// items, with system text pulled into `instructions_out`, assistant tool_calls
// -> function_call items, and tool results -> function_call_output items.
[[nodiscard]] schema::Value to_responses_input(const schema::Value& messages,
                                               std::string& instructions_out);
// chat-completions tools -> Responses flat function tools.
[[nodiscard]] schema::Value to_responses_tools(const schema::Value& tools);
// Full request body. Reasoning is included only when `effort` is non-empty; when
// `summary` is also set, a reasoning summary is requested (display only — the
// Responses reasoning items are not yet round-tripped across tool calls).
[[nodiscard]] std::string build_responses_body(const std::string& model,
                                               const std::string& effort,
                                               const schema::Value& messages,
                                               const schema::Value& tools, bool summary = false);

// Fold a complete Responses SSE byte stream into a CompletedTurn (test seam over
// the streaming accumulator). The reasoning summary is captured into `reasoning`
// (display text, provider "gpt") and never into `text` or `on_delta`.
[[nodiscard]] CompletedTurn fold_responses_sse(std::string_view sse,
                                               const DeltaSink& on_delta = {});

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
