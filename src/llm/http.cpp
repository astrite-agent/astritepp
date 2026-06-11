#include "llm/http.hpp"

#include <algorithm>
#include <cstddef>

#include <curl/curl.h>

namespace astrite::llm::http {

namespace {

bool ensure_global_init() {
    static const CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
    return rc == CURLE_OK;
}

struct StreamState {
    const std::function<void(std::string_view)>* on_chunk;
    std::stop_token cancel;
    std::string tail;
};

std::size_t on_write_stream(char* data, std::size_t size, std::size_t nmemb, void* userp) {
    auto* state = static_cast<StreamState*>(userp);
    const std::size_t total = size * nmemb;
    (*state->on_chunk)(std::string_view(data, total));
    if (state->tail.size() < 4096) {
        state->tail.append(data, std::min(total, 4096 - state->tail.size()));
    }
    return total;
}

int on_progress(void* userp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* state = static_cast<StreamState*>(userp);
    return state->cancel.stop_requested() ? 1 : 0; // nonzero aborts the transfer
}

std::size_t on_write_buffer(char* data, std::size_t size, std::size_t nmemb, void* userp) {
    auto* out = static_cast<std::string*>(userp);
    const std::size_t total = size * nmemb;
    out->append(data, total);
    return total;
}

curl_slist* build_headers(const std::vector<std::string>& headers) {
    curl_slist* list = nullptr;
    for (const std::string& h : headers) {
        list = curl_slist_append(list, h.c_str());
    }
    return list;
}

} // namespace

StreamResult stream_post(const std::string& url, const std::vector<std::string>& headers,
                         const std::string& body,
                         const std::function<void(std::string_view)>& on_chunk,
                         std::stop_token cancel) {
    if (!ensure_global_init()) {
        return {false, 0, "curl_global_init failed", {}};
    }
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        return {false, 0, "curl_easy_init failed", {}};
    }

    StreamState state{&on_chunk, std::move(cancel), {}};
    curl_slist* hdrs = build_headers(headers);

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_write_stream);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &state);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);

    const CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (rc == CURLE_ABORTED_BY_CALLBACK) {
        return {false, status, "cancelled", std::move(state.tail)};
    }
    if (rc != CURLE_OK) {
        return {false, status, std::string("transport: ") + curl_easy_strerror(rc),
                std::move(state.tail)};
    }
    return {true, status, {}, std::move(state.tail)};
}

std::expected<std::string, std::string> get(const std::string& url,
                                            const std::vector<std::string>& headers) {
    if (!ensure_global_init()) {
        return std::unexpected("curl_global_init failed");
    }
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        return std::unexpected("curl_easy_init failed");
    }
    std::string out;
    curl_slist* hdrs = build_headers(headers);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_write_buffer);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);

    const CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        return std::unexpected(std::string("transport: ") + curl_easy_strerror(rc));
    }
    if (status != 200) {
        return std::unexpected("HTTP " + std::to_string(status) + ": " + out);
    }
    return out;
}

} // namespace astrite::llm::http
