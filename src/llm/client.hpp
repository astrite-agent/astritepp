#pragma once

// OpenAI-compatible streaming client over system libcurl: blocking easy-handle,
// write callback feeding the SSE accumulator, cancellation via the progress
// callback checking a stop_token. No async runtime (DESIGN §8).

#include <expected>
#include <stop_token>
#include <string>
#include <string_view>

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

class Client {
public:
    explicit Client(Config cfg) : cfg_(std::move(cfg)) {}

    // Blocking POST {base}/chat/completions with stream:true. Returns the settled
    // turn; transport failures, non-200 statuses, and cancellation are errors.
    [[nodiscard]] std::expected<CompletedTurn, std::string>
    stream_chat(const schema::Value& messages, const schema::Value& tools,
                StreamAccumulator::DeltaSink on_delta = {}, std::stop_token cancel = {});

private:
    Config cfg_;
};

} // namespace astrite::llm
