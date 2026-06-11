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
#include <vector>

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

    // ---- consent: the human side of self-modification (B4). These are the ONLY
    // way a model-authored component becomes active. The model stages a proposal
    // (component.propose); a human reviews and accepts it here, which is what
    // writes the component file + a hash-pinned trust record and installs it.

    struct ProposalSummary {
        std::string name;
        std::string kind;
        std::string risk;
        std::vector<std::string> caps;
    };

    // Names of components staged under .astrite/proposals/, awaiting review.
    [[nodiscard]] std::vector<std::string> pending_proposals() const;

    // Approve a staged proposal: re-validate against the live registry, write
    // .astrite/components/<name>.json + a trust record, install it live, and clear
    // the staging file. Fails (installing nothing) on an unknown or invalid one.
    [[nodiscard]] std::expected<ProposalSummary, std::string>
    accept_proposal(const std::string& name);

    // Discard a staged proposal without installing it.
    bool reject_proposal(const std::string& name);

private:
    explicit Runtime(Options opts);
    void register_effects();
    [[nodiscard]] schema::Value tool_definitions() const;

    // One mutation the agent made this session: the file's content before and
    // after. diff.workspace renders the net change per path — a trusted,
    // git-free record of what the agent did (it only sees its own writes).
    struct WriteRecord {
        std::string path;
        std::string before;
        std::string after;
    };

    Options opts_;
    Session session_;
    kernel::Registry registry_;
    std::vector<WriteRecord> journal_; // turn-thread only (turns are serialized)
    EventChannel events_;
    schema::Value tool_defs_;
    std::mutex turn_mu_;             // one turn at a time
    std::stop_token current_cancel_; // the active turn's cancel token (set under turn_mu_)
};

} // namespace astrite::runtime
