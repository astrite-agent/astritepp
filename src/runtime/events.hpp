#pragma once

// The sealed runtime event vocabulary frontends consume, and a small bounded
// channel. Exactly one terminal event (TurnFinished | TurnError) ends each turn;
// the consumer must drain to it.

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace astrite::runtime {

struct Started {};
struct AssistantDelta {
    std::string text;
};
struct AssistantSettled {
    std::string text;
    int prompt_tokens = 0;
    int completion_tokens = 0;
    int total_tokens = 0;
    bool has_usage = false;
    int cache_read_tokens = 0;  // prompt tokens served from cache (trailing: keeps 5-arg init)
    int cache_write_tokens = 0; // prompt tokens written to cache this call
};
// A turn's reasoning/thinking trace, settled at end of turn (never streamed into
// the visible text). Empty traces are not emitted. Display-only — the round-trip
// copy lives in the message log's `_reasoning` sidecar, not here.
struct ReasoningSettled {
    std::string text;
};
struct ToolStarted {
    std::string id;
    std::string name;
    std::string arguments;
};
struct ToolFinished {
    std::string id;
    std::string name;
    std::string content;
};
// A model-authored component staged for human review. Authority is NOT granted
// by this event — it announces a candidate the human can later approve (which is
// what writes the trust record and installs it). The frontend renders this as
// root-owned kernel chrome, never as model-styleable content.
struct ComponentProposed {
    std::string name;
    std::string kind;
    std::vector<std::string> caps;
    std::string risk;
    std::string staged_path;     // .astrite/proposals/<name>.json
    bool replaces_existing = false; // true if a component of this name is already loaded
};
struct TurnFinished {};
struct TurnError {
    std::string message;
};

using Event = std::variant<Started, AssistantDelta, AssistantSettled, ReasoningSettled, ToolStarted,
                           ToolFinished, ComponentProposed, TurnFinished, TurnError>;

class EventChannel {
public:
    explicit EventChannel(std::size_t capacity = 1024) : capacity_(capacity) {}

    void push(Event e) {
        std::unique_lock lock(mu_);
        not_full_.wait(lock, [&] { return q_.size() < capacity_ || closed_; });
        if (closed_) {
            return;
        }
        q_.push_back(std::move(e));
        not_empty_.notify_one();
    }

    // Blocking pop; nullopt only after close() with an empty queue.
    [[nodiscard]] std::optional<Event> pop() {
        std::unique_lock lock(mu_);
        not_empty_.wait(lock, [&] { return !q_.empty() || closed_; });
        return take_front_locked();
    }

    [[nodiscard]] std::optional<Event> try_pop() {
        std::lock_guard lock(mu_);
        return take_front_locked();
    }

    void close() {
        std::lock_guard lock(mu_);
        closed_ = true;
        not_empty_.notify_all();
        not_full_.notify_all();
    }

private:
    std::optional<Event> take_front_locked() {
        if (q_.empty()) {
            return std::nullopt;
        }
        Event e = std::move(q_.front());
        q_.pop_front();
        not_full_.notify_one();
        return e;
    }

    std::mutex mu_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::deque<Event> q_;
    std::size_t capacity_;
    bool closed_ = false;
};

} // namespace astrite::runtime
