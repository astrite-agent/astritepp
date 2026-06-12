#include "llm/gpt.hpp"

#include <cstdlib>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <glaze/json.hpp>

#include "llm/http.hpp"
#include "schema/value_json.hpp"

namespace astrite::llm {

using schema::Value;

std::expected<GptConfig, std::string> GptConfig::from_env() {
    const char* key = std::getenv("OPENAI_API_KEY");
    if (key == nullptr || *key == '\0') {
        return std::unexpected("OPENAI_API_KEY is not set");
    }
    const char* model = std::getenv("OPENAI_MODEL");
    const char* base = std::getenv("OPENAI_BASE_URL");
    const char* effort = std::getenv("OPENAI_REASONING_EFFORT");
    const char* summary = std::getenv("OPENAI_REASONING_SUMMARY");
    const bool want_summary = summary != nullptr && *summary != '\0' &&
                              std::string_view(summary) != "0" &&
                              std::string_view(summary) != "false";
    return from_env_or(key, model != nullptr ? model : "", base != nullptr ? base : "",
                       effort != nullptr ? effort : "", want_summary);
}

GptConfig GptConfig::from_env_or(std::string api_key, std::string model, std::string base_url,
                                 std::string reasoning_effort, bool reasoning_summary) {
    GptConfig cfg;
    cfg.api_key = std::move(api_key);
    cfg.model = model.empty() ? "gpt-5.2" : std::move(model);
    cfg.base_url = base_url.empty() ? "https://api.openai.com/v1" : std::move(base_url);
    while (cfg.base_url.ends_with('/')) {
        cfg.base_url.pop_back();
    }
    // Default reasoning models to medium effort; leave others unset.
    if (reasoning_effort.empty() &&
        (cfg.model.starts_with("gpt-5") || cfg.model.starts_with("o3") ||
         cfg.model.starts_with("o4") || cfg.model.starts_with("codex"))) {
        reasoning_effort = "medium";
    }
    cfg.reasoning_effort = std::move(reasoning_effort);
    cfg.reasoning_summary = reasoning_summary;
    return cfg;
}

namespace {
const std::string* str_of(const Value* v) {
    return v != nullptr ? v->as_str() : nullptr;
}

// A simple message item: {role, content:[{type, text}]}.
Value message_item(const std::string& role, const std::string& part_type, const std::string& text) {
    Value part = Value::object();
    part.set("type", Value::str(part_type));
    part.set("text", Value::str(text));
    Value m = Value::object();
    m.set("type", Value::str("message"));
    m.set("role", Value::str(role));
    m.set("content", Value::list({std::move(part)}));
    return m;
}
} // namespace

Value to_responses_input(const Value& messages, std::string& instructions_out) {
    Value::List out;
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
                instructions_out += (instructions_out.empty() ? "" : "\n\n") + *c;
            }
            continue;
        }
        if (*role == "tool") {
            Value item = Value::object();
            item.set("type", Value::str("function_call_output"));
            if (const Value* id = msg.get("tool_call_id")) {
                item.set("call_id", *id);
            }
            const std::string* c = str_of(msg.get("content"));
            item.set("output", Value::str(c != nullptr ? *c : ""));
            out.push_back(std::move(item));
            continue;
        }
        if (*role == "user") {
            out.push_back(message_item("user", "input_text",
                                       str_of(msg.get("content")) != nullptr
                                           ? *str_of(msg.get("content"))
                                           : ""));
        } else if (*role == "assistant") {
            if (const std::string* c = str_of(msg.get("content")); c != nullptr && !c->empty()) {
                out.push_back(message_item("assistant", "output_text", *c));
            }
            if (const Value* tcs = msg.get("tool_calls"); tcs != nullptr && tcs->as_list()) {
                for (const Value& tc : *tcs->as_list()) {
                    const Value* fn = tc.get("function");
                    Value item = Value::object();
                    item.set("type", Value::str("function_call"));
                    if (const Value* id = tc.get("id")) {
                        item.set("call_id", *id);
                    }
                    if (fn != nullptr) {
                        if (const Value* n = fn->get("name")) {
                            item.set("name", *n);
                        }
                        if (const std::string* a = str_of(fn->get("arguments"))) {
                            item.set("arguments", Value::str(*a));
                        }
                    }
                    out.push_back(std::move(item));
                }
            }
        }
    }
    return Value::list(std::move(out));
}

