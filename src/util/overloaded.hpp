#pragma once

namespace astrite {

// The classic visit helper. An overload set with no catch-all is exhaustive at
// compile time: adding a variant alternative breaks every visit site that
// doesn't handle it.
template <class... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};

} // namespace astrite
