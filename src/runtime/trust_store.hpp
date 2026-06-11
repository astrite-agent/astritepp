#pragma once

// Trust records: the hash-pinned approvals that gate disk-sourced components
// (overrides + repo-local additions). A record says "a human approved THIS exact
// source (by sha256) with THESE capabilities." At load, a disk component whose
// source doesn't match an approved record is untrusted — new or changed. The
// kernel writes records only through the consent flow (B4); nothing model-driven
// can reach .astrite/ (control-plane reservation), so a record means a human
// said yes. Embedded defaults are compiled in and implicitly trusted — they
// never need a record.

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <glaze/glaze.hpp>

#include "util/sha256.hpp"

namespace astrite::runtime {

struct TrustRecord {
    std::string name;
    std::string sha256;            // pin: sha256 of the approved component source
    std::vector<std::string> caps; // declared caps at approval (for the audit trail)
    std::string risk;              // risk tier at approval
    std::string approved_at;       // ISO-8601 stamp, set by the caller
};

enum class TrustState { trusted, untrusted_new, untrusted_changed };
enum class TrustPolicy { warn, strict };

// Pure: does this source match its approval record?
[[nodiscard]] inline TrustState evaluate_trust(std::string_view source,
                                               const std::optional<TrustRecord>& rec) {
    if (!rec.has_value()) {
        return TrustState::untrusted_new;
    }
    return rec->sha256 == util::sha256_hex(source) ? TrustState::trusted
                                                   : TrustState::untrusted_changed;
}

// Pure: under the given policy, may an untrusted source load? `warn` loads it
// (and the caller announces it); `strict` refuses everything unapproved.
[[nodiscard]] inline bool should_load(TrustState state, TrustPolicy policy) {
    return state == TrustState::trusted || policy == TrustPolicy::warn;
}

[[nodiscard]] inline TrustRecord make_trust_record(std::string name, std::string_view source,
                                                   std::vector<std::string> caps, std::string risk,
                                                   std::string approved_at) {
    return TrustRecord{std::move(name), util::sha256_hex(source), std::move(caps), std::move(risk),
                       std::move(approved_at)};
}

[[nodiscard]] inline std::optional<TrustRecord>
read_trust_record(const std::filesystem::path& trust_dir, const std::string& name) {
    std::ifstream in(trust_dir / (name + ".json"), std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::string buf{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    TrustRecord rec;
    if (glz::read_json(rec, buf)) {
        return std::nullopt; // a corrupt record is treated as no record (fail closed)
    }
    return rec;
}

[[nodiscard]] inline bool write_trust_record(const std::filesystem::path& trust_dir,
                                             const TrustRecord& rec) {
    std::error_code ec;
    std::filesystem::create_directories(trust_dir, ec);
    std::string out;
    if (glz::write_json(rec, out)) {
        return false;
    }
    std::ofstream o(trust_dir / (rec.name + ".json"), std::ios::binary | std::ios::trunc);
    if (!o) {
        return false;
    }
    o << out;
    return o.good();
}

} // namespace astrite::runtime
