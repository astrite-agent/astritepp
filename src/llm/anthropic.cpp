#include "llm/anthropic.hpp"

#include <cstdlib>
#include <map>
#include <optional>
#include <utility>

#include <glaze/json.hpp>

#include "llm/http.hpp"
#include "schema/value_json.hpp"

namespace astrite::llm {

using schema::Value;

std::expected<AnthropicConfig, std::string> AnthropicConfig::from_env() {
    const char* key = std::getenv("ANTHROPIC_API_KEY");
    if (key == nullptr || *key == '\0') {
        return std::unexpected("ANTHROPIC_API_KEY is not set");
    }
    const char* model = std::getenv("ANTHROPIC_MODEL");
    const char* base = std::getenv("ANTHROPIC_BASE_URL");
    const char* max_tok = std::getenv("ANTHROPIC_MAX_TOKENS");
    int max_tokens = max_tok != nullptr ? std::atoi(max_tok) : 0;
    return from_env_or(key, model != nullptr ? model : "", base != nullptr ? base : "", max_tokens);
}

AnthropicConfig AnthropicConfig::from_env_or(std::string api_key, std::string model,
                                             std::string base_url, int max_tokens) {
    AnthropicConfig cfg;
    cfg.api_key = std::move(api_key);
    cfg.model = model.empty() ? "claude-haiku-4-5-20251001" : std::move(model);
    cfg.base_url = base_url.empty() ? "https://api.anthropic.com/v1" : std::move(base_url);
    while (cfg.base_url.ends_with('/')) {
        cfg.base_url.pop_back();
    }
    cfg.max_tokens = max_tokens > 0 ? max_tokens : 4096;
    return cfg;
}

namespace {

Value ephemeral() {
    Value cc = Value::object();
    cc.set("type", Value::str("ephemeral"));
    return cc;
}

// Ensure a message's content is a block array and mark its last block cacheable.
void add_cache_control(Value& msg) {
    const Value* c = msg.get("content");
    Value::List blocks;
    if (c != nullptr && c->as_list() != nullptr) {
        blocks = *c->as_list();
    } else {
        Value tb = Value::object();
        tb.set("type", Value::str("text"));
        tb.set("text", c != nullptr && c->as_str() != nullptr ? *c : Value::str(""));
        blocks.push_back(std::move(tb));
    }
    if (blocks.empty()) {
        return;
    }
    blocks.back().set("cache_control", ephemeral());
    msg.set("content", Value::list(std::move(blocks)));
}

const std::string* str_of(const Value* v) {
    return v != nullptr ? v->as_str() : nullptr;
}

} // namespace

Value to_anthropic_messages(const Value& messages, std::string& system_out, bool cache) {
    Value::List out;
    Value::List pending; // coalesced tool_result blocks -> one user turn

    auto flush = [&] {
        if (pending.empty()) {
            return;
        }
        Value m = Value::object();
        m.set("role", Value::str("user"));
        m.set("content", Value::list(std::move(pending)));
        pending.clear();
        out.push_back(std::move(m));
    };

    const Value::List* list = messages.as_list();
    if (list == nullptr) {
        return Value::list({});
    }
    for (const Value& msg : *list) {
        const std::string* role = str_of(msg.get("role"));
        if (role == nullptr) {
            continue;
        }
        if (*role == "system") {
            if (const std::string* c = str_of(msg.get("content"))) {
                system_out += (system_out.empty() ? "" : "\n\n") + *c;
            }
            continue;
        }
        if (*role == "tool") {
            Value b = Value::object();
            b.set("type", Value::str("tool_result"));
            if (const Value* id = msg.get("tool_call_id")) {
                b.set("tool_use_id", *id);
            }
            const Value* content = msg.get("content");
            b.set("content", content != nullptr ? *content : Value::str(""));
            pending.push_back(std::move(b));
            continue;
        }
        flush();
        if (*role == "user") {
            Value m = Value::object();
            m.set("role", Value::str("user"));
            const Value* content = msg.get("content");
            m.set("content", content != nullptr ? *content : Value::str(""));
            out.push_back(std::move(m));
        } else if (*role == "assistant") {
            Value::List blocks;
            if (const std::string* c = str_of(msg.get("content")); c != nullptr && !c->empty()) {
                Value tb = Value::object();
                tb.set("type", Value::str("text"));
                tb.set("text", Value::str(*c));
                blocks.push_back(std::move(tb));
            }
            if (const Value* tcs = msg.get("tool_calls"); tcs != nullptr && tcs->as_list()) {
                for (const Value& tc : *tcs->as_list()) {
                    const Value* fn = tc.get("function");
                    Value ub = Value::object();
                    ub.set("type", Value::str("tool_use"));
                    if (const Value* id = tc.get("id")) {
                        ub.set("id", *id);
                    }
                    if (fn != nullptr) {
                        if (const Value* n = fn->get("name")) {
                            ub.set("name", *n);
                        }
                    }
                    Value input = Value::object();
                    if (fn != nullptr) {
                        if (const std::string* args = str_of(fn->get("arguments"))) {
                            if (auto parsed = schema::parse_json(*args);
                                parsed.has_value() && parsed->is_object()) {
                                input = std::move(*parsed);
                            }
                        }
                    }
                    ub.set("input", std::move(input));
                    blocks.push_back(std::move(ub));
                }
            }
            Value m = Value::object();
            m.set("role", Value::str("assistant"));
            m.set("content", Value::list(std::move(blocks)));
            out.push_back(std::move(m));
        }
    }
    flush();
    if (cache && !out.empty()) {
        add_cache_control(out.back());
    }
    return Value::list(std::move(out));
}

Value to_anthropic_tools(const Value& tools) {
    Value::List out;
    if (const Value::List* list = tools.as_list()) {
        for (const Value& tool : *list) {
            const Value* fn = tool.get("function");
            if (fn == nullptr) {
                continue;
            }
            Value t = Value::object();
            if (const Value* n = fn->get("name")) {
                t.set("name", *n);
            }
            if (const Value* d = fn->get("description")) {
                t.set("description", *d);
            }
            if (const Value* params = fn->get("parameters")) {
                t.set("input_schema", *params);
            } else {
                Value schema = Value::object();
                schema.set("type", Value::str("object"));
                schema.set("properties", Value::object());
                t.set("input_schema", std::move(schema));
            }
            out.push_back(std::move(t));
        }
    }
    return Value::list(std::move(out));
}

std::string build_anthropic_body(const std::string& model, int max_tokens, const Value& messages,
                                 const Value& tools, bool cache) {
    Value req = Value::object();
    req.set("model", Value::str(model));
    req.set("max_tokens", Value::integer(max_tokens));
    req.set("stream", Value::boolean(true));

    std::string system;
    req.set("messages", to_anthropic_messages(messages, system, cache));
    if (!system.empty()) {
        if (cache) {
            Value block = Value::object();
            block.set("type", Value::str("text"));
            block.set("text", Value::str(system));
            block.set("cache_control", ephemeral());
            req.set("system", Value::list({std::move(block)}));
        } else {
            req.set("system", Value::str(system));
        }
    }
    if (Value atools = to_anthropic_tools(tools);
        atools.as_list() != nullptr && !atools.as_list()->empty()) {
        req.set("tools", std::move(atools));
    }
    return schema::to_json(req);
}

// ---- streaming SSE fold -------------------------------------------------------

namespace anth_wire {
struct Usage {
    std::optional<int> input_tokens;
    std::optional<int> output_tokens;
    std::optional<int> cache_creation_input_tokens;
    std::optional<int> cache_read_input_tokens;
};
struct Delta {
    std::optional<std::string> type;
    std::optional<std::string> text;
    std::optional<std::string> partial_json;
    std::optional<std::string> stop_reason;
};
struct ContentBlock {
    std::optional<std::string> type;
    std::optional<std::string> id;
    std::optional<std::string> name;
};
struct MessageInfo {
    std::optional<Usage> usage;
};
struct Event {
    std::optional<std::string> type;
    std::optional<int> index;
    std::optional<Delta> delta;
    std::optional<ContentBlock> content_block;
    std::optional<MessageInfo> message;
    std::optional<Usage> usage;
};
} // namespace anth_wire

namespace {

class AnthropicAccumulator {
public:
    explicit AnthropicAccumulator(DeltaSink on_delta) : on_delta_(std::move(on_delta)) {}

