#include "runtime/runtime.hpp"

#include <ctime>
#include <fstream>
#include <map>
#include <utility>

#include "kernel/authority.hpp"
#include "kernel/blackboard.hpp"
#include "kernel/eval.hpp"
#include "kernel/prims.hpp"
#include "kernel/trust.hpp"
#include "llm/provider.hpp"
#include "llm/provider_factory.hpp"
#include "runtime/fs_read_args.hpp"
#include "runtime/proposals.hpp"
#include "runtime/task_args.hpp"
#include "runtime/trust_store.hpp"
#include "sandbox/sandbox.hpp"
#include "schema/load.hpp"
#include "util/control_plane.hpp"
#include "util/unified_diff.hpp"

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
        auto provider = llm::make_provider_from_env();
        if (!provider.has_value()) {
            return std::unexpected("runtime: " + provider.error());
        }
        std::shared_ptr<llm::Provider> p = std::move(*provider);
        opts.chat = [p](const Value& messages, const Value& tools,
                        llm::StreamAccumulator::DeltaSink on_delta, std::stop_token cancel) {
            return p->stream_chat(messages, tools, std::move(on_delta), std::move(cancel));
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

// Belt for write effects: confine, then refuse the control plane (.astrite). The
// model's only route to a write is through here, so this is the primary gate; the
// sandbox helper re-checks in its confined child as the suspender.
std::expected<std::string, kernel::KernelError> confine_write(const Value& args, const char* prim) {
    auto rel = confine(args, prim);
    if (!rel.has_value()) {
        return rel;
    }
    if (is_control_plane_rel(*rel)) {
        return std::unexpected(kernel::KernelError{
            std::string(prim) + ": the control plane (.astrite) is read-only to components"});
    }
    return rel;
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
            auto range = read_range_spec(args);
            if (!range.has_value()) {
                return std::unexpected(range.error());
            }
            sandbox::OpResult r = sandbox::run_fs_op("read", base, *rel, *range);
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
        [this, base](const Value& args) -> std::expected<Value, kernel::KernelError> {
            auto rel = confine_write(args, "fs.write");
            if (!rel.has_value()) {
                return std::unexpected(rel.error());
            }
            const Value* content = args.get("content");
            if (content == nullptr || content->as_str() == nullptr) {
                return std::unexpected(kernel::KernelError{"fs.write: `content` must be a string"});
            }
            sandbox::OpResult before = sandbox::run_fs_op("read", base, *rel); // "" if new
            sandbox::OpResult r = sandbox::fs_write(base, *rel, *content->as_str());
            if (!r.ok) {
                return std::unexpected(kernel::KernelError{r.output.empty() ? "fs.write failed"
                                                                            : r.output});
            }
            journal_.push_back({*rel, before.ok ? before.output : "", *content->as_str()});
            return Value::str(std::move(r.output));
        });
    registry_.add_effect(
        "fs.edit", "fs.edit",
        [this, base](const Value& args) -> std::expected<Value, kernel::KernelError> {
            auto rel = confine_write(args, "fs.edit");
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
            sandbox::OpResult before = sandbox::run_fs_op("read", base, *rel);
            sandbox::OpResult r =
                sandbox::fs_edit(base, *rel, *old_text->as_str(), *new_text->as_str(), expected);
            if (!r.ok) {
                return std::unexpected(kernel::KernelError{r.output.empty() ? "fs.edit failed"
                                                                            : r.output});
            }
            sandbox::OpResult after = sandbox::run_fs_op("read", base, *rel);
            journal_.push_back(
                {*rel, before.ok ? before.output : "", after.ok ? after.output : ""});
            return Value::str(std::move(r.output));
        });
    registry_.add_effect(
        "task.run", "task.run",
        [base](const Value& args) -> std::expected<Value, kernel::KernelError> {
            // The spec is component-authored and pinned; the model selects the
            // verb, never the argv. A malformed spec is a hard error; a task that
            // merely *fails* (non-zero exit) or cannot spawn is a structured
            // result the model reads and adapts to, not a walk error.
            const Value* spec = args.get("spec");
            if (spec == nullptr) {
                return std::unexpected(kernel::KernelError{"task.run: missing `spec`"});
            }
            auto ts = build_task_spec(*spec, base);
            if (!ts.has_value()) {
                return std::unexpected(ts.error());
            }
            return task_result_value(sandbox::run_task(*ts));
        });

    registry_.add_effect(
        "workspace.context", "workspace.context",
        [this, base](const Value&) -> std::expected<Value, kernel::KernelError> {
            // Deterministic session orientation, so the model doesn't burn calls
            // rediscovering the obvious: workspace name, top-level entries,
            // detected project markers, and the live tool/task verbs.
            Value out = Value::object();
            std::string root_name = base.filename().string();
            if (root_name.empty()) {
                root_name = base.parent_path().filename().string();
            }
            out.set("root", Value::str(root_name.empty() ? base.string() : root_name));

            Value::List entries;
            sandbox::OpResult listing = sandbox::run_fs_op("list", base, "");
            if (listing.ok) {
                std::vector<std::string> lines = util::detail::split_lines(listing.output);
                constexpr std::size_t kCap = 200;
                for (std::size_t i = 0; i < lines.size() && i < kCap; ++i) {
                    entries.push_back(Value::str(lines[i]));
                }
            }
            out.set("entries", Value::list(std::move(entries)));

            Value::List markers;
            for (const char* m : {"CMakeLists.txt", "Cargo.toml", "go.mod", "package.json",
                                  "pyproject.toml", "Makefile", "meson.build", ".git"}) {
                std::error_code ec;
                if (std::filesystem::exists(base / m, ec)) {
                    markers.push_back(Value::str(m));
                }
            }
            out.set("markers", Value::list(std::move(markers)));

            Value::List tools, tasks;
            for (const auto& [name, comp] : opts_.components) {
                if (comp.kind == "tool") {
                    tools.push_back(Value::str(name));
                } else if (comp.kind == "task") {
                    tasks.push_back(Value::str(name));
                }
            }
            out.set("tools", Value::list(std::move(tools)));
            out.set("tasks", Value::list(std::move(tasks)));
            return out;
        });

    registry_.add_effect(
        "diff.workspace", "diff.workspace",
        [this](const Value&) -> std::expected<Value, kernel::KernelError> {
            // The net change per path from the session write-journal — kernel-
            // generated, git-free, and scoped to what the agent itself wrote
            // (first-seen `before` vs latest `after`). The read-side counterpart
            // to fs.edit: make the change, then inspect and explain it.
            std::map<std::string, std::pair<std::string, std::string>> nets;
            std::vector<std::string> order;
            for (const WriteRecord& w : journal_) {
                auto [it, inserted] = nets.try_emplace(w.path, w.before, w.after);
                if (inserted) {
                    order.push_back(w.path);
                } else {
                    it->second.second = w.after;
                }
            }
            Value::List files;
            std::string unified;
            for (const std::string& path : order) {
                const auto& [bef, aft] = nets[path];
                std::string d = util::unified_diff(bef, aft);
                if (d.empty()) {
                    continue; // net no-op (written then reverted)
                }
                Value f = Value::object();
                f.set("path", Value::str(path));
                f.set("diff", Value::str(d));
                files.push_back(std::move(f));
                unified += "--- a/" + path + "\n+++ b/" + path + "\n" + d;
            }
            Value out = Value::object();
            out.set("count", Value::integer(static_cast<std::int64_t>(files.size())));
            out.set("files", Value::list(std::move(files)));
            out.set("unified", Value::str(std::move(unified)));
            return out;
        });

    registry_.add_effect(
        "component.propose", "component.propose",
        [this, base](const Value& args) -> std::expected<Value, kernel::KernelError> {
            // The model hands the kernel a candidate component (it cannot write
            // .astrite itself). The kernel validates + authority-folds it, stages
            // it for human review, and announces it. Proposing grants NOTHING —
            // acceptance (human, B4) is what installs it and pins a trust record.
            const Value* src = args.get("source");
            if (src == nullptr || src->as_str() == nullptr) {
                return std::unexpected(
                    kernel::KernelError{"component.propose: `source` must be a string"});
            }
            const std::string& source = *src->as_str();
            Value out = Value::object();
            auto comp = schema::load_component_jsonc(source);
            if (!comp.has_value()) {
                out.set("proposed", Value::boolean(false));
                out.set("errors", Value::list({Value::str(comp.error().message)}));
                return out; // invalid candidate is a result the model can fix, not an error
            }
            auto trace = kernel::check_component_authority(*comp, registry_);
            if (!trace.has_value()) {
                out.set("proposed", Value::boolean(false));
                out.set("errors", Value::list({Value::str(trace.error().message)}));
                return out;
            }
            if (!stage_proposal(base, comp->name, source)) {
                return std::unexpected(
                    kernel::KernelError{"component.propose: could not stage the proposal"});
            }
            const std::string risk = kernel::risk_tier(trace->reached);
            const bool replaces = opts_.components.contains(comp->name);
            events_.push(ComponentProposed{comp->name, comp->kind, comp->caps, risk,
                                           staged_proposal_path(base, comp->name).string(),
                                           replaces});
            out.set("proposed", Value::boolean(true));
            out.set("name", Value::str(comp->name));
            out.set("kind", Value::str(comp->kind));
            out.set("risk", Value::str(risk));
            out.set("replaces_existing", Value::boolean(replaces));
            out.set("note", Value::str("staged for human review; not active until approved"));
            return out;
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
            events_.push(AssistantSettled{turn->text, turn->usage.prompt_tokens,
                                          turn->usage.completion_tokens, turn->usage.total_tokens,
                                          turn->usage.present});

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
        // Both `tool` and `task` components are model-callable: the model selects
        // a verb by calling it. A task's argv is pinned, so it takes no model
        // arguments — it gets an empty object schema unless it declares one.
        if (component.kind != "tool" && component.kind != "task") {
            continue;
        }
        Value fn = Value::object();
        fn.set("name", Value::str(component.name));
        if (component.description.has_value()) {
            fn.set("description", Value::str(*component.description));
        }
        if (component.parameters.has_value()) {
            fn.set("parameters", *component.parameters);
        } else if (component.kind == "task") {
            Value schema = Value::object();
            schema.set("type", Value::str("object"));
            schema.set("properties", Value::object());
            fn.set("parameters", std::move(schema));
        }
        Value def = Value::object();
        def.set("type", Value::str("function"));
        def.set("function", std::move(fn));
        defs.push_back(std::move(def));
    }
    return Value::list(std::move(defs));
}

namespace {
std::string iso8601_now() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    ::gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}
} // namespace

