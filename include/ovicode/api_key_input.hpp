#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>

#include "ovicode/clipboard.hpp"

namespace ovicode {

std::optional<std::string> normalizeApiKeyInput(std::string_view value);

struct ApiKeyClipboard {
    std::function<std::optional<std::string>()> read = readSystemClipboard;
    std::function<bool(std::string_view)> write = writeSystemClipboard;
};

struct ApiKeyInput {
    ftxui::Component component;
    ftxui::Component input;
    std::function<void()> reset;
    std::function<void()> begin_edit;
    // Run before the modal's Normal/Insert key mapping so pasted text is data,
    // never navigation commands. Returns true when the event was consumed.
    std::function<bool(const ftxui::Event&)> handle_special_event;
};

ApiKeyInput makeApiKeyInput(std::string& value, ftxui::InputOption options,
                           ApiKeyClipboard clipboard = {});

}  // namespace ovicode
