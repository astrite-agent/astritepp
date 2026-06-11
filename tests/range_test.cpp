// Pure line-range slicing for fs.read (range reads). These nail the slice
// semantics without forking; fs_helper_test exercises the real confined path.
#include <doctest.h>

#include <string>

#include "sandbox/range.hpp"

using astrite::sandbox::slice_lines;

TEST_CASE("slice_lines: the identity slice returns the whole content byte-for-byte") {
    CHECK(slice_lines("a\nb\nc", 1, 0) == "a\nb\nc");
    CHECK(slice_lines("a\nb\nc\n", 1, 0) == "a\nb\nc\n");
    CHECK(slice_lines("", 1, 0).empty());
    CHECK(slice_lines("solo", 1, 0) == "solo");
}

TEST_CASE("slice_lines: a window keeps each line's terminator") {
    CHECK(slice_lines("a\nb\nc\nd", 2, 2) == "b\nc\n");
    CHECK(slice_lines("a\nb\nc\nd", 1, 1) == "a\n");
    CHECK(slice_lines("a\nb\nc\nd", 4, 1) == "d"); // last line, no trailing newline
    CHECK(slice_lines("a\nb\nc\nd\n", 4, 1) == "d\n"); // last line, newline preserved
}

TEST_CASE("slice_lines: limit <= 0 reads from offset to the end") {
    CHECK(slice_lines("a\nb\nc\nd", 3, 0) == "c\nd");
    CHECK(slice_lines("a\nb\nc\nd\n", 3, 0) == "c\nd\n");
}

TEST_CASE("slice_lines: offset is clamped up to 1") {
    CHECK(slice_lines("a\nb\nc", 0, 1) == "a\n");
    CHECK(slice_lines("a\nb\nc", -5, 2) == "a\nb\n");
}

TEST_CASE("slice_lines: an offset past the end yields empty, never an error") {
    CHECK(slice_lines("a\nb\nc", 99, 0).empty());
    CHECK(slice_lines("a\nb\nc", 4, 10).empty()); // 3 lines, ask for line 4
}

TEST_CASE("slice_lines: a limit larger than the remainder stops at the end") {
    CHECK(slice_lines("a\nb\nc", 2, 100) == "b\nc");
}
