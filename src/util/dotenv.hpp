#pragma once

// Minimal .env loader: read KEY=VALUE lines into the process environment so the
// binaries pick up provider keys placed in a local .env, WITHOUT overriding
// anything already set in the real environment (the shell always wins). Blank
// lines and '#' comments are skipped; surrounding quotes on the value are
// stripped. Not a general dotenv parser — just enough for local dev keys.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace astrite::util {

inline void load_dotenv(const std::filesystem::path& path = ".env") {
    std::ifstream in(path);
    if (!in) {
        return;
    }
    std::string line;
    while (std::getline(in, line)) {
        std::size_t begin = line.find_first_not_of(" \t");
        if (begin == std::string::npos || line[begin] == '#') {
            continue;
        }
        std::size_t eq = line.find('=', begin);
        if (eq == std::string::npos) {
            continue;
        }
        std::string key = line.substr(begin, eq - begin);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) {
            key.pop_back();
        }
        if (key.empty()) {
            continue;
        }
        std::string val = line.substr(eq + 1);
        std::size_t vbegin = val.find_first_not_of(" \t");
        val = vbegin == std::string::npos ? std::string{} : val.substr(vbegin);
        while (!val.empty() && (val.back() == '\r' || val.back() == ' ' || val.back() == '\t')) {
            val.pop_back();
        }
        if (val.size() >= 2 && (val.front() == '"' || val.front() == '\'') &&
            val.back() == val.front()) {
            val = val.substr(1, val.size() - 2);
        }
        ::setenv(key.c_str(), val.c_str(), /*overwrite=*/0); // real environment wins
    }
}

} // namespace astrite::util
