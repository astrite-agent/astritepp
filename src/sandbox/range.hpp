#pragma once

// Pure line-range slicing for fs.read. Factored out of the confined helper so it
// can be unit-tested without forking. 1-based line offset, line count limit.

#include <string>
#include <string_view>

namespace astrite::sandbox {

// Return the [offset, offset + limit) line slice of `content`. `offset` is
// 1-based and clamped up to 1; `limit <= 0` means "to end of content". The
// result is byte-faithful to the original region — each line keeps its trailing
// newline exactly as in `content` — so slice_lines(c, 1, 0) == c and the slice
// of a final line without a newline has no newline appended.
[[nodiscard]] inline std::string slice_lines(std::string_view content, long offset, long limit) {
    if (offset < 1) {
        offset = 1;
    }
    std::string out;
    long line_no = 0;
    long emitted = 0;
    std::size_t pos = 0;
    const std::size_t n = content.size();
    while (pos < n) {
        const std::size_t nl = content.find('\n', pos);
        const std::size_t end = (nl == std::string_view::npos) ? n : nl + 1;
        ++line_no;
        if (line_no >= offset) {
            if (limit > 0 && emitted >= limit) {
                break;
            }
            out.append(content.substr(pos, end - pos));
            ++emitted;
        }
        pos = end;
    }
    return out;
}

} // namespace astrite::sandbox
