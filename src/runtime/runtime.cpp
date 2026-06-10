#include "runtime/runtime.hpp"

#include <utility>

#include "kernel/authority.hpp"
#include "kernel/blackboard.hpp"
#include "kernel/eval.hpp"
#include "kernel/prims.hpp"
#include "llm/client.hpp"

namespace astrite::runtime {

using schema::Value;

namespace {

// Bridges kernel facts into runtime events.
struct EventObserver : kernel::EvalObserver {
    EventChannel* channel;
    explicit EventObserver(EventChannel* c) : channel(c) {}
    void tool_started(std::string_view id, std::string_view name,
                      std::string_view arguments) override {
        channel->push(ToolStarted{std::string(id), std::string(name), std::string(arguments)});
    }
    void tool_finished(std::string_view id, std::string_view name,
                       std::string_view content) override {
        channel->push(ToolFinished{std::string(id), std::string(name), std::string(content)});
    }
};

} // namespace

Runtime::Runtime(Options opts)
    : opts_(std::move(opts)), session_(opts_.session_file), registry_(kernel::default_registry()),
      tool_defs_(Value::list({})) {}

std::expected<std::unique_ptr<Runtime>, std::string> Runtime::create(Options opts) {
    if (!opts.components.contains("turn")) {
        return std::unexpected("runtime: no `turn` component registered");
    }
    if (!opts.chat) {
        auto cfg = llm::Config::from_env();
        if (!cfg.has_value()) {
            return std::unexpected("runtime: " + cfg.error());
        }
        auto client = std::make_shared<llm::Client>(std::move(*cfg));
        opts.chat = [client](const Value& messages, const Value& tools,
                             llm::StreamAccumulator::DeltaSink on_delta) {
            return client->stream_chat(messages, tools, std::move(on_delta));
        };
    }

    auto runtime = std::unique_ptr<Runtime>(new Runtime(std::move(opts)));
    runtime->register_effects();
    runtime->tool_defs_ = runtime->tool_definitions();

    // Every loaded component is authority-checked before anything runs.
    for (const auto& [name, component] : runtime->opts_.components) {
        auto trace = kernel::check_component_authority(component, runtime->registry_);
        if (!trace.has_value()) {
            return std::unexpected("runtime: " + trace.error().message);
        }
    }
    return runtime;
}

void Runtime::register_effects() {
    registry_.add_effect(
        "session.read", "session.read",
        [this](const Value&) -> std::expected<Value, kernel::KernelError> {
            return session_.read();
        });
    registry_.add_effect(
        "session.append", "session.append",
        [this](const Value& args) -> std::expected<Value, kernel::KernelError> {
            const Value* record = args.get("record");
            if (record == nullptr) {
                return std::unexpected(kernel::KernelError{"session.append: missing `record`"});
            }
            auto appended = session_.append(*record);
            if (!appended.has_value()) {
                return std::unexpected(appended.error());
            }
            return Value();
        });
    registry_.add_effect(
        "llm.stream", "llm.stream",
        [this](const Value& args) -> std::expected<Value, kernel::KernelError> {
            const Value* messages = args.get("messages");
            if (messages == nullptr || !messages->is_list()) {
                return std::unexpected(
                    kernel::KernelError{"llm.stream: `messages` must be a list"});
            }
            const Value* tools = args.get("tools");
            auto turn = opts_.chat(
                *messages, tools != nullptr ? *tools : Value::list({}),
                [this](std::string_view delta) {
                    events_.push(AssistantDelta{std::string(delta)});
                });
            if (!turn.has_value()) {
                return std::unexpected(kernel::KernelError{"llm.stream: " + turn.error()});
            }
            events_.push(AssistantSettled{turn->text});

            Value::List calls;
            calls.reserve(turn->tool_calls.size());
            for (const llm::ToolCall& call : turn->tool_calls) {
                Value entry = Value::object();
                entry.set("id", Value::str(call.id));
                entry.set("name", Value::str(call.name));
                entry.set("arguments", Value::str(call.arguments));
                calls.push_back(std::move(entry));
            }
            Value result = Value::object();
            result.set("text", Value::str(turn->text));
            result.set("finish", Value::str(turn->finish_reason));
            result.set("tool_calls", Value::list(std::move(calls)));
            result.set("message", llm::assistant_message(*turn));
            return result;
        });
}

Value Runtime::tool_definitions() const {
    Value::List defs;
    for (const auto& [name, component] : opts_.components) {
        if (component.kind != "tool") {
            continue;
        }
        Value fn = Value::object();
        fn.set("name", Value::str(component.name));
        if (component.description.has_value()) {
            fn.set("description", Value::str(*component.description));
        }
        if (component.parameters.has_value()) {
            fn.set("parameters", *component.parameters);
        }
        Value def = Value::object();
        def.set("type", Value::str("function"));
        def.set("function", std::move(fn));
        defs.push_back(std::move(def));
    }
    return Value::list(std::move(defs));
}

bool Runtime::submit_user_message(const std::string& text) {
    std::lock_guard turn_lock(turn_mu_);
    events_.push(Started{});

    auto fail = [&](std::string message) {
        events_.push(TurnError{std::move(message)});
        return false;
    };

    // Seed the system prompt exactly once, on first contact with an empty log.
    if (!opts_.system_prompt.empty()) {
        auto ctx = session_.read();
        if (!ctx.has_value()) {
            return fail(ctx.error().message);
        }
        if (ctx->get("messages")->as_list()->empty()) {
            Value system = Value::object();
            system.set("role", Value::str("system"));
            system.set("content", Value::str(opts_.system_prompt));
            if (auto appended = session_.append(system); !appended.has_value()) {
                return fail(appended.error().message);
            }
        }
    }

    Value user = Value::object();
    user.set("role", Value::str("user"));
    user.set("content", Value::str(text));
    if (auto appended = session_.append(user); !appended.has_value()) {
        return fail(appended.error().message);
    }

    kernel::Blackboard bb;
    Value cfg = Value::object();
    cfg.set("tools", tool_defs_);
    bb.bind("cfg", std::move(cfg));

    EventObserver observer(&events_);
    kernel::Context ctx{&registry_, &opts_.components, &observer};
    const schema::Component& turn = opts_.components.find("turn")->second;
    auto status = kernel::walk(turn.tree, bb, ctx);
    if (!status.has_value()) {
        return fail(status.error().message);
    }
    events_.push(TurnFinished{});
    return true;
}

} // namespace astrite::runtime
