// SHA-256 against the standard FIPS 180-4 / NIST test vectors.
#include <doctest.h>

#include <string>

#include "util/sha256.hpp"

using astrite::util::sha256_hex;

TEST_CASE("sha256: the canonical short vectors") {
    CHECK(sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("sha256: multi-block messages match the reference") {
    CHECK(sha256_hex("The quick brown fox jumps over the lazy dog") ==
          "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592");
    CHECK(sha256_hex(std::string(1000, 'a')) ==
          "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
}

TEST_CASE("sha256: distinct inputs hash differently; identical inputs match") {
    CHECK(sha256_hex("component-v1") == sha256_hex(std::string("component-v1")));
    CHECK(sha256_hex("component-v1") != sha256_hex("component-v2"));
    CHECK(sha256_hex("a").size() == 64);
}
