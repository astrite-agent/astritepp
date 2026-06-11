// The enforcement floor implementation — contained raw-syscall surface. Landlock
// and seccomp are one-way ratchets on a process and its descendants, so they are
// only ever applied inside a short-lived child; the long-running kernel keeps the
// broad access it legitimately needs (model network, terminal, project reads).

#include "sandbox/sandbox.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <ctime>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <csignal>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <linux/landlock.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "sandbox/range.hpp"
#include "sandbox/seccomp.hpp"
#include "util/control_plane.hpp"

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

// "offset:limit" -> (offset, limit). Empty / malformed fields keep the defaults
// (offset 1, limit 0 == whole file), so a bad range degrades to a full read.
void parse_read_range(const std::string& spec, long& offset, long& limit) {
    auto to_long = [](std::string_view s, long def) -> long {
        if (s.empty()) {
            return def;
        }
        long v = def;
        auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
        return ec == std::errc{} ? v : def;
    };
    std::size_t colon = spec.find(':');
    if (colon == std::string::npos) {
        offset = to_long(spec, offset);
    } else {
        offset = to_long(std::string_view(spec).substr(0, colon), offset);
        limit = to_long(std::string_view(spec).substr(colon + 1), limit);
    }
}

int op_read(const std::string& base, const std::string& rel, long offset, long limit) {
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
    if (offset > 1 || limit > 0) {
        write_str(1, slice_lines(out, offset, limit)); // identity slice == whole file
    } else {
        write_str(1, out);
    }
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

bool write_file_full(const std::string& path, const std::string& content) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return false;
    }
    const char* p = content.data();
    std::size_t n = content.size();
    bool ok = true;
    while (n > 0) {
        ssize_t w = ::write(fd, p, n);
        if (w <= 0) {
            ok = false;
            break;
        }
        p += w;
        n -= static_cast<std::size_t>(w);
    }
    ::close(fd);
    return ok;
}

int op_write(const std::string& base, const std::string& rel, const std::string& content) {
    if (is_control_plane_rel(rel)) { // suspenders: independent of the runtime belt
        write_str(1, "fs.write: the control plane (.astrite) is read-only to components");
        return 1;
    }
    if (!write_file_full(join(base, rel), content)) {
        write_str(1, "fs.write: cannot write `" + rel + "`: " + std::strerror(errno));
        return 1;
    }
    write_str(1, "wrote " + std::to_string(content.size()) + " bytes to " + rel);
    return 0;
}

// stdin payload framing for edit: [u64 old_len][old][u64 new_len][new], native
// byte order (same binary on both ends). Returns false on a malformed frame.
bool parse_edit_payload(const std::string& payload, std::string& old_text, std::string& new_text) {
    std::size_t pos = 0;
    auto take_len = [&](std::uint64_t& out) {
        if (pos + sizeof(out) > payload.size()) {
            return false;
        }
        std::memcpy(&out, payload.data() + pos, sizeof(out));
        pos += sizeof(out);
        return true;
    };
    std::uint64_t ol = 0;
    if (!take_len(ol) || pos + ol > payload.size()) {
        return false;
    }
    old_text.assign(payload, pos, ol);
    pos += ol;
    std::uint64_t nl = 0;
    if (!take_len(nl) || pos + nl > payload.size()) {
        return false;
    }
    new_text.assign(payload, pos, nl);
    return true;
}

int op_edit(const std::string& base, const std::string& rel, const std::string& expected_s,
            const std::string& payload) {
    if (is_control_plane_rel(rel)) { // suspenders: independent of the runtime belt
        write_str(1, "fs.edit: the control plane (.astrite) is read-only to components");
        return 1;
    }
    std::string old_text, new_text;
    if (!parse_edit_payload(payload, old_text, new_text)) {
        write_str(1, "fs.edit: malformed payload");
        return 1;
    }
    if (old_text.empty()) {
        write_str(1, "fs.edit: old_text must not be empty");
        return 1;
    }
    std::size_t expected = expected_s.empty() ? 1 : std::strtoul(expected_s.c_str(), nullptr, 10);

    std::string full = join(base, rel);
    int fd = ::open(full.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        write_str(1, "fs.edit: cannot open `" + rel + "`: " + std::strerror(errno));
        return 1;
    }
    std::string content;
    bool read_ok = read_fd_all(fd, content, kReadCap);
    ::close(fd);
    if (!read_ok) {
        write_str(1, "fs.edit: read error on `" + rel + "`");
        return 1;
    }

    std::size_t count = 0;
    for (std::size_t at = content.find(old_text); at != std::string::npos;
         at = content.find(old_text, at + old_text.size())) {
        ++count;
    }
    if (count != expected) {
        write_str(1, "fs.edit: expected " + std::to_string(expected) + " occurrence(s) of " +
                         "old_text in `" + rel + "`, found " + std::to_string(count));
        return 1;
    }

    std::string result;
    result.reserve(content.size());
    std::size_t pos = 0;
    for (std::size_t at = content.find(old_text); at != std::string::npos;
         at = content.find(old_text, pos)) {
        result.append(content, pos, at - pos);
        result += new_text;
        pos = at + old_text.size();
    }
    result.append(content, pos, std::string::npos);

    if (!write_file_full(full, result)) {
        write_str(1, "fs.edit: cannot write `" + rel + "`: " + std::strerror(errno));
        return 1;
    }
    write_str(1, "replaced " + std::to_string(count) + " occurrence(s) in " + rel);
    return 0;
}

