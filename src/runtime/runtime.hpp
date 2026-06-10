#pragma once

// Frontend-agnostic runtime: registers the effect primitives as
// capability-tagged closures, authority-checks every component at startup
// (fail closed), seeds the system prompt once, and executes turns by walking
// the `turn` component — the loop itself is data, not C++.

#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "kernel/primitive.hpp"
#include "llm/sse.hpp"
#include "runtime/events.hpp"
#include "runtime/session.hpp"
#include "schema/value.hpp"

namespace astrite::runtime {

// Injectable model transport: messages/tools in, settled turn out, deltas live.
using ChatFn = std::function<std::expected<llm::CompletedTurn, std::string>(
    const schema::Value& messages, const schema::Value& tools,
    llm::StreamAccumulator::DeltaSink on_delta)>;

struct Options {
    std::filesystem::path session_file;
    std::string system_prompt;
    kernel::ComponentMap components; // must contain the agent component `turn`
    ChatFn chat;                     // empty → built from env (OPENAI_API_KEY…)
};

class Runtime {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<Runtime>, std::string>
    create(Options opts);

    // Blocking: append the user message, walk `turn`, emit events. Exactly one
    // terminal event per call (TurnFinished | TurnError). Returns false on error.
    bool submit_user_message(const std::string& text);

    [[nodiscard]] EventChannel& events() { return events_; }

private:
    explicit Runtime(Options opts);
    void register_effects();
    [[nodiscard]] schema::Value tool_definitions() const;

    Options opts_;
    Session session_;
    kernel::Registry registry_;
    EventChannel events_;
    schema::Value tool_defs_;
    std::mutex turn_mu_; // one turn at a time
};

} // namespace astrite::runtime
