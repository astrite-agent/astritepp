#pragma once

// The model backend seam. A Provider turns a turn's messages + tools into a
// settled CompletedTurn, streaming text deltas live and honoring a stop_token.
// Concrete providers (OpenAI-compatible, OpenAI-native GPT/Codex, Anthropic)
// each own their wire format and SSE fold but share the http transport and the
// CompletedTurn/Usage/ModelInfo types. The runtime only ever sees this seam.

#include <expected>
#include <stop_token>
#include <string>
#include <vector>

#include "llm/sse.hpp"
#include "schema/value.hpp"

namespace astrite::llm {

class Provider {
public:
    virtual ~Provider() = default;

    // A short identifier for the backend ("openai-compat", "gpt", "anthropic").
    [[nodiscard]] virtual std::string name() const = 0;
    // The active model id.
    [[nodiscard]] virtual std::string model() const = 0;
    // The endpoint base URL (for display / diagnostics).
    [[nodiscard]] virtual std::string base_url() const = 0;

    // Blocking: stream one assistant turn. Transport failures, non-2xx statuses,
    // and cancellation are errors; a settled turn (text/tool_calls/usage) is success.
    [[nodiscard]] virtual std::expected<CompletedTurn, std::string>
    stream_chat(const schema::Value& messages, const schema::Value& tools, DeltaSink on_delta,
                std::stop_token cancel) = 0;

    // The provider's model catalog (best-effort; may be a static table).
    [[nodiscard]] virtual std::expected<std::vector<ModelInfo>, std::string> list_models() = 0;
    // The context window for a model id, or 0 if unknown.
    [[nodiscard]] virtual int context_limit(const std::string& model_id) const = 0;
};

} // namespace astrite::llm
