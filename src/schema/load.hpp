#pragma once

#include <expected>
#include <string>
#include <string_view>

#include "schema/ir.hpp"

namespace astrite::schema {

struct LoadError {
    std::string message;
};

// Decode one YAML document into a Component, fail-closed: unknown fields, duplicate
// keys, wrong shapes, and zero-or-two node discriminators are all load errors.
// NOTE (sprint): this currently parses in-process; M3 moves the parse into the
// confined loader child (DESIGN §7).
[[nodiscard]] std::expected<Component, LoadError> load_component_yaml(std::string_view yaml);

} // namespace astrite::schema
