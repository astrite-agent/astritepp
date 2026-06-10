// The enforcement floor implementation — contained raw-syscall surface. Landlock
// and seccomp are one-way ratchets on a process and its descendants, so they are
// only ever applied inside a short-lived child; the long-running kernel keeps the
// broad access it legitimately needs (model network, terminal, project reads).

#include "sandbox/sandbox.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <linux/landlock.h>
#include <sys/prctl.h>
#include <sys/stat.h>
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

// ---- confined fs op implementations (run inside the helper child) ------------

constexpr std::size_t kReadCap = 4u * 1024 * 1024;     // fs.read content cap
constexpr std::size_t kCaptureCap = 8u * 1024 * 1024;  // parent capture ceiling
constexpr std::size_t kFindFileCap = 1u * 1024 * 1024; // skip files larger than this
constexpr std::size_t kListCap = 4096;                 // bounded directory listing
constexpr std::size_t kFindMatchCap = 500;             // bounded match output
constexpr std::size_t kFindLineCap = 200;              // truncate long matched lines

void write_str(int fd, std::string_view s) {
    const char* p = s.data();
    std::size_t n = s.size();
    while (n > 0) {
        ssize_t w = ::write(fd, p, n);
        if (w <= 0) {
            return;
        }
        p += w;
        n -= static_cast<std::size_t>(w);
    }
}

bool read_fd_all(int fd, std::string& out, std::size_t cap) {
    char buf[65536];
    for (;;) {
        ssize_t r = ::read(fd, buf, sizeof buf);
        if (r < 0) {
            return false;
        }
        if (r == 0) {
            break;
        }
        std::size_t got = static_cast<std::size_t>(r);
        if (out.size() + got > cap) {
            out.append(buf, cap - out.size());
            break;
        }
        out.append(buf, got);
    }
    return true;
}

std::string join(const std::string& base, const std::string& rel) {
    return rel.empty() ? base : base + "/" + rel;
}

int op_read(const std::string& base, const std::string& rel) {
    std::string full = join(base, rel);
    int fd = ::open(full.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        write_str(1, "fs.read: cannot open `" + rel + "`: " + std::strerror(errno));
        return 1;
    }
    struct stat st{};
    if (::fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
        ::close(fd);
        write_str(1, "fs.read: `" + rel + "` is a directory");
        return 1;
    }
    std::string out;
    bool ok = read_fd_all(fd, out, kReadCap);
    ::close(fd);
    if (!ok) {
        write_str(1, "fs.read: read error on `" + rel + "`");
        return 1;
    }
    write_str(1, out);
    return 0;
}