    void feed(std::string_view bytes) {
        buffer_.append(bytes);
        std::size_t nl = 0;
        while ((nl = buffer_.find('\n')) != std::string::npos) {
            std::string_view line(buffer_.data(), nl);
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
            process_line(line);
            buffer_.erase(0, nl + 1);
        }
    }

    [[nodiscard]] CompletedTurn settle() const {
        CompletedTurn turn;
        turn.text = text_;
        turn.finish_reason = finish_;
        turn.usage = usage_;
        turn.usage.total_tokens = usage_.prompt_tokens + usage_.completion_tokens;
        for (const auto& [index, call] : calls_) {
            if (call.has_name) {
                turn.tool_calls.push_back({call.id, call.name, call.arguments});
            }
        }
        return turn;
    }

private:
    struct Accum {
        std::string id;
        std::string name;
        std::string arguments;
        bool has_name = false;
    };

    void process_line(std::string_view line) {
        constexpr std::string_view prefix = "data:";
        if (!line.starts_with(prefix)) {
            return; // `event:` lines and blanks; the data JSON carries its own type
        }
        std::string_view payload = line.substr(prefix.size());
        if (payload.starts_with(' ')) {
            payload.remove_prefix(1);
        }
        anth_wire::Event ev{};
        if (glz::read<glz::opts{.error_on_unknown_keys = false}>(ev, payload)) {
            return; // tolerate malformed events
        }
        apply(ev);
    }

