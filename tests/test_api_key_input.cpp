#include <iostream>
#include <optional>
#include <string>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include "ovicode/api_key_input.hpp"

namespace {

using namespace ovicode;
using namespace ftxui;
int failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::cerr << __FILE__ << ':' << __LINE__                            \
                      << ": check failed: " #condition << '\n';                \
            ++failures;                                                         \
        }                                                                       \
    } while (false)

struct KeyFixture {
    std::string key = "sk-or-v1-original-test-key";
    std::optional<std::string> clipboard;
    std::string copied;
    bool copy_succeeds = true;
    ApiKeyInput field = makeApiKeyInput(
        key, InputOption{},
        {[this] { return clipboard; },
         [this](std::string_view text) {
             if (!copy_succeeds) return false;
             copied = text;
             return true;
         }});

    std::string render() {
        auto screen = Screen::Create(Dimension::Fixed(120), Dimension::Fixed(8));
        Render(screen, field.component->Render());
        return screen.ToString();
    }

    void focus() {
        field.input->TakeFocus();
        render();
    }

    void paste(std::string_view text) {
        CHECK(field.handle_special_event(Event::Special("\x1b[200~")));
        for (const char character : text) {
            CHECK(field.handle_special_event(
                character == '\n' ? Event::Return : Event::Character(character)));
        }
        CHECK(field.handle_special_event(Event::Special("\x1b[201~")));
    }
};

void testNormalization() {
    CHECK(normalizeApiKeyInput("  sk-or-v1-test \r\n") == "sk-or-v1-test");
    CHECK(normalizeApiKeyInput("Bearer sk-or-v1-test") == "sk-or-v1-test");
    CHECK(normalizeApiKeyInput("\"Bearer sk-or-v1-test\"") == "sk-or-v1-test");
    CHECK(normalizeApiKeyInput("Bearer 'sk-or-v1-test'") == "sk-or-v1-test");
    CHECK(normalizeApiKeyInput("'sk-or-v1-test'") == "sk-or-v1-test");
    CHECK(normalizeApiKeyInput(" \r\n") == "");
    CHECK(!normalizeApiKeyInput("sk-or-v1-test other-key"));
    CHECK(!normalizeApiKeyInput("sk-or-v1-test\nother-key"));
    CHECK(!normalizeApiKeyInput("sk-or-v1-test\x1b[31m"));
    CHECK(!normalizeApiKeyInput(std::string("key\0data", 8)));
    CHECK(!normalizeApiKeyInput("\"sk-or-v1-test"));
    CHECK(!normalizeApiKeyInput("sk-or-v1-test\tmore"));
    CHECK(!normalizeApiKeyInput(std::string(4097, 'x')));
    CHECK(normalizeApiKeyInput(std::string(4096, 'x')).has_value());
}

void testVisibilityAndSelection() {
    KeyFixture fixture;
    fixture.focus();
    const std::string original = fixture.key;
    CHECK(fixture.render().find(original) == std::string::npos);
    CHECK(fixture.render().find("( ) Show key") != std::string::npos);
    CHECK(fixture.render().find("F2") == std::string::npos);
    CHECK(!fixture.field.handle_special_event(Event::F2));
    fixture.field.component->OnEvent(Event::F2);
    fixture.field.component->OnEvent(Event::Return);
    CHECK(fixture.render().find(original) == std::string::npos);
    CHECK(fixture.field.component->OnEvent(Event::Tab));
    CHECK(!fixture.field.input->Focused());
    CHECK(fixture.render().find(original) == std::string::npos);
    CHECK(fixture.field.component->OnEvent(Event::Return));
    CHECK(fixture.render().find(original) != std::string::npos);
    CHECK(fixture.render().find("(*) Show key") != std::string::npos);
    CHECK(fixture.field.component->OnEvent(Event::Return));
    CHECK(fixture.render().find(original) == std::string::npos);
    CHECK(fixture.render().find("( ) Show key") != std::string::npos);
    CHECK(fixture.field.component->OnEvent(Event::Return));
    CHECK(fixture.key == original);
    fixture.field.reset();
    CHECK(fixture.render().find(original) == std::string::npos);
    CHECK(fixture.render().find("( ) Show key") != std::string::npos);
    fixture.focus();
    CHECK(fixture.field.handle_special_event(Event::Special({1})));
    CHECK(fixture.render().find("All selected") != std::string::npos);
    fixture.field.component->OnEvent(Event::Character("new-test-key"));
    CHECK(fixture.key == "new-test-key");
    fixture.field.component->OnEvent(Event::Return);
    CHECK(fixture.key == "new-test-key");
    fixture.field.component->OnEvent(Event::End);
    fixture.field.component->OnEvent(Event::Character("-edited"));
    CHECK(fixture.key == "new-test-key-edited");
    fixture.field.begin_edit();
    fixture.field.component->OnEvent(Event::Backspace);
    CHECK(fixture.key.empty());
    fixture.key = original;
    fixture.field.reset();
    fixture.field.begin_edit();
    fixture.field.component->OnEvent(Event::Character(std::string(4097, 'x')));
    CHECK(fixture.key == original);
}

