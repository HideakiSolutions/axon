#pragma once
#include <cstdint>
#include <string>

namespace axon {

// True when `s` is well-formed UTF-8 (no overlongs, surrogates or values above U+10FFFF).
inline bool is_valid_utf8(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        size_t extra = 0;
        uint32_t min = 0, cp = 0;
        if (c < 0x80) {
            ++i;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            extra = 1;
            min = 0x80;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
            min = 0x800;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
            min = 0x10000;
            cp = c & 0x07;
        } else {
            return false;
        }
        if (i + extra >= s.size()) return false;
        for (size_t k = 1; k <= extra; ++k) {
            const auto n = static_cast<unsigned char>(s[i + k]);
            if ((n & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (n & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += extra + 1;
    }
    return true;
}

// Returns valid UTF-8. Text that is already valid is returned unchanged; anything else is
// treated as Windows-1252 (a superset of Latin-1 and the usual encoding of legacy .NET/Delphi
// sources), so accented identifiers and comments survive instead of aborting the index.
inline std::string to_valid_utf8(const std::string& s) {
    if (is_valid_utf8(s)) return s;
    static const uint16_t cp1252_high[32] = {
        0x20AC, 0x003F, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
        0x2039, 0x0152, 0x003F, 0x017D, 0x003F, 0x003F, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
        0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x003F, 0x017E, 0x0178};
    std::string out;
    out.reserve(s.size() + s.size() / 8);
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        uint32_t cp = c;
        if (c >= 0x80 && c < 0xA0) cp = cp1252_high[c - 0x80];
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

} // namespace axon
