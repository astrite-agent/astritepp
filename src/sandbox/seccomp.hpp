#pragma once

// The seccomp leg of the floor: a classic-BPF program, assembled and bounds-
// checked at COMPILE TIME as a constexpr array, that denies AF_INET/AF_INET6
// socket creation with EPERM. v0 is deny-inet with default-allow (DESIGN §7);
// a full default-deny allowlist is the eventual target. Network for sandboxed
// children is therefore off at the syscall level, beneath Landlock's net leg.

#include <array>
#include <cstddef>
#include <cstdint>

#include <linux/audit.h>
#include <linux/bpf_common.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/socket.h>
#include <sys/syscall.h>

#include <cerrno>

namespace astrite::sandbox {

namespace detail {

constexpr sock_filter stmt(std::uint16_t code, std::uint32_t k) {
    return sock_filter{code, 0, 0, k};
}
constexpr sock_filter jeq(std::uint32_t k, std::uint8_t jt, std::uint8_t jf) {
    return sock_filter{BPF_JMP | BPF_JEQ | BPF_K, jt, jf, k};
}

#if defined(__aarch64__)
constexpr std::uint32_t kAuditArch = AUDIT_ARCH_AARCH64;
#elif defined(__x86_64__)
constexpr std::uint32_t kAuditArch = AUDIT_ARCH_X86_64;
#else
#error "astrite sandbox: unsupported architecture for the seccomp arch guard"
#endif

// seccomp_data field offsets (little-endian: arg low word at the struct offset).
constexpr std::uint32_t kOffArch = offsetof(seccomp_data, arch);
constexpr std::uint32_t kOffNr = offsetof(seccomp_data, nr);
constexpr std::uint32_t kOffArg0 = offsetof(seccomp_data, args);

constexpr std::uint32_t kRetDeny = SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA);

} // namespace detail

// Indices kept explicit so the jump offsets are auditable; see the trailing
// static_assert. Layout:
//   0  load arch
//   1  arch == ours ? -> 3 : -> 2            (wrong arch fails closed)
//   2  RET KILL_PROCESS
//   3  load nr
//   4  nr == socket ? -> 5 : -> 9 (ALLOW)
//   5  load args[0] (domain, low word)
//   6  domain == AF_INET  ? -> 8 (DENY) : -> 7
//   7  domain == AF_INET6 ? -> 8 (DENY) : -> 9 (ALLOW)
//   8  RET ERRNO(EPERM)
//   9  RET ALLOW
inline constexpr std::array<sock_filter, 10> inet_deny_program = {{
    detail::stmt(BPF_LD | BPF_W | BPF_ABS, detail::kOffArch),
    detail::jeq(detail::kAuditArch, 1, 0),
    detail::stmt(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
    detail::stmt(BPF_LD | BPF_W | BPF_ABS, detail::kOffNr),
    detail::jeq(__NR_socket, 0, 4),
    detail::stmt(BPF_LD | BPF_W | BPF_ABS, detail::kOffArg0),
    detail::jeq(AF_INET, 1, 0),
    detail::jeq(AF_INET6, 0, 1),
    detail::stmt(BPF_RET | BPF_K, detail::kRetDeny),
    detail::stmt(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
}};

static_assert(inet_deny_program.size() <= 0xffff, "BPF program length must fit sock_fprog.len");

// Install the filter on the calling process (must be preceded by no_new_privs).
// Returns false on failure — the caller fails closed.
[[nodiscard]] bool install_inet_deny();

} // namespace astrite::sandbox
