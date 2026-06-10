#pragma once

#include <memory>
#include <utility>

namespace astrite {

// Value-semantic heap indirection for recursive types: deep copy, deep compare.
// Becomes std::indirect<T> (P3019) when the toolchain ships it. A moved-from box
// may only be assigned to or destroyed.
template <class T>
class box {
public:
    box(T value) : p_(std::make_unique<T>(std::move(value))) {}
    box(const box& other) : p_(std::make_unique<T>(*other.p_)) {}
    box(box&&) noexcept = default;
    box& operator=(const box& other) {
        if (this != &other) {
            p_ = std::make_unique<T>(*other.p_);
        }
        return *this;
    }
    box& operator=(box&&) noexcept = default;

    [[nodiscard]] T& operator*() { return *p_; }
    [[nodiscard]] const T& operator*() const { return *p_; }
    [[nodiscard]] T* operator->() { return p_.get(); }
    [[nodiscard]] const T* operator->() const { return p_.get(); }

    friend bool operator==(const box& a, const box& b) { return *a == *b; }

private:
    std::unique_ptr<T> p_;
};

} // namespace astrite
