#pragma once

#include <string>

namespace astrite::kernel {

// One kernel-layer error type, carried in std::expected throughout the evaluator.
// Walks never throw and never produce silent nulls.
struct KernelError {
    std::string message;
};

} // namespace astrite::kernel
