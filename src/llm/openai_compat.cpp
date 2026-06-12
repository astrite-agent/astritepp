#include "llm/openai_compat.hpp"

#include <cstdlib>
#include <utility>

#include <glaze/json.hpp>

#include "llm/http.hpp"
#include "schema/value_json.hpp"

namespace astrite::llm {

std::expected<Config, std::string> Config::from_env() {
    const char* key = std::getenv("OPENAI_API_KEY");
    if (key == nullptr || *key == '\0') {
        return std::unexpected("OPENAI_API_KEY is not set");
    }
    const char* model = std::getenv("OPENAI_MODEL");
    const char* base = std::getenv("OPENAI_BASE_URL");
    return from_env_or(key, model != nullptr ? model : "", base != nullptr ? base : "");
}

Config Config::from_env_or(std::string api_key, std::string model, std::string base_url) {
    Config cfg;
    cfg.api_key = std::move(api_key);
    cfg.model = model.empty() ? "gpt-5.2" : std::move(model);
    cfg.base_url = base_url.empty() ? "https://api.openai.com/v1" : std::move(base_url);
    while (cfg.base_url.ends_with('/')) {
        cfg.base_url.pop_back();
    }
    return cfg;
}

std::string build_request_body(const std::string& model, const schema::Value& messages,
                               const schema::Value& tools) {
    using schema::Value;
    Value req = Value::object();
    req.set("model", Value::str(model));
    req.set("messages", messages);
    req.set("stream", Value::boolean(true));
    Value stream_options = Value::object();
    stream_options.set("include_usage", Value::boolean(true)); // ask for a final usage chunk
    req.set("stream_options", std::move(stream_options));
    if (const Value::List* list = tools.as_list(); list != nullptr && !list->empty()) {
        req.set("tools", tools);
    }
    return schema::to_json(req);
}

std::expected<CompletedTurn, std::string>
OpenAiCompatProvider::stream_chat(const schema::Value& messages, const schema::Value& tools,
                                  DeltaSink on_delta, std::stop_token cancel) {
    const std::string url = cfg_.base_url + "/chat/completions";
    const std::string body = build_request_body(cfg_.model, messages, tools);
    const std::vector<std::string> headers = {
        "Content-Type: application/json",
        "Authorization: Bearer " + cfg_.api_key,
        "Accept: text/event-stream",
    };

    StreamAccumulator accumulator(std::move(on_delta));
    http::StreamResult r = http::stream_post(
        url, headers, body, [&](std::string_view chunk) { accumulator.feed(chunk); },
        std::move(cancel));

    if (!r.ok) {
        return std::unexpected(r.error);
    }
    if (r.status != 200) {
        return std::unexpected("HTTP " + std::to_string(r.status) + ": " + r.tail);
    }
    CompletedTurn turn = accumulator.settle();
    if (!turn.reasoning.empty()) {
        turn.reasoning.model = cfg_.model;
    }
    return turn;
}

namespace oc_wire {
// glaze wire for GET /models -> {data: [{id}, ...]}. Named (not anonymous) so
// glaze's reflection sees a type with linkage.
struct ModelEntry {
    std::string id;
};
struct ModelList {
    std::vector<ModelEntry> data;
};
} // namespace oc_wire

std::expected<std::vector<ModelInfo>, std::string> OpenAiCompatProvider::list_models() {
    const std::vector<std::string> headers = {"Authorization: Bearer " + cfg_.api_key};
    auto body = http::get(cfg_.base_url + "/models", headers);
    if (!body.has_value()) {
        return std::unexpected(body.error());
    }
    oc_wire::ModelList parsed;
    if (glz::read<glz::opts{.error_on_unknown_keys = false}>(parsed, *body)) {
        return std::unexpected("could not parse model list");
    }
    std::vector<ModelInfo> out;
    out.reserve(parsed.data.size());
    for (const oc_wire::ModelEntry& e : parsed.data) {
        out.push_back({e.id, context_limit(e.id)});
    }
    return out;
}

int OpenAiCompatProvider::context_limit(const std::string& model_id) const {
    // A small known table; an OPENAI_CONTEXT_WINDOW override is applied by the
    // factory. Compatible endpoints with unlisted models report 0 (unknown).
    if (model_id.starts_with("gpt-5") || model_id.starts_with("o3") ||
        model_id.starts_with("o4")) {
        return 400000;
    }
    if (model_id.starts_with("gpt-4.1")) {
        return 1000000;
    }
    if (model_id.starts_with("gpt-4o") || model_id.starts_with("gpt-4-turbo")) {
        return 128000;
    }
    return 0;
}

} // namespace astrite::llm
