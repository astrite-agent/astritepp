#pragma once

#include <expected>
#include <string>
#include <string_view>

#include "schema/ir.hpp"

namespace astrite::schema {

struct LoadError {
    std::string message;
};

// Decode one JSONC document (JSON + // and /* */ comments) into a Component,
// fail-closed: unknown fields, missing required fields, wrong shapes, and
// zero-or-two node discriminators are all load errors. JSONC is the canonical
// component format: the smallest grammar at the trust boundary, handled by the
// same mature parser that reads untrusted model output. (M3 additionally moves
// parsing of untrusted component files into the confined loader child.)
[[nodiscard]] std::expected<Component, LoadError> load_component_jsonc(std::string_view jsonc);

} // namespace astrite::schema