    void apply(const anth_wire::Event& ev) {
        const std::string type = ev.type.value_or("");
        if (type == "message_start" && ev.message.has_value() && ev.message->usage.has_value()) {
            const anth_wire::Usage& u = *ev.message->usage;
            usage_.cache_read_tokens = u.cache_read_input_tokens.value_or(0);
            usage_.cache_write_tokens = u.cache_creation_input_tokens.value_or(0);
            usage_.prompt_tokens =
                u.input_tokens.value_or(0) + usage_.cache_read_tokens + usage_.cache_write_tokens;
            usage_.present = true;
        } else if (type == "content_block_start" && ev.content_block.has_value() &&
                   ev.content_block->type.value_or("") == "tool_use") {
            Accum& a = calls_[ev.index.value_or(0)];
            a.id = ev.content_block->id.value_or("");
            a.name = ev.content_block->name.value_or("");
            a.has_name = !a.name.empty();
        } else if (type == "content_block_delta" && ev.delta.has_value()) {
            const anth_wire::Delta& d = *ev.delta;
            if (d.type.value_or("") == "text_delta" && d.text.has_value()) {
                text_ += *d.text;
                if (on_delta_) {
                    on_delta_(*d.text);
                }
            } else if (d.type.value_or("") == "input_json_delta" && d.partial_json.has_value()) {
                calls_[ev.index.value_or(0)].arguments += *d.partial_json;
            }
        } else if (type == "message_delta") {
            if (ev.usage.has_value() && ev.usage->output_tokens.has_value()) {
                usage_.completion_tokens = *ev.usage->output_tokens;
                usage_.present = true;
            }
            if (ev.delta.has_value() && ev.delta->stop_reason.has_value()) {
                finish_ = *ev.delta->stop_reason;
            }
        }
    }

    DeltaSink on_delta_;
    std::string buffer_;
    std::string text_;
    std::string finish_;
    Usage usage_;
    std::map<int, Accum> calls_;
};

} // namespace

std::expected<CompletedTurn, std::string>
AnthropicProvider::stream_chat(const Value& messages, const Value& tools, DeltaSink on_delta,
                               std::stop_token cancel) {
    const std::string url = cfg_.base_url + "/messages";
    const std::string body =
        build_anthropic_body(cfg_.model, cfg_.max_tokens, messages, tools, /*cache=*/true);
    const std::vector<std::string> headers = {
        "Content-Type: application/json",
        "x-api-key: " + cfg_.api_key,
        "anthropic-version: 2023-06-01",
        "Accept: text/event-stream",
    };

    AnthropicAccumulator acc(std::move(on_delta));
    http::StreamResult r = http::stream_post(
        url, headers, body, [&](std::string_view chunk) { acc.feed(chunk); }, std::move(cancel));

    if (!r.ok) {
        return std::unexpected(r.error);
    }
    if (r.status != 200) {
        return std::unexpected("HTTP " + std::to_string(r.status) + ": " + r.tail);
    }
    return acc.settle();
}

namespace anth_wire {
struct ModelEntry {
    std::string id;
};
struct ModelList {
    std::vector<ModelEntry> data;
};
} // namespace anth_wire

std::expected<std::vector<ModelInfo>, std::string> AnthropicProvider::list_models() {
    const std::vector<std::string> headers = {
        "x-api-key: " + cfg_.api_key,
        "anthropic-version: 2023-06-01",
    };
    auto body = http::get(cfg_.base_url + "/models", headers);
    if (!body.has_value()) {
        return std::unexpected(body.error());
    }
    anth_wire::ModelList parsed;
    if (glz::read<glz::opts{.error_on_unknown_keys = false}>(parsed, *body)) {
        return std::unexpected("could not parse model list");
    }
    std::vector<ModelInfo> out;
    out.reserve(parsed.data.size());
    for (const anth_wire::ModelEntry& e : parsed.data) {
        out.push_back({e.id, context_limit(e.id)});
    }
    return out;
}

int AnthropicProvider::context_limit(const std::string& model_id) const {
    return model_id.starts_with("claude-") ? 200000 : 0;
}

} // namespace astrite::llm
