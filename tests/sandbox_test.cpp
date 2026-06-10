// Sandbox enforcement tests. Linux-gated: the floor is a Linux guarantee, and
// these prove it by forking a child, applying the ratchet, and observing that
// real syscalls are actually denied — belt-and-suspenders ground truth, not a
// mock. They are also the proof the floor fails *closed*.
#include <doctest.h>

#ifdef __linux__

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "sandbox/sandbox.hpp"
#include "sandbox/seccomp.hpp"

namespace fs = std::filesystem;
using namespace astrite;

namespace {

fs::path make_dir(std::string suffix) {
    static int counter = 0;
    fs::path p = fs::temp_directory_path() /
                 ("astrite_sbx_" + std::to_string(++counter) + "_" + suffix);
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

void write_file(const fs::path& p, std::string_view content) {
    std::ofstream out(p, std::ios::binary);
    out << content;
}

int probe_read(fs::path path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return 1;
    }
    ::close(fd);
    return 0; // 0 == readable
}

int probe_create(fs::path path) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return 1;
    }
    ::close(fd);
    return 0; // 0 == writable
}

std::string true_binary() {
    for (const char* candidate : {"/usr/bin/true", "/bin/true"}) {
        if (fs::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

} // namespace

TEST_CASE("the inet-deny filter has a sane fixed shape") {
    CHECK(sandbox::inet_deny_program.size() == 10);
    CHECK(sandbox::landlock_abi() > 0); // this kernel supports Landlock
}

TEST_CASE("a benign child runs to completion under the ratchet") {
    sandbox::Policy policy;
    CHECK(sandbox::run_confined(policy, [] { return ::getpid() > 0 ? 0 : 1; }) == 0);
}

TEST_CASE("filesystem reads are confined to granted bases") {
    fs::path granted = make_dir("granted");
    fs::path forbidden = make_dir("forbidden");
    write_file(granted / "ok.txt", "hi");
    write_file(forbidden / "secret.txt", "nope");

    sandbox::Policy policy;
    policy.read_paths = {granted};

    CHECK(sandbox::run_confined(policy, [p = granted / "ok.txt"] { return probe_read(p); }) == 0);
    CHECK(sandbox::run_confined(policy,
                               [p = forbidden / "secret.txt"] { return probe_read(p); }) != 0);

    fs::remove_all(granted);
    fs::remove_all(forbidden);
}

TEST_CASE("writes require a write base; read-only bases reject creation") {
    fs::path ro = make_dir("ro");
    fs::path rw = make_dir("rw");

    sandbox::Policy policy;
    policy.read_paths = {ro};
    policy.write_paths = {rw};

    CHECK(sandbox::run_confined(policy, [p = rw / "new.txt"] { return probe_create(p); }) == 0);
    CHECK(sandbox::run_confined(policy, [p = ro / "new.txt"] { return probe_create(p); }) != 0);
    // A write base is also readable.
    write_file(rw / "seed.txt", "x");
    CHECK(sandbox::run_confined(policy, [p = rw / "seed.txt"] { return probe_read(p); }) == 0);

    fs::remove_all(ro);
    fs::remove_all(rw);
}

TEST_CASE("inet sockets are denied; AF_UNIX is allowed") {
    sandbox::Policy policy;
    CHECK(sandbox::run_confined(policy, [] {
              int fd = ::socket(AF_INET, SOCK_STREAM, 0);
              if (fd >= 0) {
                  ::close(fd);
                  return 1; // should not have been creatable
              }
              return 0;
          }) == 0);
    CHECK(sandbox::run_confined(policy, [] {
              int fd = ::socket(AF_INET6, SOCK_STREAM, 0);
              if (fd >= 0) {
                  ::close(fd);
                  return 1;
              }
              return 0;
          }) == 0);
    CHECK(sandbox::run_confined(policy, [] {
              int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
              if (fd < 0) {
                  return 1; // local sockets are not the floor's concern
              }
              ::close(fd);
              return 0;
          }) == 0);
}

TEST_CASE("a real dynamically-linked tool still runs under the policy") {
    std::string tru = true_binary();
    REQUIRE_FALSE(tru.empty());
    sandbox::Policy policy;
    policy.read_paths = sandbox::system_read_exec_paths(); // toolchain + libs readable/execable

    int status = sandbox::run_confined(policy, [tru] {
        char* const argv[] = {const_cast<char*>(tru.c_str()), nullptr};
        char* const envp[] = {nullptr};
        ::execve(tru.c_str(), argv, envp);
        return 127; // exec failed → libs/loader not reachable under policy
    });
    CHECK(status == 0); // `true` ran and exited 0 inside the sandbox
}

TEST_CASE("the floor fails closed: an unopenable base is simply not granted") {
    sandbox::Policy policy;
    policy.read_paths = {"/nonexistent/path/xyzzy"}; // skipped, not fatal
    // The ratchet still applies and confines everything else.
    CHECK(sandbox::run_confined(policy, [] {
              return ::open("/etc/hostname", O_RDONLY) >= 0 ? 1 : 0; // not granted → denied
          }) == 0);
}

#endif // __linux__
