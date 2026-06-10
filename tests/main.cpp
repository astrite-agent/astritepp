#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest.h>

#include "sandbox/sandbox.hpp"

// The test binary doubles as the sandbox helper: run_fs_op self-re-execs
// /proc/self/exe (= this binary) with the hidden __sbx subcommand, so the fs
// helper tests exercise the real delivery path rather than a stand-in.
int main(int argc, char** argv) {
    if (int rc = astrite::sandbox::maybe_run_helper(argc, argv); rc >= 0) {
        return rc;
    }
    return doctest::Context(argc, argv).run();
}
