#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace ovicode {

std::optional<std::string> readSystemClipboard();
bool writeSystemClipboard(std::string_view text);

}  // namespace ovicode
