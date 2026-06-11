// Unified diff: the LCS-based hunked output behind diff.workspace.
#include <doctest.h>

#include <string>

#include "util/unified_diff.hpp"

using astrite::util::unified_diff;

TEST_CASE("unified_diff: identical content yields no diff") {
    CHECK(unified_diff("a\nb\nc", "a\nb\nc").empty());
    CHECK(unified_diff("", "").empty());
}

TEST_CASE("unified_diff: a single changed line shows -/+ with context") {
    std::string d = unified_diff("one\ntwo\nthree\n", "one\nTWO\nthree\n");
    CHECK(d.find("-two") != std::string::npos);
    CHECK(d.find("+TWO") != std::string::npos);
    CHECK(d.find(" one") != std::string::npos);   // context kept
    CHECK(d.find(" three") != std::string::npos);
    CHECK(d.find("@@") != std::string::npos);
}

TEST_CASE("unified_diff: pure additions and deletions") {
    std::string add = unified_diff("a\nb", "a\nb\nc");
    CHECK(add.find("+c") != std::string::npos);
    CHECK(add.find("\n-") == std::string::npos); // nothing removed

    std::string del = unified_diff("a\nb\nc", "a\nc");
    CHECK(del.find("-b") != std::string::npos);
    CHECK(del.find("\n+") == std::string::npos); // nothing added
}

TEST_CASE("unified_diff: a brand-new file is all additions") {
    std::string d = unified_diff("", "hello\nworld\n");
    CHECK(d.find("+hello") != std::string::npos);
    CHECK(d.find("+world") != std::string::npos);
    CHECK(d.find("\n-") == std::string::npos);
}

TEST_CASE("unified_diff: distant changes split into separate hunks") {
    std::string before;
    std::string after;
    for (int i = 0; i < 30; ++i) {
        before += "line" + std::to_string(i) + "\n";
        after += (i == 1 ? "CHANGED1\n" : (i == 25 ? "CHANGED25\n" : "line" + std::to_string(i) + "\n"));
    }
    std::string d = unified_diff(before, after);
    // two changes far apart -> two hunk headers
    std::size_t first = d.find("@@");
    std::size_t second = d.find("@@", first + 1);
    std::size_t third = d.find("@@", second + 1); // header has two @@; count hunks via lines
    (void)third;
    // count distinct hunk header lines
    int hunks = 0;
    for (std::size_t p = d.find("@@ -"); p != std::string::npos; p = d.find("@@ -", p + 1)) {
        ++hunks;
    }
    CHECK(hunks == 2);
    CHECK(d.find("CHANGED1") != std::string::npos);
    CHECK(d.find("CHANGED25") != std::string::npos);
    // unchanged middle (e.g. line15) is NOT in the output
    CHECK(d.find("line15") == std::string::npos);
}

TEST_CASE("unified_diff: oversized inputs degrade to a summary, not a dump") {
    std::string big;
    for (int i = 0; i < 50; ++i) {
        big += "x\n";
    }
    std::string d = unified_diff("a", big, 3, /*max_lines=*/10);
    CHECK(d.find("diff omitted") != std::string::npos);
}