std::vector<std::string> Runtime::pending_proposals() const {
    return list_staged_proposals(opts_.workspace_root);
}

std::expected<Runtime::ProposalSummary, std::string>
Runtime::accept_proposal(const std::string& name) {
    std::lock_guard turn_lock(turn_mu_); // mutates components/tool_defs; never mid-turn

    auto source = read_staged_proposal(opts_.workspace_root, name);
    if (!source.has_value()) {
        return std::unexpected("no staged proposal named `" + name + "`");
    }
    auto comp = schema::load_component_jsonc(*source);
    if (!comp.has_value()) {
        return std::unexpected("staged proposal is invalid: " + comp.error().message);
    }
    if (comp->name != name) {
        return std::unexpected("staged proposal declares a different name `" + comp->name + "`");
    }
    auto trace = kernel::check_component_authority(*comp, registry_);
    if (!trace.has_value()) {
        return std::unexpected("authority check failed: " + trace.error().message);
    }
    const std::string risk = kernel::risk_tier(trace->reached);

    // Kernel-owned writes — the trusted side may write the control plane the model
    // cannot. Install the component, then pin a trust record of the exact source.
    const std::filesystem::path comp_dir = opts_.workspace_root / ".astrite" / "components";
    std::error_code ec;
    std::filesystem::create_directories(comp_dir, ec);
    {
        std::ofstream out(comp_dir / (name + ".json"), std::ios::binary | std::ios::trunc);
        if (!out) {
            return std::unexpected("could not write the component file");
        }
        out << *source;
        if (!out.good()) {
            return std::unexpected("could not write the component file");
        }
    }
    TrustRecord record =
        make_trust_record(name, *source, comp->caps, risk, iso8601_now());
    if (!write_trust_record(opts_.workspace_root / ".astrite" / "trust", record)) {
        return std::unexpected("could not write the trust record");
    }

    ProposalSummary summary{comp->name, comp->kind, risk, comp->caps};
    opts_.components.insert_or_assign(name, std::move(*comp)); // install live
    tool_defs_ = tool_definitions();
    (void)discard_proposal(opts_.workspace_root, name); // best-effort cleanup
    return summary;
}

bool Runtime::reject_proposal(const std::string& name) {
    return discard_proposal(opts_.workspace_root, name);
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
