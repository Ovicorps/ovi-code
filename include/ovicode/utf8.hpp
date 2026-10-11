#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace ovicode::utf8 {

// Cursor helpers operate on extended-grapheme-like boundaries. They cover
// combining marks, variation selectors, emoji modifiers, ZWJ sequences, and
// regional-indicator pairs without exposing byte offsets to editor callers.
std::size_t nextBoundary(std::string_view text, std::size_t offset);
std::size_t previousBoundary(std::string_view text, std::size_t offset);
std::size_t clampBoundary(std::string_view text, std::size_t offset);

int displayWidth(std::string_view text);
std::size_t byteOffsetForColumn(std::string_view text, int column);
std::string sliceColumns(std::string_view text, int start_column,
                         int max_columns);
std::string truncateColumns(std::string_view text, int max_columns,
                            std::string_view suffix = "...");
std::string truncateBytes(std::string_view text, std::size_t max_bytes);
bool isWordAt(std::string_view text, std::size_t offset);
bool isValid(std::string_view text);

}  // namespace ovicode::utf8
