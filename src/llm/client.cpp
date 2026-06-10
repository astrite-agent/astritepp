#include "llm/client.hpp"

#include <cstdlib>
#include <utility>

#include <curl/curl.h>

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
    if (const Value::List* list = tools.as_list(); list != nullptr && !list->empty()) {
        req.set("tools", tools);
    }
    return schema::to_json(req);
}

namespace {

struct TransferState {
    StreamAccumulator* accumulator;
    std::stop_token cancel;
    std::string raw_tail; // bounded copy of the response for non-200 diagnostics
};

size_t on_write(char* data, size_t size, size_t nmemb, void* userp) {
    auto* state = static_cast<TransferState*>(userp);
    const size_t total = size * nmemb;
    state->accumulator->feed(std::string_view(data, total));
    if (state->raw_tail.size() < 4096) {
        state->raw_tail.append(data, std::min(total, 4096 - state->raw_tail.size()));
    }
    return total;
}

int on_progress(void* userp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* state = static_cast<TransferState*>(userp);
    return state->cancel.stop_requested() ? 1 : 0; // nonzero aborts the transfer
}

} // namespace

std::expected<CompletedTurn, std::string> Client::stream_chat(const schema::Value& messages,
                                                              const schema::Value& tools,
                                                              StreamAccumulator::DeltaSink on_delta,
                                                              std::stop_token cancel) {
    static const CURLcode global = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (global != CURLE_OK) {
        return std::unexpected("curl_global_init failed");
    }

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        return std::unexpected("curl_easy_init failed");
    }

    const std::string url = cfg_.base_url + "/chat/completions";
    const std::string body = build_request_body(cfg_.model, messages, tools);
    const std::string auth = "Authorization: Bearer " + cfg_.api_key;

    StreamAccumulator accumulator(std::move(on_delta));
    TransferState state{&accumulator, std::move(cancel), {}};

    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, auth.c_str());
    headers = curl_slist_append(headers, "Accept: text/event-stream");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &state);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L); // the endpoint is the endpoint

    const CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc == CURLE_ABORTED_BY_CALLBACK) {
        return std::unexpected("cancelled");
    }
    if (rc != CURLE_OK) {
        return std::unexpected(std::string("transport: ") + curl_easy_strerror(rc));
    }
    if (status != 200) {
        return std::unexpected("HTTP " + std::to_string(status) + ": " + state.raw_tail);
    }
    return accumulator.settle();
}

} // namespace astrite::llm