bool entry_kinds(DIR* d, dirent* e, bool& is_dir, bool& is_link, bool& is_reg) {
    is_dir = is_link = is_reg = false;
    if (e->d_type == DT_DIR) {
        is_dir = true;
    } else if (e->d_type == DT_LNK) {
        is_link = true;
    } else if (e->d_type == DT_REG) {
        is_reg = true;
    } else if (e->d_type == DT_UNKNOWN) {
        struct stat st{};
        if (::fstatat(::dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            return false;
        }
        is_dir = S_ISDIR(st.st_mode);
        is_link = S_ISLNK(st.st_mode);
        is_reg = S_ISREG(st.st_mode);
    }
    return true;
}

int op_list(const std::string& base, const std::string& rel) {
    std::string full = join(base, rel);
    DIR* d = ::opendir(full.c_str());
    if (d == nullptr) {
        write_str(1, "fs.list: cannot open `" + (rel.empty() ? std::string(".") : rel) +
                         "`: " + std::strerror(errno));
        return 1;
    }
    std::vector<std::string> entries;
    for (dirent* e = ::readdir(d); e != nullptr && entries.size() < kListCap; e = ::readdir(d)) {
        std::string name = e->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        bool is_dir = false, is_link = false, is_reg = false;
        entry_kinds(d, e, is_dir, is_link, is_reg);
        if (is_link) {
            continue; // never follow or surface symlinks
        }
        entries.push_back(is_dir ? name + "/" : name);
    }
    ::closedir(d);
    std::sort(entries.begin(), entries.end());
    std::string out;
    for (const std::string& s : entries) {
        out += s;
        out += '\n';
    }
    write_str(1, out);
    return 0;
}

bool skip_walk_dir(const std::string& name) {
    static const char* kSkip[] = {".git",        ".hg",    ".svn",  ".astrite",
                                  "node_modules", "target", "build", ".cache"};
    for (const char* s : kSkip) {
        if (name == s) {
            return true;
        }
    }
    return false;
}

int op_find(const std::string& base, const std::string& rel, const std::string& pattern) {
    if (pattern.empty()) {
        write_str(1, "search.find: empty pattern");
        return 1;
    }
    std::string out;
    std::size_t matches = 0;
    std::vector<std::string> stack{rel}; // dir paths relative to base ("" == root)
    while (!stack.empty() && matches < kFindMatchCap) {
        std::string dirrel = stack.back();
        stack.pop_back();
        DIR* d = ::opendir(join(base, dirrel).c_str());
        if (d == nullptr) {
            continue;
        }
        std::vector<std::string> subdirs, files;
        for (dirent* e = ::readdir(d); e != nullptr; e = ::readdir(d)) {
            std::string name = e->d_name;
            if (name == "." || name == "..") {
                continue;
            }
            bool is_dir = false, is_link = false, is_reg = false;
            entry_kinds(d, e, is_dir, is_link, is_reg);
            if (is_link) {
                continue;
            }
            std::string childrel = dirrel.empty() ? name : dirrel + "/" + name;
            if (is_dir && !skip_walk_dir(name)) {
                subdirs.push_back(std::move(childrel));
            } else if (is_reg) {
                files.push_back(std::move(childrel));
            }
        }
        ::closedir(d);
        std::sort(files.begin(), files.end());
        std::sort(subdirs.begin(), subdirs.end());
        for (auto it = subdirs.rbegin(); it != subdirs.rend(); ++it) {
            stack.push_back(*it);
        }
        for (const std::string& frel : files) {
            if (matches >= kFindMatchCap) {
                break;
            }
            int fd = ::open(join(base, frel).c_str(), O_RDONLY | O_CLOEXEC);
            if (fd < 0) {
                continue;
            }
            std::string content;
            read_fd_all(fd, content, kFindFileCap);
            ::close(fd);
            if (content.find('\0') != std::string::npos) {
                continue; // skip binary-ish files
            }
            std::size_t line_no = 0, pos = 0;
            while (pos <= content.size() && matches < kFindMatchCap) {
                std::size_t nl = content.find('\n', pos);
                std::string line = content.substr(
                    pos, nl == std::string::npos ? std::string::npos : nl - pos);
                ++line_no;
                if (line.find(pattern) != std::string::npos) {
                    if (line.size() > kFindLineCap) {
                        line = line.substr(0, kFindLineCap) + "...";
                    }
                    out += frel + ":" + std::to_string(line_no) + ":" + line + "\n";
                    ++matches;
                }
                if (nl == std::string::npos) {
                    break;
                }
                pos = nl + 1;
            }
        }
    }
    write_str(1, out);
    return 0;
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

OpResult run_fs_op(const std::string& op, const std::filesystem::path& base, const std::string& rel,
                   const std::string& extra) {
    // Build argv in the parent — the child does nothing but execv before its
    // address space is replaced, so no post-fork allocation in a threaded host.
    std::string base_s = base.string();
    std::vector<char*> av;
    av.push_back(const_cast<char*>("astrite"));
    av.push_back(const_cast<char*>("__sbx"));
    av.push_back(const_cast<char*>(op.c_str()));
    av.push_back(const_cast<char*>(base_s.c_str()));
    av.push_back(const_cast<char*>(rel.c_str()));
    if (!extra.empty()) {
        av.push_back(const_cast<char*>(extra.c_str()));
    }
    av.push_back(nullptr);

    int fds[2];
    if (::pipe(fds) != 0) {
        return {false, "fs op: pipe failed"};
    }
    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        return {false, "fs op: fork failed"};
    }
    if (pid == 0) {
        ::close(fds[0]);
        ::dup2(fds[1], STDOUT_FILENO);
        ::close(fds[1]);
        ::execv("/proc/self/exe", av.data());
        ::_exit(127); // exec failed
    }
    ::close(fds[1]);
    std::string output;
    read_fd_all(fds[0], output, kCaptureCap);
    ::close(fds[0]);
    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) {
        return {false, "fs op: wait failed"};
    }
    bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    return {ok, std::move(output)};
}

int maybe_run_helper(int argc, char** argv) {
    if (argc < 2 || std::strcmp(argv[1], "__sbx") != 0) {
        return -1; // not a helper invocation; caller proceeds normally
    }
    if (argc < 5) {
        write_str(1, "sbx: usage: __sbx <op> <base> <rel> [extra]");
        return 2;
    }
    std::string op = argv[2];
    std::string base = argv[3];
    std::string rel = argv[4];
    std::string extra = argc > 5 ? argv[5] : "";

    Policy policy;
    policy.read_paths = {base}; // read-only confinement to the workspace base
    if (!apply(policy)) {
        write_str(1, "sbx: sandbox apply failed");
        return 126;
    }
    if (op == "read") {
        return op_read(base, rel);
    }
    if (op == "list") {
        return op_list(base, rel);
    }
    if (op == "find") {
        return op_find(base, rel, extra);
    }
    write_str(1, "sbx: unknown op `" + op + "`");
    return 2;
}

} // namespace astrite::sandbox