Value to_responses_tools(const Value& tools) {
    Value::List out;
    if (const Value::List* list = tools.as_list()) {
        for (const Value& tool : *list) {
            const Value* fn = tool.get("function");
            if (fn == nullptr) {
                continue;
            }
            Value t = Value::object();
            t.set("type", Value::str("function"));
            if (const Value* n = fn->get("name")) {
                t.set("name", *n);
            }
            if (const Value* d = fn->get("description")) {
                t.set("description", *d);
            }
            if (const Value* params = fn->get("parameters")) {
                t.set("parameters", *params);
            }
            out.push_back(std::move(t));
        }
    }
    return Value::list(std::move(out));
}

std::string build_responses_body(const std::string& model, const std::string& effort,
                                 const Value& messages, const Value& tools, bool summary) {
    Value req = Value::object();
    req.set("model", Value::str(model));
    req.set("stream", Value::boolean(true));

    std::string instructions;
    req.set("input", to_responses_input(messages, instructions));
    if (!instructions.empty()) {
        req.set("instructions", Value::str(instructions));
    }
    if (!effort.empty()) {
        Value reasoning = Value::object();
        reasoning.set("effort", Value::str(effort));
        if (summary) {
            reasoning.set("summary", Value::str("auto"));
        }
        req.set("reasoning", std::move(reasoning));
    }
    if (Value rtools = to_responses_tools(tools);
        rtools.as_list() != nullptr && !rtools.as_list()->empty()) {
        req.set("tools", std::move(rtools));
    }
    return schema::to_json(req);
}

// ---- streaming SSE fold -------------------------------------------------------

namespace gpt_wire {
struct UsageDetails {
    std::optional<int> cached_tokens;
};
struct Usage {
    std::optional<int> input_tokens;
    std::optional<int> output_tokens;
    std::optional<int> total_tokens;
    std::optional<UsageDetails> input_tokens_details;
};
struct ResponseObj {
    std::optional<Usage> usage;
};
struct Item {
    std::optional<std::string> id;
    std::optional<std::string> type;
    std::optional<std::string> call_id;
    std::optional<std::string> name;
};
struct Event {
    std::optional<std::string> type;
    std::optional<std::string> delta;
    std::optional<std::string> item_id;
    std::optional<Item> item;
    std::optional<ResponseObj> response;
};
} // namespace gpt_wire

namespace {

class ResponsesAccumulator {
public:
    explicit ResponsesAccumulator(DeltaSink on_delta) : on_delta_(std::move(on_delta)) {}

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
        for (const Accum& call : calls_) {
            if (!call.name.empty()) {
                turn.tool_calls.push_back({call.call_id, call.name, call.arguments});
            }
        }
        // Reasoning summary is display-only on the Responses path: we capture the
        // human-readable text but do not (yet) round-trip the opaque reasoning
        // items, so `blocks` stays unset. Tool use is correct without them.
        if (!reasoning_.empty()) {
            turn.reasoning.provider = "gpt";
            turn.reasoning.text = reasoning_;
        }
        return turn;
    }

private:
    struct Accum {
        std::string item_id;
        std::string call_id;
        std::string name;
        std::string arguments;
    };

    Accum& call_for(const std::string& item_id) {
        for (Accum& a : calls_) {
            if (a.item_id == item_id) {
                return a;
            }
        }
        calls_.push_back({item_id, {}, {}, {}});
        return calls_.back();
    }

    void process_line(std::string_view line) {
        constexpr std::string_view prefix = "data:";
        if (!line.starts_with(prefix)) {
            return;
        }
        std::string_view payload = line.substr(prefix.size());
        if (payload.starts_with(' ')) {
            payload.remove_prefix(1);
        }
        gpt_wire::Event ev{};
        if (glz::read<glz::opts{.error_on_unknown_keys = false}>(ev, payload)) {
            return;
        }
        apply(ev);
    }

