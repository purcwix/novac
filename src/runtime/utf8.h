#pragma once
// utf8.h — lightweight UTF-8 aware string utilities for Novac
// Storage stays as std::string (UTF-8 bytes); these helpers make
// operations codepoint-aware instead of byte-aware.
#include <string>
#include <vector>
#include <cstdint>
#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
#endif
#include <algorithm>

namespace utf8 {

// ── how many bytes does this leading byte introduce? ──────────────────────────
inline int seqLen(unsigned char c) {
    if (c < 0x80)          return 1; // ASCII
    if ((c & 0xE0) == 0xC0) return 2; // 2-byte sequence
    if ((c & 0xF0) == 0xE0) return 3; // 3-byte sequence
    if ((c & 0xF8) == 0xF0) return 4; // 4-byte sequence
    return 1; // continuation byte / invalid — treat as 1 so we advance
}

// ── codepoint count (NOT byte count) ─────────────────────────────────────────
inline size_t length(const std::string &s) {
    size_t count = 0, i = 0;
    while (i < s.size()) {
        i += seqLen((unsigned char)s[i]);
        count++;
    }
    return count;
}

// ── split into individual codepoint strings ───────────────────────────────────
inline std::vector<std::string> codepoints(const std::string &s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        int len = seqLen((unsigned char)s[i]);
        len = std::min(len, (int)(s.size() - i));
        out.push_back(s.substr(i, len));
        i += len;
    }
    return out;
}

// ── byte offset of the nth codepoint (-1 if out of range) ────────────────────
inline int byteOffset(const std::string &s, int n) {
    int i = 0, cur = 0;
    int total = (int)s.size();
    if (n < 0) n = (int)length(s) + n;
    while (i < total) {
        if (cur == n) return i;
        i += seqLen((unsigned char)s[i]);
        cur++;
    }
    return -1; // past end
}

// ── get the nth codepoint as a string ("" if out of range) ───────────────────
inline std::string at(const std::string &s, int n) {
    int total = (int)length(s);
    if (n < 0) n = total + n;
    if (n < 0 || n >= total) return "";
    int off = byteOffset(s, n);
    if (off < 0) return "";
    int len = seqLen((unsigned char)s[off]);
    len = std::min(len, (int)s.size() - off);
    return s.substr(off, len);
}

// ── substring by codepoint indices [start, end) ──────────────────────────────
inline std::string substr(const std::string &s, int start, int end) {
    int total = (int)length(s);
    if (start < 0) start = std::max(0, total + start);
    if (end   < 0) end   = std::max(0, total + end);
    start = std::min(start, total);
    end   = std::min(end,   total);
    if (start >= end) return "";

    int byteStart = byteOffset(s, start);
    if (byteStart < 0) return "";

    // walk from byteStart to find byteEnd
    int i = byteStart, cur = start;
    while (i < (int)s.size() && cur < end) {
        i += seqLen((unsigned char)s[i]);
        cur++;
    }
    return s.substr(byteStart, i - byteStart);
}

// ── codepoint-aware indexOf: returns codepoint index, not byte index ──────────
// (returns -1 if not found)
// Note: find() on raw bytes is fine for UTF-8 because valid multibyte
// sequences never overlap with ASCII or each other — so byte-level find
// gives the right byte position; we just convert it to a codepoint index.
inline int indexOf(const std::string &haystack, const std::string &needle) {
    if (needle.empty()) return 0;
    auto bytePos = haystack.find(needle);
    if (bytePos == std::string::npos) return -1;
    // count codepoints before bytePos
    int count = 0;
    size_t i = 0;
    while (i < bytePos) {
        i += seqLen((unsigned char)haystack[i]);
        count++;
    }
    return count;
}

inline int lastIndexOf(const std::string &haystack, const std::string &needle) {
    if (needle.empty()) return (int)length(haystack);
    auto bytePos = haystack.rfind(needle);
    if (bytePos == std::string::npos) return -1;
    int count = 0;
    size_t i = 0;
    while (i < bytePos) {
        i += seqLen((unsigned char)haystack[i]);
        count++;
    }
    return count;
}

// ── codepoint-aware padStart / padEnd ────────────────────────────────────────
inline std::string padStart(const std::string &s, int width, const std::string &pad) {
    int len = (int)length(s);
    if (len >= width) return s;
    std::string out;
    int needed = width - len;
    auto pads = codepoints(pad.empty() ? " " : pad);
    for (int i = 0; i < needed; i++) out += pads[i % pads.size()];
    out += s;
    return out;
}

inline std::string padEnd(const std::string &s, int width, const std::string &pad) {
    int len = (int)length(s);
    if (len >= width) return s;
    std::string out = s;
    int needed = width - len;
    auto pads = codepoints(pad.empty() ? " " : pad);
    for (int i = 0; i < needed; i++) out += pads[i % pads.size()];
    return out;
}

} // namespace utf8