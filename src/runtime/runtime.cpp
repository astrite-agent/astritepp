#include "runtime/runtime.hpp"

#include <utility>

#include "kernel/authority.hpp"
#include "kernel/blackboard.hpp"
#include "kernel/eval.hpp"
#include "kernel/prims.hpp"
#include "kernel/trust.hpp"
#include "llm/client.hpp"
#include "sandbox/sandbox.hpp"

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
    if (opts.workspace_root.empty()) {
        std::error_code ec;
        opts.workspace_root = std::filesystem::current_path(ec);
    }
    if (!opts.chat) {
        auto cfg = llm::Config::from_env();
        if (!cfg.has_value()) {
            return std::unexpected("runtime: " + cfg.error());
        }
        auto client = std::make_shared<llm::Client>(std::move(*cfg));
        opts.chat = [client](const Value& messages, const Value& tools,
                             llm::StreamAccumulator::DeltaSink on_delta, std::stop_token cancel) {
            return client->stream_chat(messages, tools, std::move(on_delta), std::move(cancel));
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

namespace {

// Belt: confine a model-supplied path string to a workspace-relative rel via the
// trust-boundary primitive. Returns the rel, or a KernelError carrying the reason
// (which dispatch surfaces to the model as recoverable content).
std::expected<std::string, kernel::KernelError> confine(const Value& args, const char* prim) {
    const Value* path_v = args.get("path");
    if (path_v == nullptr || path_v->as_str() == nullptr) {
        return std::unexpected(kernel::KernelError{std::string(prim) + ": `path` must be a string"});
    }
    Value rq = Value::object();
    rq.set("path", *path_v);
    rq.set("base", Value::str("workspace"));
    Value resolved = kernel::resolve_path(rq);
    const Value* ok = resolved.get("ok");
    if (ok == nullptr || ok->as_bool() == nullptr || !*ok->as_bool()) {
        const Value* reason = resolved.get("reason");
        return std::unexpected(kernel::KernelError{
            std::string(prim) + ": " +
            (reason != nullptr && reason->as_str() != nullptr ? *reason->as_str() : "bad path")});
    }
    return *resolved.get("rel")->as_str();
}

// list/find may target the base root, which path.resolve rejects (empty rel);
// "." and "" mean "the workspace root" here.
std::expected<std::string, kernel::KernelError> confine_dir(const Value& args, const char* prim) {
    const Value* path_v = args.get("path");
    if (path_v == nullptr || path_v->as_str() == nullptr || path_v->as_str()->empty() ||
        *path_v->as_str() == ".") {
        return std::string{}; // root
    }
    return confine(args, prim);
}

} // namespace

void Runtime::register_effects() {
    const std::filesystem::path base = opts_.workspace_root;

    registry_.add_effect(
        "fs.read", "fs.read",
        [base](const Value& args) -> std::expected<Value, kernel::KernelError> {
            auto rel = confine(args, "fs.read");
            if (!rel.has_value()) {
                return std::unexpected(rel.error());
            }
            sandbox::OpResult r = sandbox::run_fs_op("read", base, *rel);
            if (!r.ok) {
                return std::unexpected(kernel::KernelError{r.output.empty() ? "fs.read failed"
                                                                            : r.output});
            }
            return Value::str(std::move(r.output));
        });
    registry_.add_effect(
        "fs.list", "fs.list",
        [base](const Value& args) -> std::expected<Value, kernel::KernelError> {
            auto rel = confine_dir(args, "fs.list");
            if (!rel.has_value()) {
                return std::unexpected(rel.error());
            }
            sandbox::OpResult r = sandbox::run_fs_op("list", base, *rel);
            if (!r.ok) {
                return std::unexpected(kernel::KernelError{r.output.empty() ? "fs.list failed"
                                                                            : r.output});
            }
            return Value::str(std::move(r.output));
        });
    registry_.add_effect(
        "search.find", "search.find",
        [base](const Value& args) -> std::expected<Value, kernel::KernelError> {
            const Value* pattern = args.get("pattern");
            if (pattern == nullptr || pattern->as_str() == nullptr) {
                return std::unexpected(
                    kernel::KernelError{"search.find: `pattern` must be a string"});
            }
            auto rel = confine_dir(args, "search.find");
            if (!rel.has_value()) {
                return std::unexpected(rel.error());
            }
            sandbox::OpResult r = sandbox::run_fs_op("find", base, *rel, *pattern->as_str());
            if (!r.ok) {
                return std::unexpected(kernel::KernelError{r.output.empty() ? "search.find failed"
                                                                            : r.output});
            }
            return Value::str(std::move(r.output));
        });

    registry_.add_effect(
        "fs.write", "fs.write",
        [base](const Value& args) -> std::expected<Value, kernel::KernelError> {
            auto rel = confine(args, "fs.write");
            if (!rel.has_value()) {
                return std::unexpected(rel.error());
            }
            const Value* content = args.get("content");
            if (content == nullptr || content->as_str() == nullptr) {
                return std::unexpected(kernel::KernelError{"fs.write: `content` must be a string"});
            }
            sandbox::OpResult r = sandbox::fs_write(base, *rel, *content->as_str());
            if (!r.ok) {
                return std::unexpected(kernel::KernelError{r.output.empty() ? "fs.write failed"
                                                                            : r.output});
            }
            return Value::str(std::move(r.output));
        });
    registry_.add_effect(
        "fs.edit", "fs.edit",
        [base](const Value& args) -> std::expected<Value, kernel::KernelError> {
            auto rel = confine(args, "fs.edit");
            if (!rel.has_value()) {
                return std::unexpected(rel.error());
            }
            const Value* old_text = args.get("old_text");
            const Value* new_text = args.get("new_text");
            if (old_text == nullptr || old_text->as_str() == nullptr || new_text == nullptr ||
                new_text->as_str() == nullptr) {
                return std::unexpected(
                    kernel::KernelError{"fs.edit: `old_text` and `new_text` must be strings"});
            }
            std::size_t expected = 1;
            if (const Value* e = args.get("expected_replacements"); e != nullptr) {
                if (const std::int64_t* n = e->as_int(); n != nullptr && *n >= 0) {
                    expected = static_cast<std::size_t>(*n);
                } else {
                    return std::unexpected(kernel::KernelError{
                        "fs.edit: `expected_replacements` must be a non-negative integer"});
                }
            }
            sandbox::OpResult r =
                sandbox::fs_edit(base, *rel, *old_text->as_str(), *new_text->as_str(), expected);
            if (!r.ok) {
                return std::unexpected(kernel::KernelError{r.output.empty() ? "fs.edit failed"
                                                                            : r.output});
            }
            return Value::str(std::move(r.output));
        });

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
                },
                current_cancel_);
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

bool Runtime::submit_user_message(const std::string& text, std::stop_token cancel) {
    std::lock_guard turn_lock(turn_mu_);
    current_cancel_ = std::move(cancel);
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
