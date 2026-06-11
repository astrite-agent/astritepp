#pragma once

// The streaming sharp edge, kept pure and fixture-testable: an SSE line
// accumulator plus index-keyed tool-call fragment accumulation. The index is
// untrusted — it is only ever a map key, never an allocation size; id/name
// overwrite, arguments append; distinct calls are capped; incomplete entries
// (no name) are filtered on settle. Malformed chunks are tolerated, not fatal:
// the stream is best-effort, the *settled* value is what the kernel trusts.

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glaze/json.hpp>

#include "schema/value.hpp"

namespace astrite::llm {

// A live token sink for streamed assistant text, shared by every provider.
using DeltaSink = std::function<void(std::string_view)>;

struct ToolCall {
    std::string id;
    std::string name;
    std::string arguments; // raw JSON string, untrusted until json-coerced downstream
};

struct Usage {
    int prompt_tokens = 0;
    int completion_tokens = 0;
    int total_tokens = 0;
    int cache_read_tokens = 0;  // prompt tokens served from cache (cheap)
    int cache_write_tokens = 0; // prompt tokens written to cache this call
    bool present = false;       // true once the server reported usage
};

struct CompletedTurn {
    std::string text;
    std::vector<ToolCall> tool_calls;
    std::string finish_reason;
    Usage usage;
};

// One entry in a provider's model catalog. `context_limit` is 0 when unknown.
struct ModelInfo {
    std::string id;
    int context_limit = 0;
};

namespace wire {

// Streamed chunk shapes (OpenAI-compatible). Everything optional; unknown
// fields are tolerated for cross-provider drift. glaze reads these by aggregate
// reflection — member names are the JSON keys.
struct FnDelta {
    std::optional<std::string> name;
    std::optional<std::string> arguments;
};
struct ToolCallDelta {
    std::optional<std::int64_t> index;
    std::optional<std::string> id;
    std::optional<std::string> type;
    std::optional<FnDelta> function;
};
struct Delta {
    std::optional<std::string> role;
    std::optional<std::string> content;
    std::optional<std::vector<ToolCallDelta>> tool_calls;
};
struct Choice {
    std::optional<Delta> delta;
    std::optional<std::string> finish_reason;
    std::optional<std::int64_t> index;
};
struct PromptTokensDetails {
    std::optional<int> cached_tokens; // OpenAI automatic prompt caching
};
struct UsageWire {
    std::optional<int> prompt_tokens;
    std::optional<int> completion_tokens;
    std::optional<int> total_tokens;
    std::optional<PromptTokensDetails> prompt_tokens_details;
};
struct Chunk {
    std::optional<std::vector<Choice>> choices;
    std::optional<UsageWire> usage; // final chunk when stream_options.include_usage
};

} // namespace wire

class StreamAccumulator {
public:
    using DeltaSink = llm::DeltaSink; // alias kept for existing call sites

    StreamAccumulator() = default;
    explicit StreamAccumulator(DeltaSink on_delta) : on_delta_(std::move(on_delta)) {}

    // Feed raw bytes as they arrive; lines may split anywhere.
    void feed(std::string_view bytes) {
        buffer_.append(bytes);
        size_t newline = 0;
        while ((newline = buffer_.find('\n')) != std::string::npos) {
            std::string_view line(buffer_.data(), newline);
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
            process_line(line);
            buffer_.erase(0, newline + 1);
        }
    }

    [[nodiscard]] CompletedTurn settle() const {
        CompletedTurn turn;
        turn.text = text_;
        turn.finish_reason = finish_reason_;
        turn.usage = usage_;
        for (const auto& [index, accum] : calls_) {
            if (accum.has_name) {
                turn.tool_calls.push_back({accum.id, accum.name, accum.arguments});
            }
        }
        return turn;
    }

private:
    static constexpr size_t MAX_DISTINCT_CALLS = 64;

