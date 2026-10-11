#include "ovicode/api_key_input.hpp"

#include <algorithm>
#include <cctype>
#include <memory>
#include <utility>

#include <ftxui/dom/elements.hpp>

namespace ovicode {

using namespace ftxui;

namespace {

constexpr std::size_t kMaxKeyBytes = 4096;

std::string_view trim(std::string_view value) {
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return value;
}

struct KeyState {
    std::string& value;
    ApiKeyClipboard clipboard;
    int cursor = 0;
    bool hidden = true;
    bool show_key = false;
    bool selected_all = false;
    bool pasting = false;
    bool oversized_paste = false;
    std::string paste_buffer;
    std::string notice;

    KeyState(std::string& text, ApiKeyClipboard callbacks)
        : value(text), clipboard(std::move(callbacks)) {}

    void clear() {
        value.clear();
        cursor = 0;
        selected_all = false;
        notice = "Key cleared. Save changes to apply.";
    }

    void paste(std::string_view text) {
        const auto key = normalizeApiKeyInput(text);
        if (!key || key->empty()) {
            notice = "Paste rejected: use one key, without internal spaces or "
                     "control characters. Current key unchanged.";
            return;
        }
        value = *key;
        cursor = static_cast<int>(value.size());
        selected_all = false;
        notice = "Key replaced. Save changes to apply.";
    }

    void copy() {
        const auto key = normalizeApiKeyInput(value);
        if (!key || key->empty()) {
            notice = "No valid key to copy.";
        } else if (!clipboard.write || !clipboard.write(*key)) {
            notice = "Clipboard unavailable. Show the key and use terminal copy.";
        } else {
            notice = "Key copied. Your system clipboard now contains a secret.";
        }
    }

    void paste_clipboard() {
        const auto text = clipboard.read ? clipboard.read() : std::nullopt;
        if (text) {
            paste(*text);
        } else {
            notice = "Clipboard unavailable. Focus the key and use terminal paste.";
        }
    }
};

}  // namespace

std::optional<std::string> normalizeApiKeyInput(std::string_view value) {
    if (value.size() > kMaxKeyBytes) return std::nullopt;
    value = trim(value);
    if (value.size() >= 2 &&
        ((value.front() == '"' && value.back() == '"') ||
         (value.front() == '\'' && value.back() == '\''))) {
        value = trim(value.substr(1, value.size() - 2));
    }
    if (value.starts_with("Bearer ")) value = trim(value.substr(7));
    if (value.size() >= 2 &&
        ((value.front() == '"' && value.back() == '"') ||
         (value.front() == '\'' && value.back() == '\''))) {
        value = trim(value.substr(1, value.size() - 2));
    }
    for (const unsigned char character : value) {
        if (character <= 32 || character >= 127 || character == '"' ||
            character == '\'') {
            return std::nullopt;
        }
    }
    return std::string(value);
}

ApiKeyInput makeApiKeyInput(std::string& value, InputOption options,
                           ApiKeyClipboard clipboard) {
    auto state = std::make_shared<KeyState>(value, std::move(clipboard));
    options.password = &state->hidden;
    options.multiline = false;
    options.cursor_position = &state->cursor;
    const auto on_change = options.on_change;
    options.on_change = [state, on_change] {
        state->notice.clear();
        if (on_change) on_change();
    };
    auto input = Input(&value, "API key", std::move(options));
    auto input_events = CatchEvent(input, [state](Event event) {
        if (event.is_character()) {
            if (event.character().size() +
                    (state->selected_all ? 0 : state->value.size()) >
                kMaxKeyBytes) {
                state->notice = "Key is too long. Paste only the API key.";
                return true;
            }
            if (state->selected_all) {
                state->value.clear();
                state->cursor = 0;
                state->selected_all = false;
            }
        } else if (event == Event::Backspace || event == Event::Delete) {
            if (state->selected_all) {
                state->clear();
                return true;
            }
        } else if (event != Event::Custom && !event.is_mouse()) {
            state->selected_all = false;
        }
        return false;
    });
    CheckboxOption visibility_option;
    visibility_option.transform = [](const EntryState& entry) {
        Element radio = text((entry.state ? "(*) " : "( ) ") + entry.label);
        if (entry.focused) radio |= inverted;
        if (entry.state) radio |= bold;
        return radio;
    };
    visibility_option.on_change = [state] {
        state->hidden = !state->show_key;
    };
    auto visibility = Checkbox("Show key", &state->show_key, visibility_option);
    auto copy = Button("Copy", [state] { state->copy(); });
    auto paste = Button("Paste", [state] { state->paste_clipboard(); });
    auto clear = Button("Clear", [state] { state->clear(); });
    auto buttons = Container::Horizontal({copy, paste, clear});
    auto input_row = Container::Horizontal({input_events, visibility});
    auto layout = Container::Vertical({input_row, buttons});
    auto component = Renderer(layout, [=] {
        Element rendered = input_events->Render();
        if (input->Focused()) rendered |= focusCursorBlock;
        return vbox({
            hbox({std::move(rendered) | size(HEIGHT, EQUAL, 1) | xflex,
                  text(" "), visibility->Render()}),
            hbox({copy->Render(), text(" "), paste->Render(), text(" "),
                  clear->Render()}),
            paragraph(state->notice.empty()
                          ? (state->selected_all
                                  ? "All selected; type or paste to replace the key."
                                  : "Ctrl+A select all · Ctrl+C copy · Ctrl+V "
                                    "replace · Focus Show key and press Enter "
                                    "to toggle")
                          : state->notice),
        });
    });
    auto reset = [state] {
        state->hidden = true;
        state->show_key = false;
        state->selected_all = false;
        state->pasting = false;
        state->oversized_paste = false;
        state->paste_buffer.clear();
        state->notice.clear();
        state->cursor = static_cast<int>(state->value.size());
    };
    reset();
    auto begin_edit = [state] {
        state->selected_all = true;
        state->cursor = static_cast<int>(state->value.size());
        state->notice.clear();
    };
    auto special = [state, input, begin_edit](const Event& event) {
        if (state->pasting) {
            if (event.input() == "\x1b[201~") {
                state->pasting = false;
                if (state->oversized_paste) {
                    state->notice = "Paste rejected: key is too long. "
                                    "Current key unchanged.";
                } else {
                    state->paste(state->paste_buffer);
                }
                state->paste_buffer.clear();
            } else if (event != Event::Custom) {
                if (state->paste_buffer.size() + event.input().size() >
                    kMaxKeyBytes) {
                    state->oversized_paste = true;
                } else if (!state->oversized_paste) {
                    state->paste_buffer += event.input();
                }
            }
            return event != Event::Custom;
        }
        if (!input->Focused()) return false;
        if (event.input() == "\x1b[200~") {
            state->pasting = true;
            state->oversized_paste = false;
            state->paste_buffer.clear();
        } else if (event == Event::Special({1})) {
            begin_edit();
        } else if (event == Event::Special({3})) {
            state->copy();
        } else if (event == Event::Special({22})) {
            state->paste_clipboard();
        } else if (event == Event::Special({21})) {
            state->clear();
        } else if (event == Event::Special({5})) {
            state->selected_all = false;
            state->cursor = static_cast<int>(state->value.size());
        } else {
            return false;
        }
        return true;
    };
    return {component, input, reset, begin_edit, special};
}

}  // namespace ovicode
