#pragma once

// Pure translation between the kernel value world and sandbox::run_task. Kept
// out of runtime.cpp so the spec validation (the part that turns inert,
// component-authored task data into a confined TaskSpec) and the result mapping
// are unit-testable without forking a child. The model never authors a spec —
// argv is pinned in the component — but we still validate and confine defensively
// (a buggy component must not be able to escape the workspace).

#include <algorithm>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

#include "kernel/error.hpp"
#include "kernel/trust.hpp"
#include "sandbox/sandbox.hpp"
#include "schema/value.hpp"

namespace astrite::runtime {

inline constexpr int kTaskDefaultTimeoutMs = 120'000;            // 2 min
inline constexpr std::size_t kTaskMaxTimeoutMs = 600'000;        // 10 min ceiling
inline constexpr std::size_t kTaskDefaultOutputCap = 256u * 1024;
inline constexpr std::size_t kTaskMaxOutputCap = 4u * 1024 * 1024; // 4 MiB ceiling

namespace detail {

// Resolve a workspace-relative subpath to an absolute path under `root`. The
// anchors "", ".", and "workspace" mean the root itself; everything else goes
// through the path.resolve belt (rejecting absolute paths and `..` escapes).
[[nodiscard]] inline std::expected<std::filesystem::path, kernel::KernelError>
confine_under(const std::filesystem::path& root, const std::string& rel, const char* what) {
    if (rel.empty() || rel == "." || rel == "workspace") {
        return root;
    }
    schema::Value rq = schema::Value::object();
    rq.set("path", schema::Value::str(rel));
    rq.set("base", schema::Value::str("workspace"));
    schema::Value resolved = kernel::resolve_path(rq);
    const schema::Value* ok = resolved.get("ok");
    if (ok == nullptr || ok->as_bool() == nullptr || !*ok->as_bool()) {
        const schema::Value* reason = resolved.get("reason");
        return std::unexpected(kernel::KernelError{
            std::string("task: ") + what + ": " +
            (reason != nullptr && reason->as_str() != nullptr ? *reason->as_str() : "bad path")});
    }
    return root / *resolved.get("rel")->as_str();
}

} // namespace detail

// Turn a (trusted, component-authored) task spec value into a confined TaskSpec.
// argv is required and fully pinned (no holes in the MVP). cwd defaults to the
// workspace root; declared `writable` subtrees become the only write bases;
// reads are always workspace + system toolchain dirs. net is rejected (deferred).
[[nodiscard]] inline std::expected<sandbox::TaskSpec, kernel::KernelError>
build_task_spec(const schema::Value& spec, const std::filesystem::path& workspace_root) {
    using schema::Value;
    if (!spec.is_object()) {
        return std::unexpected(kernel::KernelError{"task: spec must be an object"});
    }
    if (const Value* net = spec.get("net");
        net != nullptr && net->as_bool() != nullptr && *net->as_bool()) {
        return std::unexpected(kernel::KernelError{"task: network access is not yet supported"});
    }
    if (const Value* eff = spec.get("effect"); eff != nullptr) {
        const std::string* s = eff->as_str();
        if (s == nullptr || (*s != "check" && *s != "produce")) {
            return std::unexpected(
                kernel::KernelError{"task: `effect` must be \"check\" or \"produce\""});
        }
    }

    const Value* argv_v = spec.get("argv");
    if (argv_v == nullptr || argv_v->as_list() == nullptr || argv_v->as_list()->empty()) {
        return std::unexpected(kernel::KernelError{"task: `argv` must be a non-empty list"});
    }
    sandbox::TaskSpec out;
    for (const Value& a : *argv_v->as_list()) {
        if (a.as_str() == nullptr) {
            return std::unexpected(
                kernel::KernelError{"task: every `argv` element must be a string"});
        }
        out.argv.push_back(*a.as_str());
    }

    std::string cwd = "workspace";
    if (const Value* c = spec.get("cwd"); c != nullptr) {
        if (c->as_str() == nullptr) {
            return std::unexpected(kernel::KernelError{"task: `cwd` must be a string"});
        }
        cwd = *c->as_str();
    }
    auto cwd_abs = detail::confine_under(workspace_root, cwd, "cwd");
    if (!cwd_abs.has_value()) {
        return std::unexpected(cwd_abs.error());
    }
    out.cwd = *cwd_abs;

    out.read_paths = sandbox::system_read_exec_paths();
    out.read_paths.push_back(workspace_root);

    if (const Value* w = spec.get("writable"); w != nullptr) {
        if (w->as_list() == nullptr) {
            return std::unexpected(kernel::KernelError{"task: `writable` must be a list"});
        }
        for (const Value& p : *w->as_list()) {
            if (p.as_str() == nullptr) {
                return std::unexpected(
                    kernel::KernelError{"task: every `writable` entry must be a string"});
            }
            auto abs = detail::confine_under(workspace_root, *p.as_str(), "writable");
            if (!abs.has_value()) {
                return std::unexpected(abs.error());
            }
            out.write_paths.push_back(*abs);
        }
    }

    int timeout = kTaskDefaultTimeoutMs;
    if (const Value* t = spec.get("timeout_ms"); t != nullptr) {
        if (t->as_int() == nullptr) {
            return std::unexpected(kernel::KernelError{"task: `timeout_ms` must be an integer"});
        }
        long v = static_cast<long>(*t->as_int());
        v = std::max(v, 0L);
        v = std::min(v, static_cast<long>(kTaskMaxTimeoutMs));
        timeout = static_cast<int>(v);
    }
    out.timeout_ms = timeout;

    std::size_t cap = kTaskDefaultOutputCap;
    if (const Value* o = spec.get("output_cap_bytes"); o != nullptr) {
        if (o->as_int() == nullptr) {
            return std::unexpected(
                kernel::KernelError{"task: `output_cap_bytes` must be an integer"});
        }
        long v = static_cast<long>(*o->as_int());
        v = std::max(v, 1L);
        v = std::min(v, static_cast<long>(kTaskMaxOutputCap));
        cap = static_cast<std::size_t>(v);
    }
    out.output_cap = cap;

    return out;
}

// Map a TaskResult to the structured value the model sees. status is the single
// field a component branches on: passed | failed | timed_out | error.
[[nodiscard]] inline schema::Value task_result_value(const sandbox::TaskResult& r) {
    using schema::Value;
    const char* status = !r.spawned          ? "error"
                         : r.timed_out        ? "timed_out"
                         : (r.code == 0)      ? "passed"
                                              : "failed";
    Value v = Value::object();
    v.set("status", Value::str(status));
    v.set("code", Value::integer(r.code));
    v.set("timed_out", Value::boolean(r.timed_out));
    v.set("stdout", Value::str(r.out));
    v.set("stderr", Value::str(r.err));
    v.set("stdout_truncated", Value::boolean(r.out_truncated));
    v.set("stderr_truncated", Value::boolean(r.err_truncated));
    if (!r.spawned) {
        v.set("error", Value::str(r.spawn_error));
    }
    return v;
}

} // namespace astrite::runtime
