#include "ovicode/utf8.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>

namespace ovicode::utf8 {

namespace {

struct Rune {
    char32_t value = 0xFFFD;
    std::size_t bytes = 1;
};

Rune decode(std::string_view text, std::size_t offset) {
    if (offset >= text.size()) return {0, 0};
    const auto first = static_cast<unsigned char>(text[offset]);
    if (first < 0x80) return {first, 1};

    int count = 0;
    char32_t value = 0;
    if ((first & 0xE0) == 0xC0) {
        count = 2;
        value = first & 0x1F;
    } else if ((first & 0xF0) == 0xE0) {
        count = 3;
        value = first & 0x0F;
    } else if ((first & 0xF8) == 0xF0) {
        count = 4;
        value = first & 0x07;
    } else {
        return {};
    }
    if (offset + static_cast<std::size_t>(count) > text.size()) return {};
    for (int i = 1; i < count; ++i) {
        const auto next = static_cast<unsigned char>(text[offset + i]);
        if ((next & 0xC0) != 0x80) return {};
        value = (value << 6) | (next & 0x3F);
    }
    if ((count == 2 && value < 0x80) || (count == 3 && value < 0x800) ||
        (count == 4 && value < 0x10000) || value > 0x10FFFF ||
        (value >= 0xD800 && value <= 0xDFFF)) {
        return {};
    }
    return {value, static_cast<std::size_t>(count)};
}

bool in(char32_t value, char32_t first, char32_t last) {
    return value >= first && value <= last;
}

bool isCombining(char32_t value) {
    return in(value, 0x0300, 0x036F) || in(value, 0x1AB0, 0x1AFF) ||
           in(value, 0x1DC0, 0x1DFF) || in(value, 0x20D0, 0x20FF) ||
           in(value, 0xFE20, 0xFE2F) || in(value, 0xFE00, 0xFE0F) ||
           in(value, 0xE0100, 0xE01EF) || in(value, 0x1F3FB, 0x1F3FF) ||
           value == 0x200C;
}

bool isRegionalIndicator(char32_t value) {
    return in(value, 0x1F1E6, 0x1F1FF);
}

bool isWide(char32_t value) {
    return value >= 0x1100 &&
           (value <= 0x115F || value == 0x2329 || value == 0x232A ||
            in(value, 0x2E80, 0xA4CF) || in(value, 0xAC00, 0xD7A3) ||
            in(value, 0xF900, 0xFAFF) || in(value, 0xFE10, 0xFE19) ||
            in(value, 0xFE30, 0xFE6F) || in(value, 0xFF00, 0xFF60) ||
            in(value, 0xFFE0, 0xFFE6) || in(value, 0x1F300, 0x1FAFF) ||
            in(value, 0x20000, 0x3FFFD));
}

int runeWidth(char32_t value) {
    if (value == 0 || value == 0x200D || isCombining(value) ||
        value < 0x20 || in(value, 0x7F, 0x9F)) {
        return 0;
    }
    return isWide(value) ? 2 : 1;
}

}  // namespace

std::size_t nextBoundary(std::string_view text, std::size_t offset) {
    offset = std::min(offset, text.size());
    if (offset == text.size()) return offset;

    Rune first = decode(text, offset);
    std::size_t next = offset + first.bytes;
    bool join_next = false;
    int regional_count = isRegionalIndicator(first.value) ? 1 : 0;
    while (next < text.size()) {
        Rune rune = decode(text, next);
        const bool consume = isCombining(rune.value) || join_next ||
                             rune.value == 0x200D ||
                             (isRegionalIndicator(rune.value) &&
                              regional_count == 1);
        if (!consume) break;
        next += rune.bytes;
        join_next = rune.value == 0x200D;
        if (isRegionalIndicator(rune.value)) ++regional_count;
    }
    return next;
}

std::size_t previousBoundary(std::string_view text, std::size_t offset) {
    offset = std::min(offset, text.size());
    std::size_t previous = 0;
    std::size_t current = 0;
    while (current < offset) {
        previous = current;
        const std::size_t next = nextBoundary(text, current);
        if (next >= offset || next <= current) return previous;
        current = next;
    }
    return previous;
}

std::size_t clampBoundary(std::string_view text, std::size_t offset) {
    offset = std::min(offset, text.size());
    std::size_t current = 0;
    while (current < offset) {
        const std::size_t next = nextBoundary(text, current);
        if (next > offset || next <= current) return current;
        current = next;
    }
    return current;
}

int displayWidth(std::string_view text) {
    int width = 0;
    for (std::size_t offset = 0; offset < text.size();) {
        const std::size_t end = nextBoundary(text, offset);
        int glyph_width = 0;
        for (std::size_t inner = offset; inner < end;) {
            Rune rune = decode(text, inner);
            glyph_width = std::max(glyph_width, runeWidth(rune.value));
            inner += rune.bytes;
        }
        width += glyph_width;
        offset = end;
    }
    return width;
}

std::size_t byteOffsetForColumn(std::string_view text, int column) {
    if (column <= 0) return 0;
    int width = 0;
    std::size_t offset = 0;
    while (offset < text.size()) {
        const std::size_t next = nextBoundary(text, offset);
        const int glyph_width = displayWidth(text.substr(offset, next - offset));
        if (width + glyph_width > column) break;
        width += glyph_width;
        offset = next;
    }
    return offset;
}

std::string sliceColumns(std::string_view text, int start_column,
                         int max_columns) {
    if (max_columns <= 0) return {};
    const std::size_t start = byteOffsetForColumn(text, start_column);
    const std::size_t end = start + byteOffsetForColumn(
                                        text.substr(start), max_columns);
    return std::string(text.substr(start, end - start));
}

std::string truncateColumns(std::string_view text, int max_columns,
                            std::string_view suffix) {
    if (displayWidth(text) <= max_columns) return std::string(text);
    const int suffix_width = displayWidth(suffix);
    if (max_columns <= suffix_width) return sliceColumns(suffix, 0, max_columns);
    return sliceColumns(text, 0, max_columns - suffix_width) +
           std::string(suffix);
}

std::string truncateBytes(std::string_view text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) return std::string(text);
    const std::size_t end = clampBoundary(text, max_bytes);
    return std::string(text.substr(0, end));
}

bool isWordAt(std::string_view text, std::size_t offset) {
    if (offset >= text.size()) return false;
    const Rune rune = decode(text, offset);
    if (rune.value < 0x80) {
        return std::isalnum(static_cast<unsigned char>(rune.value)) != 0 ||
               rune.value == '_';
    }
    return !isCombining(rune.value) && rune.value != 0x200D;
}

bool isValid(std::string_view text) {
    for (std::size_t offset = 0; offset < text.size();) {
        const Rune rune = decode(text, offset);
        if (rune.value == 0xFFFD && rune.bytes == 1) return false;
        offset += rune.bytes;
    }
    return true;
}

}  // namespace ovicode::utf8
