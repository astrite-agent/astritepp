#pragma once

#include <expected>

#include "kernel/blackboard.hpp"
#include "kernel/error.hpp"
#include "kernel/primitive.hpp"
#include "schema/ir.hpp"

namespace astrite::kernel {

// Walk one node: plain recursion, exhaustive visit, no async runtime. Errors
// propagate immediately; Success/Failure compose per node semantics (DESIGN §5).
[[nodiscard]] std::expected<Status, KernelError> walk(const schema::Node& node, Blackboard& bb,
                                                      const Context& ctx);

} // namespace astrite::kernel
