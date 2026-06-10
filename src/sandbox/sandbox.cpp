// The enforcement floor implementation — contained raw-syscall surface. Landlock
// and seccomp are one-way ratchets on a process and its descendants, so they are
// only ever applied inside a short-lived child; the long-running kernel keeps the
// broad access it legitimately needs (model network, terminal, project reads).

#include "sandbox/sandbox.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>

#include <fcntl.h>
#include <linux/landlock.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "sandbox/seccomp.hpp"

namespace astrite::sandbox {
namespace {

// Full set of filesystem rights we ever govern; per-path grants are a subset.
constexpr std::uint64_t kFsRead =
    LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_EXECUTE;
constexpr std::uint64_t kFsWriteExtra =
    LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_REMOVE_DIR |
    LANDLOCK_ACCESS_FS_REMOVE_FILE | LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR |
    LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_FIFO |
    LANDLOCK_ACCESS_FS_MAKE_BLOCK | LANDLOCK_ACCESS_FS_MAKE_SYM;
// Newer rights may postdate the build headers even on a newer kernel; guard each
// so the floor tracks what the headers know and the ABI clamp does the rest.
#ifdef LANDLOCK_ACCESS_FS_REFER
constexpr std::uint64_t kFsRefer = LANDLOCK_ACCESS_FS_REFER; // ABI >= 2
#else
constexpr std::uint64_t kFsRefer = 0;
#endif
#ifdef LANDLOCK_ACCESS_FS_TRUNCATE
constexpr std::uint64_t kFsTruncate = LANDLOCK_ACCESS_FS_TRUNCATE; // ABI >= 3
#else
constexpr std::uint64_t kFsTruncate = 0;
#endif
#ifdef LANDLOCK_ACCESS_FS_IOCTL_DEV
constexpr std::uint64_t kFsIoctlDev = LANDLOCK_ACCESS_FS_IOCTL_DEV; // ABI >= 5
#else
constexpr std::uint64_t kFsIoctlDev = 0;
#endif
#if defined(LANDLOCK_ACCESS_NET_BIND_TCP) && defined(LANDLOCK_ACCESS_NET_CONNECT_TCP)
constexpr std::uint64_t kNet = LANDLOCK_ACCESS_NET_BIND_TCP | LANDLOCK_ACCESS_NET_CONNECT_TCP;
#else
constexpr std::uint64_t kNet = 0;
#endif

long ll_create_ruleset(const landlock_ruleset_attr* attr, std::size_t size, std::uint32_t flags) {
    return ::syscall(__NR_landlock_create_ruleset, attr, size, flags);
}
long ll_add_rule(int ruleset_fd, landlock_rule_type type, const void* attr, std::uint32_t flags) {
    return ::syscall(__NR_landlock_add_rule, ruleset_fd, type, attr, flags);
}
long ll_restrict_self(int ruleset_fd, std::uint32_t flags) {
    return ::syscall(__NR_landlock_restrict_self, ruleset_fd, flags);
}

// Mask the full right sets down to what the running kernel's ABI understands —
// passing unknown bits is rejected, so this keeps the floor portable forward.
struct FsMasks {
    std::uint64_t fs;
    std::uint64_t net;
};
FsMasks abi_masks(int abi) {
    std::uint64_t fs = kFsRead | kFsWriteExtra;
    std::uint64_t net = 0;
    if (abi >= 2) {
        fs |= kFsRefer;
    }
    if (abi >= 3) {
        fs |= kFsTruncate;
    }
    if (abi >= 4) {
        net = kNet;
    }
    if (abi >= 5) {
        fs |= kFsIoctlDev;
    }
    return {fs, net};
}

bool add_path(int ruleset_fd, const std::filesystem::path& path, std::uint64_t allowed) {
    int parent = ::open(path.c_str(), O_PATH | O_CLOEXEC);
    if (parent < 0) {
        return false; // unopenable base: skip it (fail closed — it stays denied)
    }
    landlock_path_beneath_attr pb{};
    pb.allowed_access = allowed;
    pb.parent_fd = parent;
    long rc = ll_add_rule(ruleset_fd, LANDLOCK_RULE_PATH_BENEATH, &pb, 0);
    ::close(parent);
    return rc == 0;
}

// Apply the Landlock leg: a ruleset handling the full right set (clamped to ABI),
// path rules granting the requested subset per base, and no net rules at all so
// all TCP bind/connect is denied. Returns false on hard failure (fail closed).
bool apply_landlock(const Policy& policy) {
    int abi = landlock_abi();
    if (abi <= 0) {
        return false; // no enforcement floor available -> deny
    }
    FsMasks masks = abi_masks(abi);

    landlock_ruleset_attr attr{};
    attr.handled_access_fs = masks.fs;
    attr.handled_access_net = masks.net;

    int ruleset_fd = static_cast<int>(ll_create_ruleset(&attr, sizeof(attr), 0));
    if (ruleset_fd < 0) {
        return false;
    }

    const std::uint64_t read_rights = kFsRead & masks.fs;
    const std::uint64_t write_rights = (kFsRead | kFsWriteExtra | kFsTruncate) & masks.fs;
    for (const auto& path : policy.read_paths) {
        add_path(ruleset_fd, path, read_rights); // best-effort: missing base stays denied
    }
    for (const auto& path : policy.write_paths) {
        add_path(ruleset_fd, path, write_rights);
    }

    bool ok = ll_restrict_self(ruleset_fd, 0) == 0;
    ::close(ruleset_fd);
    return ok;
}

// The full ratchet, in order. Each step fails closed.
bool apply(const Policy& policy) {
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        return false;
    }
    if (!install_inet_deny()) {
        return false;
    }
    return apply_landlock(policy);
}

} // namespace

bool install_inet_deny() {
    // sock_fprog.filter is non-const; copy the constexpr program into a mutable buffer.
    static thread_local std::array<sock_filter, inet_deny_program.size()> buf = inet_deny_program;
    sock_fprog prog{static_cast<unsigned short>(buf.size()), buf.data()};
    return ::syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER, 0, &prog) == 0;
}

int landlock_abi() {
    return static_cast<int>(
        ll_create_ruleset(nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION));
}

std::vector<std::filesystem::path> system_read_exec_paths() {
    std::vector<std::filesystem::path> out;
    for (const char* candidate : {"/usr", "/lib", "/lib64", "/bin", "/sbin", "/etc"}) {
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) {
            out.emplace_back(candidate);
        }
    }
    return out;
}

int run_confined(const Policy& policy, const std::function<int()>& body) {
    pid_t pid = ::fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        if (!apply(policy)) {
            ::_exit(126); // ratchet failed -> never run the body
        }
        int code = body();
        ::_exit(code & 0xff);
    }
    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) {
        return -1;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1; // killed by signal (e.g. seccomp KILL on arch mismatch)
}

} // namespace astrite::sandbox