    void apply(const gpt_wire::Event& ev) {
        const std::string type = ev.type.value_or("");
        if (type == "response.output_text.delta" && ev.delta.has_value()) {
            text_ += *ev.delta;
            if (on_delta_) {
                on_delta_(*ev.delta);
            }
        } else if (type == "response.reasoning_summary_text.delta" && ev.delta.has_value()) {
            reasoning_ += *ev.delta; // captured for display, never streamed to the human
        } else if (type == "response.output_item.added" && ev.item.has_value() &&
                   ev.item->type.value_or("") == "function_call") {
            Accum& a = call_for(ev.item->id.value_or(""));
            a.call_id = ev.item->call_id.value_or("");
            a.name = ev.item->name.value_or("");
        } else if (type == "response.function_call_arguments.delta" && ev.item_id.has_value() &&
                   ev.delta.has_value()) {
            call_for(*ev.item_id).arguments += *ev.delta;
        } else if (type == "response.completed" && ev.response.has_value() &&
                   ev.response->usage.has_value()) {
            const gpt_wire::Usage& u = *ev.response->usage;
            usage_.prompt_tokens = u.input_tokens.value_or(0);
            usage_.completion_tokens = u.output_tokens.value_or(0);
            usage_.total_tokens =
                u.total_tokens.value_or(usage_.prompt_tokens + usage_.completion_tokens);
            if (u.input_tokens_details.has_value()) {
                usage_.cache_read_tokens = u.input_tokens_details->cached_tokens.value_or(0);
            }
            usage_.present = true;
            finish_ = "stop";
        }
    }

    DeltaSink on_delta_;
    std::string buffer_;
    std::string text_;
    std::string reasoning_;
    std::string finish_;
    Usage usage_;
    std::vector<Accum> calls_;
};

} // namespace

CompletedTurn fold_responses_sse(std::string_view sse, const DeltaSink& on_delta) {
    ResponsesAccumulator acc(on_delta);
    acc.feed(sse);
    return acc.settle();
}

std::expected<CompletedTurn, std::string> GptProvider::stream_chat(const Value& messages,
                                                                   const Value& tools,
                                                                   DeltaSink on_delta,
                                                                   std::stop_token cancel) {
    const std::string url = cfg_.base_url + "/responses";
    const std::string body = build_responses_body(cfg_.model, cfg_.reasoning_effort, messages, tools,
                                                  cfg_.reasoning_summary);
    const std::vector<std::string> headers = {
        "Content-Type: application/json",
        "Authorization: Bearer " + cfg_.api_key,
        "Accept: text/event-stream",
    };

    ResponsesAccumulator acc(std::move(on_delta));
    http::StreamResult r = http::stream_post(
        url, headers, body, [&](std::string_view chunk) { acc.feed(chunk); }, std::move(cancel));

    if (!r.ok) {
        return std::unexpected(r.error);
    }
    if (r.status != 200) {
        return std::unexpected("HTTP " + std::to_string(r.status) + ": " + r.tail);
    }
    CompletedTurn turn = acc.settle();
    if (!turn.reasoning.empty()) {
        turn.reasoning.model = cfg_.model;
    }
    return turn;
}

namespace gpt_wire {
struct ModelEntry {
    std::string id;
};
struct ModelList {
    std::vector<ModelEntry> data;
};
} // namespace gpt_wire

std::expected<std::vector<ModelInfo>, std::string> GptProvider::list_models() {
    const std::vector<std::string> headers = {"Authorization: Bearer " + cfg_.api_key};
    auto body = http::get(cfg_.base_url + "/models", headers);
    if (!body.has_value()) {
        return std::unexpected(body.error());
    }
    gpt_wire::ModelList parsed;
    if (glz::read<glz::opts{.error_on_unknown_keys = false}>(parsed, *body)) {
        return std::unexpected("could not parse model list");
    }
    std::vector<ModelInfo> out;
    out.reserve(parsed.data.size());
    for (const gpt_wire::ModelEntry& e : parsed.data) {
        out.push_back({e.id, context_limit(e.id)});
    }
    return out;
}

int GptProvider::context_limit(const std::string& model_id) const {
    if (model_id.starts_with("gpt-5") || model_id.starts_with("o3") ||
        model_id.starts_with("o4") || model_id.starts_with("codex")) {
        return 400000;
    }
    if (model_id.starts_with("gpt-4.1")) {
        return 1000000;
    }
    if (model_id.starts_with("gpt-4o")) {
        return 128000;
    }
    return 0;
}

} // namespace astrite::llm
