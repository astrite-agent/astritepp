#pragma once

// The authority fold: a pure tree-walk computing the maximal reachable primitive
// authority of a component — through composites and iteration bodies — before a
// single effect fires. A component whose reachable capabilities exceed its
// declared `caps` is rejected; an unknown primitive fails the fold closed.
// `dispatch` is a routing capability of its own: the router needs `dispatch`,
// and the selected tool component's caps are checked at dispatch time.

#include <algorithm>
#include <expected>
#include <set>
#include <string>
#include <vector>

#include "kernel/error.hpp"
#include "kernel/primitive.hpp"
#include "schema/ir.hpp"
#include "util/overloaded.hpp"

namespace astrite::kernel {

struct AuthorityTrace {
    std::vector<std::string> reached; // sorted capability ids
};

namespace detail {

inline std::expected<void, KernelError> collect_caps(const schema::Node& node,
                                                     const Registry& reg,
                                                     std::set<std::string>& reached) {
    auto leaf = [&](const std::string& primitive) -> std::expected<void, KernelError> {
        if (primitive == "dispatch") {
            reached.insert("dispatch");
            return {};
        }
        if (!reg.has(primitive)) {
            return std::unexpected(
                KernelError{"authority: unknown primitive `" + primitive + "`"});
        }
        if (const std::string* cap = reg.cap_of(primitive)) {
            reached.insert(*cap);
        }
        return {};
    };
    auto children = [&](const std::vector<schema::Node>& nodes)
        -> std::expected<void, KernelError> {
        for (const schema::Node& child : nodes) {
            auto r = collect_caps(child, reg, reached);
            if (!r.has_value()) {
                return r;
            }
        }
        return {};
    };
    return std::visit(
        overloaded{
            [&](const schema::All& n) { return children(n.children); },
            [&](const schema::Any& n) { return children(n.children); },
            [&](const schema::Action& n) { return leaf(n.primitive); },
            [&](const schema::Condition& n) { return leaf(n.predicate); },
            [&](const schema::Foreach& n) { return collect_caps(*n.body, reg, reached); },
            [&](const schema::Repeat& n) -> std::expected<void, KernelError> {
                auto body = collect_caps(*n.body, reg, reached);
                if (!body.has_value()) {
                    return body;
                }
                return collect_caps(*n.until, reg, reached);
            },
        },
        node.base());
}

} // namespace detail

[[nodiscard]] inline std::expected<AuthorityTrace, KernelError>
check_component_authority(const schema::Component& component, const Registry& reg) {
    std::set<std::string> reached;
    auto walk_result = detail::collect_caps(component.tree, reg, reached);
    if (!walk_result.has_value()) {
        return std::unexpected(walk_result.error());
    }
    std::vector<std::string> missing;
    for (const std::string& cap : reached) {
        if (std::find(component.caps.begin(), component.caps.end(), cap) ==
            component.caps.end()) {
            missing.push_back(cap);
        }
    }
    if (!missing.empty()) {
        std::string list;
        for (const std::string& cap : missing) {
            list += (list.empty() ? "" : ", ") + cap;
        }
        return std::unexpected(KernelError{"authority: component `" + component.name +
                                           "` reaches undeclared capabilities: " + list});
    }
    return AuthorityTrace{{reached.begin(), reached.end()}};
}

} // namespace astrite::kernel
