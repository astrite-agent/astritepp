#pragma once

// The enforcement floor — the one place in astrite++ that issues raw syscalls.
// A Policy names the filesystem bases a confined child may read or write;
// network is denied unconditionally at this layer (per-task net relaxation is
// an M7 concern). apply() is the irreversible ratchet; run_confined() forks a
// child, applies it, and runs a body under it.

#include <filesystem>
#include <functional>
#include <vector>

namespace astrite::sandbox {

struct Policy {
    std::vector<std::filesystem::path> read_paths;  // read + list + execute
    std::vector<std::filesystem::path> write_paths; // the above + create/write/remove
};

// Existing entries among the standard toolchain/system dirs, granted read+exec
// so dynamically-linked tools can run under the policy.
[[nodiscard]] std::vector<std::filesystem::path> system_read_exec_paths();

// Query the kernel's Landlock ABI version (>0 supported, <=0 unavailable).
[[nodiscard]] int landlock_abi();

// Fork a child, apply the ratchet (no_new_privs -> seccomp -> Landlock), and run
// `body` inside it. Returns the child's exit status, or -1 on fork/wait failure.
// If the ratchet fails the child exits 126 and `body` never runs (fail closed).
//
// NOTE: `body` runs after fork in a child that does not exec immediately, so it
// must stay async-signal-safe-ish (no touching parent locks). This is the test
// and single-threaded path; the runtime's production delivery is fork+exec
// (task.run, M7) and self-re-exec (fs helpers, below).
[[nodiscard]] int run_confined(const Policy& policy, const std::function<int()>& body);

// The result of a confined fs operation: `ok` is the child's clean exit; `output`
// is the captured payload (file content / listing / matches) or the error text.
struct OpResult {
    bool ok = false;
    std::string output;
};

// Run a read-only fs op in a fresh, confined child via self-re-exec:
// fork + exec(/proc/self/exe __sbx <op> <base> <rel> [extra]), which applies the
// ratchet read-confined to `base` and performs the op. This is safe to call from
// a multithreaded process — the child execs immediately into a clean address
// space. `op` is "read" | "list" | "find"; `rel` is relative to `base` ("" for
// the base root); `extra` is the pattern for "find".
[[nodiscard]] OpResult run_fs_op(const std::string& op, const std::filesystem::path& base,
                                 const std::string& rel, const std::string& extra = "");

// Write-confined variants: the child applies a write policy on `base` and receives
// its payload over stdin (so large content never goes through argv). fs_write
// creates/overwrites; fs_edit does exact-match replacement, failing unless the
// number of matches equals `expected`.
[[nodiscard]] OpResult fs_write(const std::filesystem::path& base, const std::string& rel,
                                const std::string& content);
[[nodiscard]] OpResult fs_edit(const std::filesystem::path& base, const std::string& rel,
                               const std::string& old_text, const std::string& new_text,
                               std::size_t expected);

// ---- task execution (M7): the only path that runs a *declared external* program.
// Unlike the fs helpers (which self-re-exec /proc/self/exe), run_task forks and
// execs a program named by a pinned argv template. The model never reaches this:
// it selects a task verb; the kernel owns the argv. Confinement is the same floor
// (no_new_privs -> seccomp net-deny -> Landlock) with read+exec on the declared
// read bases and write on the declared write bases. stdout/stderr are captured
// separately and capped; a timeout kills the child's whole process group.

struct TaskSpec {
    std::vector<std::string> argv;                  // argv[0] = program (PATH-resolved if bare)
    std::filesystem::path cwd;                      // working directory (absolute)
    std::vector<std::filesystem::path> read_paths;  // read + execute bases
    std::vector<std::filesystem::path> write_paths; // write bases (also readable)
    int timeout_ms = 0;                             // 0 == no timeout
    std::size_t output_cap = 256u * 1024;           // per-stream capture ceiling
};

struct TaskResult {
    bool spawned = false;        // the child applied the ratchet and exec'd the program
    int code = -1;               // exit code (>=0) or -signal if killed by a signal
    bool timed_out = false;      // killed because it exceeded timeout_ms
    bool out_truncated = false;  // stdout hit output_cap
    bool err_truncated = false;  // stderr hit output_cap
    std::string out;             // captured stdout (<= output_cap)
    std::string err;             // captured stderr (<= output_cap)
    std::string spawn_error;     // why the spawn failed, when !spawned
};

[[nodiscard]] TaskResult run_task(const TaskSpec& spec);

// The hidden-subcommand entry point. Frontends (and the test binary) call this
// first in main(): if argv is a `__sbx` invocation it runs the confined op and
// returns the exit code; otherwise it returns -1 and main() proceeds normally.
[[nodiscard]] int maybe_run_helper(int argc, char** argv);

} // namespace astrite::sandbox
