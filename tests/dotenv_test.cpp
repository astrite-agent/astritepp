// .env loading: fills unset variables, never overrides the real environment.
#include <doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "util/dotenv.hpp"

namespace fs = std::filesystem;

namespace {
fs::path tmp_env(std::string body) {
    static int n = 0;
    fs::path p = fs::temp_directory_path() / ("astrite_env_" + std::to_string(++n));
    std::ofstream(p) << body;
    return p;
}
std::string get(const char* k) {
    const char* v = std::getenv(k);
    return v != nullptr ? v : std::string{};
}
} // namespace

TEST_CASE("load_dotenv: sets unset keys, strips quotes/comments/blank lines") {
    ::unsetenv("ASTRITE_T_A");
    ::unsetenv("ASTRITE_T_B");
    fs::path p = tmp_env("# a comment\n\nASTRITE_T_A=hello\nASTRITE_T_B=\"quoted val\"\n");
    astrite::util::load_dotenv(p);
    CHECK(get("ASTRITE_T_A") == "hello");
    CHECK(get("ASTRITE_T_B") == "quoted val");
    fs::remove(p);
}

TEST_CASE("load_dotenv: the real environment wins over the file") {
    ::setenv("ASTRITE_T_C", "from-env", /*overwrite=*/1);
    fs::path p = tmp_env("ASTRITE_T_C=from-file\n");
    astrite::util::load_dotenv(p);
    CHECK(get("ASTRITE_T_C") == "from-env");
    fs::remove(p);
    ::unsetenv("ASTRITE_T_C");
}

TEST_CASE("load_dotenv: a missing file is a no-op") {
    astrite::util::load_dotenv(fs::temp_directory_path() / "astrite_no_such_env_xyz");
    CHECK(true); // did not throw / crash
}
