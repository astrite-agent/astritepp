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
// (task.run, M7) and self-re-exec (fs helpers / confined loader, M3/M7).
[[nodiscard]] int run_confined(const Policy& policy, const std::function<int()>& body);

} // namespace astrite::sandbox