void testClipboard() {
    KeyFixture fixture;
    fixture.focus();
    fixture.clipboard = "\r\n\"Bearer sk-or-v1-pasted-test-key\" \n";
    CHECK(fixture.field.handle_special_event(Event::Special({22})));
    CHECK(fixture.key == "sk-or-v1-pasted-test-key");
    CHECK(fixture.field.handle_special_event(Event::Special({3})));
    CHECK(fixture.copied == fixture.key);
    CHECK(fixture.render().find("contains a secret") != std::string::npos);
    CHECK(fixture.render().find(fixture.key) == std::string::npos);
    const std::string original = fixture.key;
    fixture.clipboard = "invalid\nmultiple-keys";
    CHECK(fixture.field.handle_special_event(Event::Special({22})));
    CHECK(fixture.key == original);
    CHECK(fixture.render().find("Paste rejected") != std::string::npos);
    fixture.clipboard = "";
    CHECK(fixture.field.handle_special_event(Event::Special({22})));
    CHECK(fixture.key == original);
    fixture.clipboard = std::nullopt;
    CHECK(fixture.field.handle_special_event(Event::Special({22})));
    CHECK(fixture.key == original);
    CHECK(fixture.render().find("Clipboard unavailable") != std::string::npos);
    fixture.copy_succeeds = false;
    CHECK(fixture.field.handle_special_event(Event::Special({3})));
    CHECK(fixture.key == original);
    CHECK(fixture.render().find("Clipboard unavailable") != std::string::npos);
    CHECK(fixture.field.handle_special_event(Event::Special({21})));
    CHECK(fixture.key.empty());
}

void testBracketedPaste() {
    KeyFixture fixture;
    fixture.focus();
    fixture.paste("  Bearer sk-or-v1-qqqiiiasss-test-key\n");
    CHECK(fixture.key == "sk-or-v1-qqqiiiasss-test-key");
    const std::string original = fixture.key;
    fixture.paste("bad\nmultiple-keys");
    CHECK(fixture.key == original);
    fixture.paste(std::string(5000, 'x'));
    CHECK(fixture.key == original);
    CHECK(fixture.render().find("too long") != std::string::npos);
    CHECK(fixture.field.handle_special_event(Event::Special("\x1b[200~")));
    CHECK(fixture.field.handle_special_event(Event::Character("partial")));
    fixture.field.reset();
    CHECK(fixture.key == original);
    CHECK(fixture.field.handle_special_event(Event::Special("\x1b[200~")));
    CHECK(fixture.field.handle_special_event(Event::Escape));
    CHECK(fixture.field.handle_special_event(Event::Character("malicious")));
    CHECK(fixture.field.handle_special_event(Event::Special("\x1b[201~")));
    CHECK(fixture.key == original);
    fixture.paste("sk-or-v1-latest-test-key");
    CHECK(fixture.key == "sk-or-v1-latest-test-key");
}

void testControls() {
    KeyFixture fixture;
    fixture.focus();
    fixture.field.component->OnEvent(Event::End);
    fixture.field.component->OnEvent(Event::ArrowRight);
    CHECK(!fixture.field.input->Focused());
    CHECK(!fixture.field.handle_special_event(Event::F2));
    fixture.field.component->OnEvent(Event::Return);
    CHECK(fixture.render().find("(*) Show key") != std::string::npos);
    CHECK(fixture.render().find(fixture.key) != std::string::npos);
    fixture.field.component->OnEvent(Event::ArrowDown);
    fixture.field.component->OnEvent(Event::Return);
    CHECK(fixture.copied == fixture.key);
    CHECK(fixture.render().find("(*) Show key") != std::string::npos);
    fixture.clipboard = "sk-or-v1-button-paste-key";
    fixture.field.component->OnEvent(Event::ArrowRight);
    fixture.field.component->OnEvent(Event::Return);
    CHECK(fixture.key == "sk-or-v1-button-paste-key");
    fixture.field.component->OnEvent(Event::ArrowRight);
    fixture.field.component->OnEvent(Event::Return);
    CHECK(fixture.key.empty());
    fixture.field.reset();
    CHECK(fixture.render().find("( ) Show key") != std::string::npos);
}

}  // namespace

int main() {
    testNormalization();
    testVisibilityAndSelection();
    testClipboard();
    testBracketedPaste();
    testControls();
    if (failures) return 1;
    std::cout << "all API key input checks passed\n";
    return 0;
}
