#pragma once

// A small, dependency-free unified diff (LCS edit script + hunked output) for
// diff.workspace — letting the agent inspect and explain the changes it made.
// Bounded: large files degrade to a terse summary rather than dumping. Not a hot
// path; O(n*m) LCS is fine at the sizes (and cap) involved.

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace astrite::util {

namespace detail {

inline std::vector<std::string> split_lines(std::string_view s) {
    std::vector<std::string> lines;
    std::size_t pos = 0;
    while (pos < s.size()) {
        std::size_t nl = s.find('\n', pos);
        if (nl == std::string_view::npos) {
            lines.emplace_back(s.substr(pos));
            break;
        }
        lines.emplace_back(s.substr(pos, nl - pos));
        pos = nl + 1;
    }
    return lines;
}

} // namespace detail

// Unified diff of `before` vs `after` with `context` common lines around each
// change hunk. Empty result means "no change". Files exceeding `max_lines`
// degrade to a one-line summary.
[[nodiscard]] inline std::string unified_diff(std::string_view before, std::string_view after,
                                              int context = 3, std::size_t max_lines = 4000) {
    const std::vector<std::string> a = detail::split_lines(before);
    const std::vector<std::string> b = detail::split_lines(after);
    if (a == b) {
        return std::string{};
    }
    if (a.size() > max_lines || b.size() > max_lines) {
        return "(diff omitted: file too large — " + std::to_string(a.size()) + " → " +
               std::to_string(b.size()) + " lines)\n";
    }
    const std::size_t n = a.size();
    const std::size_t m = b.size();

    // LCS lengths: dp[i][j] = LCS(a[i:], b[j:]).
    std::vector<std::vector<int>> dp(n + 1, std::vector<int>(m + 1, 0));
    for (std::size_t i = n; i-- > 0;) {
        for (std::size_t j = m; j-- > 0;) {
            dp[i][j] = a[i] == b[j] ? dp[i + 1][j + 1] + 1 : std::max(dp[i + 1][j], dp[i][j + 1]);
        }
    }

    struct E {
        char tag;            // ' ' common, '-' removed, '+' added
        const std::string* text;
        std::size_t a_no;    // 1-based source line (for ' ' and '-')
        std::size_t b_no;    // 1-based source line (for ' ' and '+')
    };
    std::vector<E> script;
    std::size_t i = 0, j = 0;
    while (i < n && j < m) {
        if (a[i] == b[j]) {
            script.push_back({' ', &a[i], i + 1, j + 1});
            ++i;
            ++j;
        } else if (dp[i + 1][j] >= dp[i][j + 1]) {
            script.push_back({'-', &a[i], i + 1, 0});
            ++i;
        } else {
            script.push_back({'+', &b[j], 0, j + 1});
            ++j;
        }
    }
    while (i < n) {
        script.push_back({'-', &a[i], i + 1, 0});
        ++i;
    }
    while (j < m) {
        script.push_back({'+', &b[j], 0, j + 1});
        ++j;
    }

    // Mark entries within `context` positions of any change; group runs into hunks.
    const std::size_t sz = script.size();
    std::vector<bool> keep(sz, false);
    for (std::size_t k = 0; k < sz; ++k) {
        if (script[k].tag == ' ') {
            continue;
        }
        const auto ctx = static_cast<std::size_t>(context);
        const std::size_t lo = k >= ctx ? k - ctx : 0;
        const std::size_t hi = std::min(sz, k + ctx + 1);
        for (std::size_t t = lo; t < hi; ++t) {
            keep[t] = true;
        }
    }

    std::string out;
    for (std::size_t k = 0; k < sz;) {
        if (!keep[k]) {
            ++k;
            continue;
        }
        std::size_t end = k;
        while (end < sz && keep[end]) {
            ++end;
        }
        // hunk = [k, end); compute @@ header line spans.
        std::size_t a_start = 0, a_count = 0, b_start = 0, b_count = 0;
        for (std::size_t t = k; t < end; ++t) {
            if (script[t].tag != '+') {
                if (a_count == 0) {
                    a_start = script[t].a_no;
                }
                ++a_count;
            }
            if (script[t].tag != '-') {
                if (b_count == 0) {
                    b_start = script[t].b_no;
                }
                ++b_count;
            }
        }
        out += "@@ -" + std::to_string(a_count ? a_start : 0) + "," + std::to_string(a_count) +
               " +" + std::to_string(b_count ? b_start : 0) + "," + std::to_string(b_count) + " @@\n";
        for (std::size_t t = k; t < end; ++t) {
            out += script[t].tag;
            out += *script[t].text;
            out += '\n';
        }
        k = end;
    }
    return out;
}

} // namespace astrite::util
