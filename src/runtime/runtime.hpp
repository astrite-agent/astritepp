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
#include <stop_token>
#include <string>

#include "kernel/primitive.hpp"
#include "llm/sse.hpp"
#include "runtime/events.hpp"
#include "runtime/session.hpp"
#include "schema/value.hpp"

namespace astrite::runtime {

// Injectable model transport: messages/tools in, settled turn out, deltas live,
// cancellation via the turn's stop_token.
using ChatFn = std::function<std::expected<llm::CompletedTurn, std::string>(
    const schema::Value& messages, const schema::Value& tools,
    llm::StreamAccumulator::DeltaSink on_delta, std::stop_token cancel)>;

struct Options {
    std::filesystem::path session_file;
    std::filesystem::path workspace_root; // absolute fs base; empty → current_path()
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
    // `cancel` aborts an in-flight model stream (surfaces as a TurnError).
    bool submit_user_message(const std::string& text, std::stop_token cancel = {});

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
    std::mutex turn_mu_;             // one turn at a time
    std::stop_token current_cancel_; // the active turn's cancel token (set under turn_mu_)
};

} // namespace astrite::runtime