    void process_line(std::string_view line) {
        constexpr std::string_view prefix = "data:";
        if (!line.starts_with(prefix)) {
            return; // SSE comments, event: lines, blanks
        }
        std::string_view payload = line.substr(prefix.size());
        if (payload.starts_with(' ')) {
            payload.remove_prefix(1);
        }
        if (payload == "[DONE]") {
            done_ = true;
            return;
        }
        apply_chunk(payload);
    }

    void apply_chunk(std::string_view payload) {
        wire::Chunk chunk{};
        if (glz::read<glz::opts{.error_on_unknown_keys = false}>(chunk, payload)) {
            return; // tolerate malformed chunks; the settled value is the contract
        }
        // Usage arrives in a final chunk that often has empty choices, so read it
        // before the choices early-return.
        if (chunk.usage.has_value()) {
            const wire::UsageWire& u = *chunk.usage;
            usage_.prompt_tokens = u.prompt_tokens.value_or(usage_.prompt_tokens);
            usage_.completion_tokens = u.completion_tokens.value_or(usage_.completion_tokens);
            usage_.total_tokens = u.total_tokens.value_or(usage_.total_tokens);
            if (u.prompt_tokens_details.has_value()) {
                usage_.cache_read_tokens =
                    u.prompt_tokens_details->cached_tokens.value_or(usage_.cache_read_tokens);
            }
            usage_.present = true;
        }
        if (!chunk.choices.has_value()) {
            return;
        }
        for (const wire::Choice& choice : *chunk.choices) {
            if (choice.finish_reason.has_value()) {
                finish_reason_ = *choice.finish_reason;
            }
            if (!choice.delta.has_value()) {
                continue;
            }
            const wire::Delta& delta = *choice.delta;
            if (delta.content.has_value() && !delta.content->empty()) {
                text_ += *delta.content;
                if (on_delta_) {
                    on_delta_(*delta.content);
                }
            }
            if (delta.tool_calls.has_value()) {
                for (const wire::ToolCallDelta& tcd : *delta.tool_calls) {
                    const std::int64_t index = tcd.index.value_or(0);
                    if (!calls_.contains(index) && calls_.size() >= MAX_DISTINCT_CALLS) {
                        continue; // cap distinct calls; never trust the index for sizing
                    }
                    Accum& accum = calls_[index];
                    if (tcd.id.has_value()) {
                        accum.id = *tcd.id;
                    }
                    if (tcd.function.has_value()) {
                        if (tcd.function->name.has_value()) {
                            accum.name = *tcd.function->name;
                            accum.has_name = true;
                        }
                        if (tcd.function->arguments.has_value()) {
                            accum.arguments += *tcd.function->arguments;
                        }
                    }
                }
            }
        }
    }

    struct Accum {
        std::string id;
        std::string name;
        std::string arguments;
        bool has_name = false;
    };

    DeltaSink on_delta_;
    std::string buffer_;
    std::string text_;
    std::map<std::int64_t, Accum> calls_;
    std::string finish_reason_;
    Usage usage_;
    bool done_ = false;
};

// The settled assistant message as a session record. Pure tool-call turns carry
// content: null, matching the OpenAI message shape.
[[nodiscard]] inline schema::Value assistant_message(const CompletedTurn& turn) {
    using schema::Value;
    Value msg = Value::object();
    msg.set("role", Value::str("assistant"));
    if (turn.text.empty() && !turn.tool_calls.empty()) {
        msg.set("content", Value());
    } else {
        msg.set("content", Value::str(turn.text));
    }
    if (!turn.tool_calls.empty()) {
        Value::List calls;
        calls.reserve(turn.tool_calls.size());
        for (const ToolCall& call : turn.tool_calls) {
            Value fn = Value::object();
            fn.set("name", Value::str(call.name));
            fn.set("arguments", Value::str(call.arguments));
            Value entry = Value::object();
            entry.set("id", Value::str(call.id));
            entry.set("type", Value::str("function"));
            entry.set("function", std::move(fn));
            calls.push_back(std::move(entry));
        }
        msg.set("tool_calls", Value::list(std::move(calls)));
    }
    return msg;
}

} // namespace astrite::llm
