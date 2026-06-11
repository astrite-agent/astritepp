#pragma once

// The one place that speaks HTTP — a thin, blocking libcurl transport shared by
// every provider (no async runtime, DESIGN §8). Streaming POST feeds raw body
// bytes to a sink as they arrive and aborts on a stop_token; GET returns the
// whole body. Providers layer their own request/response shapes on top.

#include <expected>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace astrite::llm::http {

// Outcome of a streaming POST. `ok` means the transfer completed (any HTTP
// status); inspect `status` for the HTTP code. `tail` is a bounded copy of the
// response body for diagnosing non-2xx replies.
struct StreamResult {
    bool ok = false;
    long status = 0;
    std::string error; // transport/cancel error when !ok
    std::string tail;  // first few KiB of the body (for error reporting)
};

// POST `body` to `url` with `headers`, streaming the response body to `on_chunk`
// as it arrives. Returns when the transfer finishes, is cancelled, or fails.
[[nodiscard]] StreamResult stream_post(const std::string& url,
                                       const std::vector<std::string>& headers,
                                       const std::string& body,
                                       const std::function<void(std::string_view)>& on_chunk,
                                       std::stop_token cancel);

// Blocking GET; returns the body on HTTP 200, or an error string otherwise.
[[nodiscard]] std::expected<std::string, std::string>
get(const std::string& url, const std::vector<std::string>& headers);

} // namespace astrite::llm::http