std::string frame_edit_payload(const std::string& old_text, const std::string& new_text) {
    std::string out;
    std::uint64_t ol = old_text.size();
    std::uint64_t nl = new_text.size();
    out.append(reinterpret_cast<const char*>(&ol), sizeof(ol));
    out += old_text;
    out.append(reinterpret_cast<const char*>(&nl), sizeof(nl));
    out += new_text;
    return out;
}

std::string read_stdin_all() {
    std::string in;
    read_fd_all(STDIN_FILENO, in, kCaptureCap);
    return in;
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

// Resolve a task program to an absolute path in the PARENT (so the child only
// execve's a known path — no PATH search post-fork, and a missing tool fails
// cleanly before we ever fork). A name containing '/' is used verbatim.
std::string resolve_program(const std::string& prog) {
    if (prog.find('/') != std::string::npos) {
        return ::access(prog.c_str(), X_OK) == 0 ? prog : std::string{};
    }
    const char* path = ::getenv("PATH");
    std::string dirs = path != nullptr ? path : "/usr/bin:/bin";
    std::size_t start = 0;
    while (true) {
        std::size_t colon = dirs.find(':', start);
        std::string dir = dirs.substr(start, colon == std::string::npos ? colon : colon - start);
        if (dir.empty()) {
            dir = ".";
        }
        std::string cand = dir + "/" + prog;
        if (::access(cand.c_str(), X_OK) == 0) {
            return cand;
        }
        if (colon == std::string::npos) {
            return std::string{};
        }
        start = colon + 1;
    }
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

namespace {

// Self-re-exec a confined child, send `payload` on its stdin, capture its stdout.
// Used by the write ops: content too large for argv flows through the pipe, and
// the child reads all of stdin before writing status, so there is no deadlock.
OpResult run_with_stdin(const std::string& op, const std::filesystem::path& base,
                        const std::string& rel, const std::string& extra,
                        const std::string& payload) {
    static const auto ignore_sigpipe = [] {
        ::signal(SIGPIPE, SIG_IGN); // a child that dies early must not signal us
        return 0;
    }();
    (void)ignore_sigpipe;

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

    int in_fds[2];
    int out_fds[2];
    if (::pipe(in_fds) != 0) {
        return {false, "fs op: pipe failed"};
    }
    if (::pipe(out_fds) != 0) {
        ::close(in_fds[0]);
        ::close(in_fds[1]);
        return {false, "fs op: pipe failed"};
    }
    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(in_fds[0]);
        ::close(in_fds[1]);
        ::close(out_fds[0]);
        ::close(out_fds[1]);
        return {false, "fs op: fork failed"};
    }
    if (pid == 0) {
        ::dup2(in_fds[0], STDIN_FILENO);
        ::dup2(out_fds[1], STDOUT_FILENO);
        ::close(in_fds[0]);
        ::close(in_fds[1]);
        ::close(out_fds[0]);
        ::close(out_fds[1]);
        ::execv("/proc/self/exe", av.data());
        ::_exit(127);
    }
    ::close(in_fds[0]);
    ::close(out_fds[1]);
    // Send the payload, then EOF, before reading the (small) status back.
    const char* p = payload.data();
    std::size_t n = payload.size();
    while (n > 0) {
        ssize_t w = ::write(in_fds[1], p, n);
        if (w <= 0) {
            break; // child gone (EPIPE) — its status/exit code carries the failure
        }
        p += w;
        n -= static_cast<std::size_t>(w);
    }
    ::close(in_fds[1]);
    std::string output;
    read_fd_all(out_fds[0], output, kCaptureCap);
    ::close(out_fds[0]);
    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) {
        return {false, "fs op: wait failed"};
    }
    bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    return {ok, std::move(output)};
}

} // namespace

OpResult fs_write(const std::filesystem::path& base, const std::string& rel,
                  const std::string& content) {
    return run_with_stdin("write", base, rel, "", content);
}

