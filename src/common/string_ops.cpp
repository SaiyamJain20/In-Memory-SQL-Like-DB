#include "common/string_ops.h"

#include <algorithm>
#include <vector>

namespace cdb {

size_t Utf8CharLength(std::string_view s, size_t pos) noexcept {
    const auto lead = static_cast<unsigned char>(s[pos]);
    size_t len = 1;
    if (lead >= 0xF0 && lead <= 0xF7)
        len = 4;
    else if (lead >= 0xE0 && lead <= 0xEF)
        len = 3;
    else if (lead >= 0xC0 && lead <= 0xDF)
        len = 2;
    if (len > s.size() - pos)
        return 1; // truncated sequence
    for (size_t k = 1; k < len; k++) {
        if ((static_cast<unsigned char>(s[pos + k]) & 0xC0) != 0x80)
            return 1; // not a continuation byte
    }
    return len;
}

size_t Utf8Length(std::string_view s) noexcept {
    size_t chars = 0;
    for (size_t i = 0; i < s.size(); chars++) {
        i += Utf8CharLength(s, i);
    }
    return chars;
}

std::string_view SubstringView(std::string_view s, long long start,
                               std::optional<long long> length) noexcept {
    // Byte offset of the start of every character, plus the end offset as the last element.
    std::vector<size_t> offs;
    for (size_t i = 0; i < s.size();) {
        offs.push_back(i);
        i += Utf8CharLength(s, i);
    }
    offs.push_back(s.size());
    const long long n = static_cast<long long>(offs.size()) - 1; // characters
    // 0-based begin: start 1 -> 0; start 0 -> -1; negative start counts from the end
    long long begin = start > 0 ? start - 1 : (start == 0 ? -1 : n + start);
    long long end;
    if (!length) {
        end = n;
    } else if (*length >= 0) {
        end = begin + *length;
    } else { // negative length: that many characters *before* `begin`
        end = begin;
        begin = begin + *length;
    }
    begin = std::clamp<long long>(begin, 0, n);
    end = std::clamp<long long>(end, 0, n);
    if (begin >= end)
        return std::string_view();
    return s.substr(offs[static_cast<size_t>(begin)],
                    offs[static_cast<size_t>(end)] - offs[static_cast<size_t>(begin)]);
}

bool LikeMatch(std::string_view text, std::string_view pattern) noexcept {
    size_t t = 0, p = 0;
    size_t star_p = std::string_view::npos, star_t = 0;
    while (t < text.size()) {
        if (p < pattern.size() && pattern[p] == '%') {
            star_p = p++;
            star_t = t;
        } else if (p < pattern.size() && pattern[p] == '_') {
            t += Utf8CharLength(text, t);
            p++;
        } else if (p < pattern.size() && pattern[p] == text[t]) {
            p++;
            t++;
        } else if (star_p != std::string_view::npos) {
            // backtrack: let the last '%' swallow one more character
            star_t += Utf8CharLength(text, star_t);
            t = star_t;
            p = star_p + 1;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '%')
        p++;
    return p == pattern.size();
}

} // namespace cdb