OpResult fs_edit(const std::filesystem::path& base, const std::string& rel,
                 const std::string& old_text, const std::string& new_text, std::size_t expected) {
    return run_with_stdin("edit", base, rel, std::to_string(expected),
                          frame_edit_payload(old_text, new_text));
}

TaskResult run_task(const TaskSpec& spec) {
    static const auto ignore_sigpipe = [] {
        ::signal(SIGPIPE, SIG_IGN); // a killed child must never signal us
        return 0;
    }();
    (void)ignore_sigpipe;

    TaskResult res;
    if (spec.argv.empty()) {
        res.spawn_error = "task: empty argv";
        return res;
    }
    std::string prog = resolve_program(spec.argv[0]);
    if (prog.empty()) {
        res.spawn_error = "task: program not found or not executable: " + spec.argv[0];
        return res;
    }

    // Everything the child touches before execve is built HERE, in the parent —
    // no heap allocation between fork and exec (safe from a threaded host).
    std::vector<std::string> argv_storage = spec.argv;
    argv_storage[0] = prog;
    std::vector<char*> argv;
    argv.reserve(argv_storage.size() + 1);
    for (std::string& a : argv_storage) {
        argv.push_back(a.data());
    }
    argv.push_back(nullptr);

    // A curated environment: enough for a toolchain to find its sub-tools, no more.
    std::vector<std::string> env_storage;
    for (const char* key : {"PATH", "HOME", "LANG", "LC_ALL", "TMPDIR"}) {
        if (const char* v = ::getenv(key)) {
            env_storage.push_back(std::string(key) + "=" + v);
        }
    }
    env_storage.emplace_back("TERM=dumb");
    std::vector<char*> envp;
    envp.reserve(env_storage.size() + 1);
    for (std::string& e : env_storage) {
        envp.push_back(e.data());
    }
    envp.push_back(nullptr);

    std::string cwd = spec.cwd.string();
    Policy policy;
    policy.read_paths = spec.read_paths;
    policy.write_paths = spec.write_paths;
    // The irreducible device floor every program expects (read+write on /dev).
    // Without it, ninja's posix_spawn — which redirects a child's stdin from
    // /dev/null — fails EACCES, and compilers can't `-o /dev/null` in feature
    // checks. Granted here, not by the caller, because it is a property of
    // "running a program," not of any one task's declared policy. Granting the
    // whole /dev is safe: Landlock only ever *removes* access, so ordinary DAC
    // permissions remain the backstop — a non-root task still cannot open block
    // devices, /dev/mem, etc.; only world-accessible nodes (/dev/null, …) work.
    {
        std::error_code ec;
        if (std::filesystem::exists("/dev", ec)) {
            policy.write_paths.emplace_back("/dev");
        }
    }

    int out_fds[2];
    int err_fds[2];
    int sync_fds[2]; // child -> parent failure channel; write end closes on a clean exec
    if (::pipe(out_fds) != 0) {
        res.spawn_error = "task: pipe failed";
        return res;
    }
    if (::pipe(err_fds) != 0) {
        ::close(out_fds[0]);
        ::close(out_fds[1]);
        res.spawn_error = "task: pipe failed";
        return res;
    }
    if (::pipe(sync_fds) != 0) {
        ::close(out_fds[0]);
        ::close(out_fds[1]);
        ::close(err_fds[0]);
        ::close(err_fds[1]);
        res.spawn_error = "task: pipe failed";
        return res;
    }
    ::fcntl(sync_fds[1], F_SETFD, FD_CLOEXEC); // EOF on the read end == exec succeeded

    pid_t pid = ::fork();
    if (pid < 0) {
        for (int fd : {out_fds[0], out_fds[1], err_fds[0], err_fds[1], sync_fds[0], sync_fds[1]}) {
            ::close(fd);
        }
        res.spawn_error = "task: fork failed";
        return res;
    }
    if (pid == 0) {
        ::setpgid(0, 0); // own process group so the parent can kill the whole tree
        int devnull = ::open("/dev/null", O_RDONLY | O_CLOEXEC); // before the ratchet
        if (devnull >= 0) {
            ::dup2(devnull, STDIN_FILENO);
            ::close(devnull);
        }
        ::dup2(out_fds[1], STDOUT_FILENO);
        ::dup2(err_fds[1], STDERR_FILENO);
        for (int fd : {out_fds[0], out_fds[1], err_fds[0], err_fds[1], sync_fds[0]}) {
            ::close(fd);
        }
        if (::chdir(cwd.c_str()) != 0) {
            char c = 'C';
            ::write(sync_fds[1], &c, 1);
            ::_exit(126);
        }
        if (!apply(policy)) {
            char c = 'R';
            ::write(sync_fds[1], &c, 1);
            ::_exit(126);
        }
        ::execve(prog.c_str(), argv.data(), envp.data());
        char c = 'X';
        ::write(sync_fds[1], &c, 1);
        ::_exit(127);
    }

    ::setpgid(pid, pid); // race-safe with the child's own setpgid
    ::close(out_fds[1]);
    ::close(err_fds[1]);
    ::close(sync_fds[1]);

    struct timespec start{};
    ::clock_gettime(CLOCK_MONOTONIC, &start);
    auto elapsed_ms = [&start]() -> long {
        struct timespec now{};
        ::clock_gettime(CLOCK_MONOTONIC, &now);
        return (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
    };

    char buf[65536];
    auto pump = [&](int fd, std::string& dst, bool& trunc) -> bool {
        ssize_t r = ::read(fd, buf, sizeof buf);
        if (r <= 0) {
            return false; // EOF or error: stream done
        }
        std::size_t got = static_cast<std::size_t>(r);
        if (dst.size() < spec.output_cap) {
            std::size_t room = spec.output_cap - dst.size();
            dst.append(buf, std::min(room, got));
            if (got > room) {
                trunc = true;
            }
        } else {
            trunc = true;
        }
        return true;
    };

    bool out_open = true;
    bool err_open = true;
    bool sync_open = true;
    char marker = 0; // 0 == exec succeeded; 'C'/'R'/'X' == chdir/ratchet/exec failure
    while (out_open || err_open || sync_open) {
        pollfd pfd[3];
        pfd[0] = {out_open ? out_fds[0] : -1, POLLIN, 0};
        pfd[1] = {err_open ? err_fds[0] : -1, POLLIN, 0};
        pfd[2] = {sync_open ? sync_fds[0] : -1, POLLIN, 0};

        int timeout = -1;
        if (spec.timeout_ms > 0 && !res.timed_out) {
            long rem = static_cast<long>(spec.timeout_ms) - elapsed_ms();
            timeout = rem > 0 ? static_cast<int>(rem) : 0;
        }
        int n = ::poll(pfd, static_cast<nfds_t>(3), timeout);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (n == 0 && spec.timeout_ms > 0 && !res.timed_out) {
            ::kill(-pid, SIGKILL);
            res.timed_out = true;
            continue; // loop again (timeout now -1) to drain to EOF after the kill
        }
        if (out_open && (pfd[0].revents & (POLLIN | POLLHUP)) != 0) {
            if (!pump(out_fds[0], res.out, res.out_truncated)) {
                ::close(out_fds[0]);
                out_open = false;
            }
        }
        if (err_open && (pfd[1].revents & (POLLIN | POLLHUP)) != 0) {
            if (!pump(err_fds[0], res.err, res.err_truncated)) {
                ::close(err_fds[0]);
                err_open = false;
            }
        }
        if (sync_open && (pfd[2].revents & (POLLIN | POLLHUP)) != 0) {
            char c = 0;
            ssize_t r = ::read(sync_fds[0], &c, 1);
            if (r == 1) {
                marker = c;
            } else { // EOF: clean exec, or error
                ::close(sync_fds[0]);
                sync_open = false;
            }
        }
    }
    if (out_open) {
        ::close(out_fds[0]);
    }
    if (err_open) {
        ::close(err_fds[0]);
    }
    if (sync_open) {
        ::close(sync_fds[0]);
    }

    int status = 0;
    ::waitpid(pid, &status, 0);
    res.spawned = (marker == 0);
    if (!res.spawned) {
        res.spawn_error = marker == 'C'   ? "task: cannot enter working directory"
                          : marker == 'R' ? "task: sandbox apply failed"
                                          : "task: exec failed";
    }
    if (WIFEXITED(status)) {
        res.code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        res.code = -WTERMSIG(status);
    }
    return res;
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

    const bool is_write = (op == "write" || op == "edit");
    Policy policy;
    if (is_write) {
        policy.write_paths = {base}; // create/write/replace confined to the base
    } else {
        policy.read_paths = {base}; // read-only confinement
    }
    if (!apply(policy)) {
        write_str(1, "sbx: sandbox apply failed");
        return 126;
    }
    if (op == "read") {
        long offset = 1, limit = 0; // default: whole file
        parse_read_range(extra, offset, limit);
        return op_read(base, rel, offset, limit);
    }
    if (op == "list") {
        return op_list(base, rel);
    }
    if (op == "find") {
        return op_find(base, rel, extra);
    }
    if (op == "write") {
        return op_write(base, rel, read_stdin_all());
    }
    if (op == "edit") {
        return op_edit(base, rel, extra, read_stdin_all());
    }
    write_str(1, "sbx: unknown op `" + op + "`");
    return 2;
}

} // namespace astrite::sandbox
