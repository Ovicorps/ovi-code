#include "ovicode/tui.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include "ovicode/agent_md.hpp"
#include "ovicode/providers.hpp"
#include "ovicode/react.hpp"

namespace ovicode {

namespace fs = std::filesystem;
using namespace ftxui;

namespace {

// Chat screen uses a vim-style modal model: Insert mode types into the
// prompt, Normal mode (after Esc) dispatches single-key commands. We avoid
// Ctrl+S / Ctrl+Q entirely — those collide with terminal XON/XOFF flow
// control on many setups (the prompt would silently freeze).
enum class ChatMode { Insert, Normal };

// ---- Theme / palette ----------------------------------------------------
//
// Every color in the TUI is routed through Palette. Each palette owns both
// foreground and background, so contrast never depends on the terminal's
// configured background color. Auto-detection only chooses which complete
// canvas to use; users can override it permanently in settings.
struct Palette {
    Color foreground;
    Color background;
    Color border;
    Color accent;          // app name, prompt glyph (neutral)
    Color muted;           // secondary status text (model / status)
    Color insert_mode;     // -- INSERT -- tag, prompt glyph in Insert
    Color normal_mode;     // -- NORMAL -- tag, prompt glyph in Normal
    Color busy;            // "thinking…"
    Color ready;           // "ready"
    Color technique;       // technique label in header
    Color user_entry;
    Color assistant_entry;
    Color tool_call;
    Color tool_result;
    Color system_entry;
    Color error;
    Color logo;
    Color logo_subtitle;
};

Palette darkPalette() {
    return Palette{
        /*foreground     */ Color::RGB(242, 244, 248),
        /*background     */ Color::RGB(11, 13, 16),
        /*border         */ Color::RGB(183, 192, 204),
        /*accent         */ Color::RGB(99, 217, 230),
        /*muted          */ Color::RGB(183, 192, 204),
        /*insert_mode    */ Color::RGB(112, 217, 139),
        /*normal_mode    */ Color::RGB(244, 201, 93),
        /*busy           */ Color::RGB(244, 201, 93),
        /*ready          */ Color::RGB(183, 192, 204),
        /*technique      */ Color::RGB(214, 165, 247),
        /*user_entry     */ Color::RGB(244, 201, 93),
        /*assistant_entry*/ Color::RGB(112, 217, 139),
        /*tool_call      */ Color::RGB(99, 217, 230),
        /*tool_result    */ Color::RGB(128, 191, 255),
        /*system_entry   */ Color::RGB(214, 165, 247),
        /*error          */ Color::RGB(255, 123, 114),
        /*logo           */ Color::RGB(99, 217, 230),
        /*logo_subtitle  */ Color::RGB(183, 192, 204),
    };
}

Palette lightPalette() {
    return Palette{
        /*foreground     */ Color::RGB(17, 19, 24),
        /*background     */ Color::RGB(250, 250, 250),
        /*border         */ Color::RGB(71, 81, 93),
        /*accent         */ Color::RGB(7, 90, 156),
        /*muted          */ Color::RGB(71, 81, 93),
        /*insert_mode    */ Color::RGB(23, 107, 53),
        /*normal_mode    */ Color::RGB(155, 28, 49),
        /*busy           */ Color::RGB(122, 62, 0),
        /*ready          */ Color::RGB(71, 81, 93),
        /*technique      */ Color::RGB(111, 42, 142),
        /*user_entry     */ Color::RGB(122, 62, 0),
        /*assistant_entry*/ Color::RGB(23, 107, 53),
        /*tool_call      */ Color::RGB(7, 90, 156),
        /*tool_result    */ Color::RGB(11, 95, 106),
        /*system_entry   */ Color::RGB(111, 42, 142),
        /*error          */ Color::RGB(163, 19, 42),
        /*logo           */ Color::RGB(7, 90, 156),
        /*logo_subtitle  */ Color::RGB(71, 81, 93),
    };
}

Palette plainPalette() {
    Color fg = Color::RGB(245, 245, 245);
    return Palette{
        /*foreground     */ fg,
        /*background     */ Color::RGB(10, 10, 10),
        /*border         */ fg,
        /*accent         */ fg,
        /*muted          */ fg,
        /*insert_mode    */ fg,
        /*normal_mode    */ fg,
        /*busy           */ fg,
        /*ready          */ fg,
        /*technique      */ fg,
        /*user_entry     */ fg,
        /*assistant_entry*/ fg,
        /*tool_call      */ fg,
        /*tool_result    */ fg,
        /*system_entry   */ fg,
        /*error          */ fg,
        /*logo           */ fg,
        /*logo_subtitle  */ fg,
    };
}

// System theme detection. Probed once per process and cached — re-running
// `defaults` on every render would be wasteful, and the user can always pick
// Dark / Light / Plain explicitly if their environment doesn't auto-detect.
//
// Order of evidence:
//   1. $COLORFGBG  ("fg;bg" or "fg;default;bg") — set by xterm-family
//      emulators, iTerm2, kitty, alacritty, wezterm. Background index <= 6
//      = dark, >= 7 = light. Authoritative when present because it reflects
//      the user's actual terminal colors, not the OS UI mode.
//   2. macOS Appearance — `defaults read -g AppleInterfaceStyle` prints
//      "Dark" when the system is in Dark Mode and exits non-zero when in
//      Light Mode. Only consulted when COLORFGBG is absent or unparseable.
//   3. Default to Dark — most dev terminals are dark, and the dark palette
//      degrades more gracefully than the light one on unknown backgrounds.
bool detectSystemDarkBackground() {
    static const bool cached = []() -> bool {
        if (const char* v = std::getenv("COLORFGBG"); v && *v) {
            std::string s = v;
            auto last = s.find_last_of(';');
            if (last != std::string::npos) {
                try {
                    return std::stoi(s.substr(last + 1)) < 7;
                } catch (...) {
                    // fall through to other probes
                }
            }
        }
#ifdef __APPLE__
        if (FILE* p = ::popen(
                "defaults read -g AppleInterfaceStyle 2>/dev/null", "r")) {
            char buf[64] = {0};
            std::size_t n = std::fread(buf, 1, sizeof(buf) - 1, p);
            int rc = ::pclose(p);
            if (rc == 0) {
                std::string s(buf, n);
                // Successful read with "Dark" in the value = Dark Mode.
                // Successful read with anything else (rare) — treat as light.
                // Non-zero rc means the key is unset → Light Mode.
                return s.find("Dark") != std::string::npos;
            }
            return false;  // key unset = Light Mode on macOS
        }
#endif
        return true;
    }();
    return cached;
}

const Palette& palette(Theme t) {
    static const Palette dark  = darkPalette();
    static const Palette light = lightPalette();
    static const Palette plain = plainPalette();
    switch (t) {
        case Theme::Dark:  return dark;
        case Theme::Light: return light;
        case Theme::Plain: return plain;
        case Theme::Auto:
            return detectSystemDarkBackground() ? dark : light;
    }
    return dark;
}

Element onPaletteCanvas(const Palette& pal, Element element) {
    return std::move(element) | color(pal.foreground) |
           bgcolor(pal.background);
}

InputOption paletteInputOption(const Config& cfg) {
    InputOption option;
    option.transform = [&cfg](InputState state) {
        const Palette& pal = palette(cfg.theme);
        state.element |= color(state.is_placeholder ? pal.muted
                                                    : pal.foreground);
        if (state.focused) state.element |= inverted;
        return state.element;
    };
    return option;
}

MenuOption highContrastMenuOption() {
    MenuOption option = MenuOption::Vertical();
    option.entries_option.transform = [](const EntryState& state) {
        Element entry = text((state.active ? "> " : "  ") + state.label);
        if (state.focused) entry |= inverted;
        if (state.active) entry |= bold;
        return entry;
    };
    return option;
}

std::vector<std::string> techniqueIds(
    const TechniquePluginRegistry& plugins) {
    std::vector<std::string> out;
    for (Technique technique : allTechniques())
        out.push_back(techniqueToString(technique));
    for (const auto& plugin : plugins.plugins()) out.push_back(plugin->id);
    return out;
}

std::vector<std::string> techniqueLabels(
    const TechniquePluginRegistry& plugins) {
    std::vector<std::string> out;
    for (Technique technique : allTechniques()) {
        out.push_back(techniqueLabel(technique) + " — " +
                      techniqueDescription(technique));
    }
    for (const auto& plugin : plugins.plugins()) {
        out.push_back(plugin->label + " [plugin] — " + plugin->description);
    }
    return out;
}

int indexOfTechniqueId(const std::vector<std::string>& ids,
                       const std::string& id) {
    for (size_t i = 0; i < ids.size(); ++i) {
        if (ids[i] == id) return static_cast<int>(i);
    }
    return -1;
}

void selectTechnique(Config& cfg, const std::string& id) {
    cfg.technique_id = id;
    for (Technique technique : allTechniques()) {
        if (techniqueToString(technique) == id) {
            cfg.technique = technique;
            return;
        }
    }
    cfg.technique = Technique::Standard;
}

std::string selectedTechniqueLabel(
    const Config& cfg, const TechniquePluginRegistry& plugins) {
    const std::string id = cfg.technique_id.empty()
                               ? techniqueToString(cfg.technique)
                               : cfg.technique_id;
    if (const auto* plugin = plugins.find(id)) return plugin->label;
    for (Technique technique : allTechniques()) {
        if (techniqueToString(technique) == id) return techniqueLabel(technique);
    }
    return "Missing plugin: " + id;
}

int indexOfModel(const std::vector<std::string>& models,
                 const std::string& m) {
    for (size_t i = 0; i < models.size(); ++i) {
        if (models[i] == m) return static_cast<int>(i);
    }
    return -1;
}

std::string reasoningEffortChoiceLabel(Provider provider,
                                        const std::string& model,
                                        ReasoningEffort effort) {
    if (effort != ReasoningEffort::Auto) return reasoningEffortLabel(effort);
    const ModelInfo* info = findModelInfo(provider, model);
    if (info && info->auto_effort != ReasoningEffort::Auto) {
        return "Auto — model default: " +
               reasoningEffortLabel(info->auto_effort);
    }
    return "Auto — model managed";
}

int indexOfReasoningEffort(const std::vector<ReasoningEffort>& options,
                           ReasoningEffort effort) {
    for (size_t i = 0; i < options.size(); ++i) {
        if (options[i] == effort) return static_cast<int>(i);
    }
    return 0;
}

void refreshReasoningEffortChoices(
    Provider provider, const std::string& model, ReasoningEffort selected,
    std::vector<ReasoningEffort>& options, std::vector<std::string>& choices,
    int& selected_index) {
    options = reasoningEffortOptions(provider, model);
    choices.clear();
    choices.reserve(options.size());
    for (ReasoningEffort effort : options) {
        choices.push_back(reasoningEffortChoiceLabel(provider, model, effort));
    }
    selected_index = indexOfReasoningEffort(options, selected);
}

Element banner(const Palette& pal, const std::string& title) {
    return vbox({
        text("┌─ OVI-CODE ─ " + title + " " +
             std::string(40, '-')) |
            color(pal.accent),
    });
}

// Wipe the terminal (visible area + scrollback) and home the cursor before a
// non-fullscreen FTXUI screen draws its first frame. Without this the wizard
// and settings views render below whatever shell output preceded them, which
// looks like leftover clutter from a previous session.
void clearTerminal() {
    std::fputs("\x1b[3J\x1b[2J\x1b[H", stdout);
    std::fflush(stdout);
}

// Shared vim-style modal wrapper for the wizard + settings screens, mirroring
// the chat screen's NORMAL/INSERT model so the whole app navigates the same
// way:
//
//   NORMAL — j/k/h/l are translated to the arrow keys, so they drive menu
//            selection *and* move focus between fields (FTXUI Menus consume
//            Up/Down; on plain Inputs/Buttons the arrows fall through to the
//            container). i/a begin editing the focused Input; Enter activates
//            a focused Button; Tab still cycles fields natively; q or Esc
//            cancel via `on_cancel`.
//   INSERT — keystrokes flow straight to the focused FTXUI Input; Esc returns
//            to NORMAL.
//
// Every other raw character is swallowed in NORMAL so text Inputs never absorb
// navigation keys — the same invariant the chat screen relies on. `mode` is
// owned by the caller so its renderer can show the active mode in the footer.
Component vimModalLayer(Component child, ChatMode& mode,
                        std::function<void()> on_cancel) {
    return CatchEvent(
        child, [child, &mode, on_cancel = std::move(on_cancel)](Event e) {
            if (mode == ChatMode::Insert) {
                if (e == Event::Escape) {
                    mode = ChatMode::Normal;
                    return true;
                }
                return false;  // typing flows to the focused Input
            }
            // ---- NORMAL mode ----
            if (e == Event::Escape || e == Event::Character("q")) {
                on_cancel();
                return true;
            }
            if (e == Event::Character("i") || e == Event::Character("a")) {
                mode = ChatMode::Insert;
                return true;
            }
            if (e == Event::Character("j")) {
                child->OnEvent(Event::ArrowDown);
                return true;
            }
            if (e == Event::Character("k")) {
                child->OnEvent(Event::ArrowUp);
                return true;
            }
            if (e == Event::Character("h")) {
                child->OnEvent(Event::ArrowLeft);
                return true;
            }
            if (e == Event::Character("l")) {
                child->OnEvent(Event::ArrowRight);
                return true;
            }
            // Let Space reach any focused component that handles it. Enter
            // already reaches components natively (it isn't a character event).
            if (e == Event::Character(" ")) return false;
            // Swallow any other raw character so focused Inputs stay inert in
            // NORMAL; arrows / Tab / Enter still reach the components natively.
            if (e.is_character()) return true;
            return false;
        });
}

// FTXUI's Input paints a blinking *bar* cursor when focused (it hard-codes
// `focusCursorBarBlinking`). We want a rectangle everywhere — matching the
// block cursor the chat editor draws with an `inverted` cell — so override the
// focused input's shape to a steady block. Only the *focused* input is wrapped:
// FocusCursor::Render sets the terminal cursor unconditionally, so wrapping
// every input would pin the cursor to whichever one renders last instead of
// the one being edited.
Element blockCursorInput(const Component& input) {
    Element e = input->Render();
    return input->Focused() ? focusCursorBlock(std::move(e)) : e;
}

}  // namespace

// -- Setup wizard -----------------------------------------------------------

bool runSetupWizard(Config& cfg, const TechniquePluginRegistry& plugins) {
    clearTerminal();
    auto screen = ScreenInteractive::TerminalOutput();

    int step = 0;  // provider, key, model, effort, technique
    Config staged = cfg;
    const auto& providers = knownProviders();
    std::vector<std::string> provider_choices;
    int provider_idx = 0;
    for (size_t i = 0; i < providers.size(); ++i) {
        provider_choices.push_back(providers[i].label);
        if (providers[i].provider == staged.provider) {
            provider_idx = static_cast<int>(i);
        }
    }
    auto technique_ids = techniqueIds(plugins);
    const std::string configured_technique =
        cfg.technique_id.empty() ? techniqueToString(cfg.technique)
                                 : cfg.technique_id;
    int technique_idx =
        indexOfTechniqueId(technique_ids, configured_technique);
    if (technique_idx < 0) {
        technique_ids.push_back(configured_technique);
        technique_idx = static_cast<int>(technique_ids.size()) - 1;
    }
    Provider loaded_provider = staged.provider;
    std::string api_key = providerAccount(staged, loaded_provider).api_key;
    std::string model = providerAccount(staged, loaded_provider).model;
    if (model.empty()) model = defaultModel(loaded_provider);
    ReasoningEffort effort = staged.reasoning_effort;
    bool aborted = false;
    bool finished = false;
    // Start in NORMAL so j/k drive the menus immediately; i/a edits inputs.
    ChatMode mode = ChatMode::Normal;

    auto technique_choices = techniqueLabels(plugins);
    if (technique_choices.size() < technique_ids.size()) {
        technique_choices.push_back("Missing plugin: " + configured_technique);
    }

    InputOption key_opts = paletteInputOption(cfg);
    key_opts.password = true;
    auto key_input = Input(&api_key, "API key", key_opts);

    auto provider_menu =
        Menu(&provider_choices, &provider_idx, highContrastMenuOption());
    std::vector<std::string> model_choices = knownModels(loaded_provider);
    model_choices.push_back("(custom — type below)");
    int model_idx = indexOfModel(model_choices, model);
    if (model_idx < 0) model_idx = static_cast<int>(model_choices.size()) - 1;
    auto model_menu =
        Menu(&model_choices, &model_idx, highContrastMenuOption());
    auto model_input = Input(&model, "", paletteInputOption(cfg));

    std::vector<ReasoningEffort> effort_options;
    std::vector<std::string> effort_choices;
    int effort_idx = 0;
    refreshReasoningEffortChoices(loaded_provider, model, effort, effort_options,
                                  effort_choices, effort_idx);
    auto effort_menu =
        Menu(&effort_choices, &effort_idx, highContrastMenuOption());

    auto technique_menu =
        Menu(&technique_choices, &technique_idx, highContrastMenuOption());

    auto save_loaded_account = [&] {
        auto& account = providerAccount(staged, loaded_provider);
        account.api_key = api_key;
        if (model_idx >= 0 &&
            model_idx < static_cast<int>(model_choices.size()) - 1) {
            account.model = model_choices[model_idx];
        } else if (!model.empty()) {
            account.model = model;
        } else {
            account.model = defaultModel(loaded_provider);
        }
        model = account.model;
    };
    auto load_provider = [&](Provider provider) {
        loaded_provider = provider;
        staged.provider = provider;
        const auto& account = providerAccount(staged, provider);
        api_key = account.api_key;
        model = account.model.empty() ? defaultModel(provider) : account.model;
        model_choices = knownModels(provider);
        model_choices.push_back("(custom — type below)");
        model_idx = indexOfModel(model_choices, model);
        if (model_idx < 0) {
            model_idx = static_cast<int>(model_choices.size()) - 1;
        }
        refreshReasoningEffortChoices(provider, model, effort, effort_options,
                                      effort_choices, effort_idx);
    };

    auto next_button = Button("Next  ▶", [&] {
        if (step == 0) {
            save_loaded_account();
            if (provider_idx >= 0 &&
                provider_idx < static_cast<int>(providers.size())) {
                load_provider(providers[provider_idx].provider);
            }
            ++step;
        } else if (step == 1) {
            providerAccount(staged, loaded_provider).api_key = api_key;
            ++step;
        } else if (step == 2) {
            if (model_idx >= 0 &&
                model_idx < static_cast<int>(model_choices.size()) - 1) {
                model = model_choices[model_idx];
            }
            save_loaded_account();
            refreshReasoningEffortChoices(loaded_provider, model, effort,
                                          effort_options,
                                          effort_choices, effort_idx);
            ++step;
        } else if (step == 3) {
            effort = effort_options[effort_idx];
            staged.reasoning_effort = effort;
            ++step;
        } else if (step == 4) {
            finished = true;
            screen.ExitLoopClosure()();
        }
    });
    auto back_button = Button("◀ Back", [&] {
        if (step > 0) --step;
    });
    auto cancel_button = Button("Cancel", [&] {
        aborted = true;
        screen.ExitLoopClosure()();
    });

    auto buttons = Container::Horizontal({back_button, next_button, cancel_button});

    auto layout = Container::Tab(
        {
            provider_menu,
            key_input,
            Container::Vertical({model_menu, model_input}),
            effort_menu,
            technique_menu,
        },
        &step);

    auto root = Container::Vertical({layout, buttons});

    auto renderer = Renderer(root, [&] {
        const Palette& pal = palette(cfg.theme);
        // See makeSettingsPanel: hide the cursor by default so it only appears
        // (as a block) on the focused Input, never lingering on the menus.
        screen.SetCursor(Screen::Cursor{0, 0, Screen::Cursor::Hidden});
        Element body;
        if (step == 0) {
            body = vbox({
                text("1. Choose an LLM provider") | bold | color(pal.accent),
                separator(),
                provider_menu->Render() | frame | size(HEIGHT, LESS_THAN, 8),
                text(""),
                paragraph(
                    "Each provider keeps its own API key and model selection. "
                    "You can switch accounts later in Settings.") |
                    color(pal.muted),
            });
        } else if (step == 1) {
            body = vbox({
                text("2. Configure " + providerLabel(loaded_provider)) | bold |
                    color(pal.accent),
                separator(),
                hbox({text("API key: "), blockCursorInput(key_input)}),
                text(""),
                paragraph(
                    "Saved to ~/.ovi-code/config.json with 0600 permissions. "
                    "Leave blank when " +
                    providerInfo(loaded_provider).environment_variable +
                    " is set in the environment.") |
                    color(pal.muted),
            });
        } else if (step == 2) {
            body = vbox({
                text("3. Choose a " + providerLabel(loaded_provider) + " model") |
                    bold | color(pal.accent),
                separator(),
                model_menu->Render() | frame |
                    size(HEIGHT, LESS_THAN, 10),
                text(""),
                hbox({text("Custom model: "), blockCursorInput(model_input)}),
            });
        } else if (step == 3) {
            body = vbox({
                text("4. Choose reasoning effort") | bold |
                    color(pal.accent),
                separator(),
                effort_menu->Render() | frame |
                    size(HEIGHT, LESS_THAN, 10),
                text(""),
                paragraph(
                    "Only effort levels supported by the selected model are "
                    "shown. Auto lets the provider manage reasoning.") |
                    color(pal.muted),
            });
        } else {
            body = vbox({
                text("5. Choose default agentic technique") | bold |
                    color(pal.accent),
                separator(),
                technique_menu->Render() | frame |
                    size(HEIGHT, LESS_THAN, 12),
                text(""),
                paragraph(
                    "You can switch techniques any time from the chat "
                    "screen via Normal mode (Esc, then t).") |
                    color(pal.muted),
            });
        }
        Element content = vbox({
            banner(pal, "setup"),
            text(""),
            body,
            text(""),
            separator(),
            hbox({
                back_button->Render(),
                text("  "),
                next_button->Render(),
                text("  "),
                cancel_button->Render(),
                filler(),
                text(mode == ChatMode::Insert ? " -- INSERT -- "
                                              : " -- NORMAL -- ") |
                    color(mode == ChatMode::Insert ? pal.insert_mode
                                                   : pal.normal_mode) |
                    bold,
                text("  "),
                text("step " + std::to_string(step + 1) + "/5") |
                    color(pal.muted),
            }),
            text(mode == ChatMode::Insert
                      ? "INSERT · type · Esc → NORMAL"
                      : "NORMAL · j/k choose · Tab field · i/a edit · "
                        "Enter button · q cancel") |
                color(pal.muted) | center,
        }) | color(pal.foreground);
        return onPaletteCanvas(
            pal, std::move(content) | border | color(pal.border));
    });

    auto modal = vimModalLayer(renderer, mode, [&] {
        aborted = true;
        screen.ExitLoopClosure()();
    });
    screen.Loop(modal);

    if (aborted) return false;
    if (!finished) return false;

    save_loaded_account();
    staged.reasoning_effort = effort;
    selectTechnique(staged, technique_ids[technique_idx]);
    cfg = std::move(staged);
    return true;
}

// -- Settings screen --------------------------------------------------------

namespace {

// Mutable state backing the settings modal. Held by shared_ptr so the pointers
// bound into the Radiobox/Input components stay valid for the whole lifetime of
// the panel (which now lives as long as the chat screen, not a single Loop).
struct SettingsState {
    int page_idx = 0;
    int provider_idx = 0;
    int technique_idx = 0;
    int theme_idx = 0;
    int model_idx = 0;
    int effort_idx = 0;
    Provider loaded_provider = Provider::OpenRouter;
    Provider effort_provider = Provider::OpenRouter;
    Config staged;
    std::string api_key, model, max_iters_s, ralph_s, plugin_iters_s, temp_s;
    std::string effort_model;
    std::vector<ReasoningEffort> effort_options;
    std::vector<std::string> page_choices{
        "Model", "Agent", "Appearance", "Shortcuts"};
    std::vector<std::string> provider_choices, technique_ids,
        technique_choices, theme_choices, model_choices, effort_choices;
};

// The settings panel as an embeddable modal: `component` is layered over the
// chat screen via ftxui::Modal, and `sync` re-reads every field from `cfg`
// (call it right before showing the panel so it reflects the live config).
struct SettingsPanel {
    Component component;
    std::function<void()> sync;
};

// Build the settings form as a modal component instead of its own
// ScreenInteractive. `mode` is the panel's own NORMAL/INSERT state (owned by
// the caller so the footer can show it). `close(changed)` is invoked when the
// user saves (changed == true) or cancels (false); the caller hides the modal
// there. `screen` is used only to pin the cursor (see runChatScreen).
SettingsPanel makeSettingsPanel(Config& cfg, ScreenInteractive& screen,
                                 ChatMode& mode,
                                 std::function<void(bool changed)> close,
                                 std::function<void()> edit_tools,
                                 std::function<void()> edit_skills,
                                 const TechniquePluginRegistry& plugins) {
    auto st = std::make_shared<SettingsState>();
    st->technique_ids = techniqueIds(plugins);
    st->technique_choices = techniqueLabels(plugins);
    for (const auto& info : knownProviders()) {
        st->provider_choices.push_back(info.label);
    }
    for (auto t : allThemes())
        st->theme_choices.push_back(themeLabel(t) + " — " +
                                    themeDescription(t));

    // Menus track externally synchronized indices, which matters when changing
    // providers replaces the model and effort lists in place.
    auto provider_menu = Menu(&st->provider_choices, &st->provider_idx);
    auto technique_menu = Menu(&st->technique_choices, &st->technique_idx);
    auto theme_menu = Menu(&st->theme_choices, &st->theme_idx);
    auto model_menu = Menu(&st->model_choices, &st->model_idx);
    auto effort_menu = Menu(&st->effort_choices, &st->effort_idx);
    auto model_input =
        Input(&st->model, "model id", paletteInputOption(cfg));
    auto custom_model_input = Maybe(model_input, [st] {
        return st->model_idx ==
               static_cast<int>(st->model_choices.size()) - 1;
    });

    MenuOption page_option = MenuOption::Horizontal();
    page_option.entries_option.transform = [&cfg](const EntryState& state) {
        const Palette& pal = palette(cfg.theme);
        Element entry = text(" " + state.label + " ");
        if (state.active) {
            entry |= color(pal.background) | bgcolor(pal.accent) | bold;
        } else {
            entry |= color(pal.muted);
            if (state.focused) entry |= inverted;
        }
        return entry;
    };
    auto page_menu = Menu(&st->page_choices, &st->page_idx, page_option);

    InputOption pw = paletteInputOption(cfg);
    pw.password = true;
    auto key_input = Input(&st->api_key, "API key", pw);
    auto iters_input =
        Input(&st->max_iters_s, "max tool iters", paletteInputOption(cfg));
    auto ralph_input = Input(&st->ralph_s, "ralph iterations",
                             paletteInputOption(cfg));
    auto plugin_iters_input = Input(&st->plugin_iters_s, "plugin iterations",
                                    paletteInputOption(cfg));
    auto temp_input =
        Input(&st->temp_s, "temperature", paletteInputOption(cfg));

    auto current_model = [st] {
        if (st->model_idx >= 0 &&
            st->model_idx < static_cast<int>(st->model_choices.size()) - 1) {
            return st->model_choices[st->model_idx];
        }
        return st->model;
    };
    auto current_provider = [st] {
        const auto& providers = knownProviders();
        if (st->provider_idx >= 0 &&
            st->provider_idx < static_cast<int>(providers.size())) {
            return providers[st->provider_idx].provider;
        }
        return st->loaded_provider;
    };
    auto refresh_effort = [st, current_model,
                           current_provider](ReasoningEffort selected) {
        st->effort_provider = current_provider();
        st->effort_model = current_model();
        refreshReasoningEffortChoices(
            st->effort_provider, st->effort_model, selected, st->effort_options,
            st->effort_choices, st->effort_idx);
    };
    auto save_account_fields = [st, current_model] {
        auto& account = providerAccount(st->staged, st->loaded_provider);
        account.api_key = st->api_key;
        account.model = current_model();
        if (account.model.empty()) {
            account.model = defaultModel(st->loaded_provider);
        }
    };
    auto load_provider_fields = [st, refresh_effort](
                                    Provider provider,
                                    ReasoningEffort selected_effort) {
        st->loaded_provider = provider;
        st->staged.provider = provider;
        const auto& account = providerAccount(st->staged, provider);
        st->api_key = account.api_key;
        st->model = account.model.empty() ? defaultModel(provider) : account.model;
        st->model_choices = knownModels(provider);
        st->model_choices.push_back("(custom model ID)");
        st->model_idx = indexOfModel(st->model_choices, st->model);
        if (st->model_idx < 0) {
            st->model_idx = static_cast<int>(st->model_choices.size()) - 1;
        }
        refresh_effort(selected_effort);
    };

    // Re-read every field from the live config. Called once now to initialise
    // and again by the caller each time the panel is reopened.
    auto sync = [st, &cfg, &plugins, load_provider_fields] {
        st->page_idx = 0;
        st->staged = cfg;
        st->provider_idx = 0;
        const auto& providers = knownProviders();
        for (size_t i = 0; i < providers.size(); ++i) {
            if (providers[i].provider == cfg.provider) {
                st->provider_idx = static_cast<int>(i);
                break;
            }
        }
        st->technique_ids = techniqueIds(plugins);
        st->technique_choices = techniqueLabels(plugins);
        const std::string technique_id =
            cfg.technique_id.empty() ? techniqueToString(cfg.technique)
                                     : cfg.technique_id;
        st->technique_idx =
            indexOfTechniqueId(st->technique_ids, technique_id);
        if (st->technique_idx < 0) {
            st->technique_ids.push_back(technique_id);
            st->technique_choices.push_back("Missing plugin: " + technique_id);
            st->technique_idx =
                static_cast<int>(st->technique_ids.size()) - 1;
        }
        st->theme_idx = 0;
        const auto& themes = allThemes();
        for (size_t i = 0; i < themes.size(); ++i)
            if (themes[i] == cfg.theme) { st->theme_idx = (int)i; break; }
        st->max_iters_s = std::to_string(cfg.max_tool_iterations);
        st->ralph_s = std::to_string(cfg.ralph_iterations);
        st->plugin_iters_s = std::to_string(cfg.plugin_iterations);
        st->temp_s = std::to_string(cfg.temperature);
        load_provider_fields(cfg.provider, cfg.reasoning_effort);
    };
    sync();

    auto save = [st, &cfg, close, current_provider, save_account_fields] {
        save_account_fields();
        st->staged.provider = current_provider();
        if (st->effort_idx >= 0 &&
            st->effort_idx < static_cast<int>(st->effort_options.size())) {
            st->staged.reasoning_effort = st->effort_options[st->effort_idx];
        }
        selectTechnique(st->staged, st->technique_ids[st->technique_idx]);
        if (st->theme_idx >= 0 &&
            st->theme_idx < static_cast<int>(allThemes().size())) {
            st->staged.theme = allThemes()[st->theme_idx];
        }
        try {
            st->staged.max_tool_iterations =
                std::max(1, std::stoi(st->max_iters_s));
        } catch (...) {
        }
        try {
            st->staged.ralph_iterations =
                std::max(1, std::stoi(st->ralph_s));
        } catch (...) {
        }
        try {
            st->staged.plugin_iterations =
                std::max(1, std::stoi(st->plugin_iters_s));
        } catch (...) {
        }
        try {
            st->staged.temperature = std::stod(st->temp_s);
        } catch (...) {
        }
        cfg = st->staged;
        close(true);
    };

    auto save_button = Button("Save changes", save);
    auto cancel_button = Button("Cancel", [close] { close(false); });
    auto tools_button = Button("Edit tools.md", std::move(edit_tools));
    auto skills_button = Button("Edit skills.md", std::move(edit_skills));

    auto model_page = Container::Vertical({
        provider_menu,
        model_menu,
        custom_model_input,
        effort_menu,
        key_input,
    });
    auto agent_page = Container::Vertical({
        technique_menu,
        iters_input,
        ralph_input,
        plugin_iters_input,
        temp_input,
        tools_button,
        skills_button,
    });
    auto appearance_page = Container::Vertical({theme_menu});
    auto shortcuts_page = Renderer([] { return text(""); });
    auto pages = Container::Tab(
        {model_page, agent_page, appearance_page, shortcuts_page},
        &st->page_idx);
    auto layout = Container::Vertical({
        page_menu,
        pages,
        Container::Horizontal({save_button, cancel_button}),
    });

    auto renderer = Renderer(layout, [=, &cfg, &screen, &mode] {
        const Palette& pal = palette(cfg.theme);
        ReasoningEffort selected = ReasoningEffort::Auto;
        if (st->effort_idx >= 0 &&
            st->effort_idx < static_cast<int>(st->effort_options.size())) {
            selected = st->effort_options[st->effort_idx];
        }
        const Provider provider = current_provider();
        if (provider != st->loaded_provider) {
            save_account_fields();
            load_provider_fields(provider, selected);
        } else if (provider != st->effort_provider ||
                   current_model() != st->effort_model) {
            refresh_effort(selected);
        }
        // Default the hardware cursor to Hidden every frame; blockCursorInput
        // re-shows a steady block only on the *focused* Input (its focusCursor
        // node runs later in the same draw). Without this reset the cursor
        // lingers — blinking — at the last input position whenever focus moves
        // to a Menu/Radiobox/Button, which renders no cursor node of its own.
        screen.SetCursor(Screen::Cursor{0, 0, Screen::Cursor::Hidden});

        Element body;
        if (st->page_idx == 0) {
            const bool custom = st->model_idx ==
                                static_cast<int>(st->model_choices.size()) - 1;
            std::vector<Element> model_fields{
                text("LLM provider") | bold | color(pal.accent),
                paragraph("Choose which account handles new turns. Each "
                          "provider retains its own key and model.") |
                    color(pal.muted),
                provider_menu->Render() | frame | size(HEIGHT, LESS_THAN, 6),
                text(""),
                text(providerLabel(provider) + " model") | bold |
                    color(pal.accent),
                paragraph("Choose a curated model or select Custom to enter "
                          "any provider-supported model ID.") |
                    color(pal.muted),
                model_menu->Render() | frame | size(HEIGHT, LESS_THAN, 9),
            };
            if (custom) {
                model_fields.push_back(text(""));
                model_fields.push_back(
                    text("Custom model ID") | bold | color(pal.accent));
                model_fields.push_back(blockCursorInput(custom_model_input));
            }
            model_fields.push_back(text(""));
            model_fields.push_back(
                text("Reasoning effort") | bold | color(pal.accent));
            model_fields.push_back(
                paragraph("Only levels supported by this model are shown. "
                          "Auto uses the model default.") |
                color(pal.muted));
            model_fields.push_back(effort_menu->Render() | frame |
                                   size(HEIGHT, LESS_THAN, 8));
            model_fields.push_back(text(""));
            model_fields.push_back(
                text(providerLabel(provider) + " API key") | bold |
                    color(pal.accent));
            model_fields.push_back(blockCursorInput(key_input));
            model_fields.push_back(
                paragraph("Stored in your config with 0600 permissions. The "
                          + providerInfo(provider).environment_variable +
                          " environment variable overrides this value.") |
                color(pal.muted));
            body = vbox(std::move(model_fields));
        } else if (st->page_idx == 1) {
            body = vbox({
                text("Agent technique") | bold | color(pal.accent),
                paragraph("Controls how each request is planned and repeated.") |
                    color(pal.muted),
                technique_menu->Render() | frame |
                    size(HEIGHT, LESS_THAN, 8),
                text(""),
                text("Execution limits") | bold | color(pal.accent),
                hbox({text("Tool-call limit        ") |
                          size(WIDTH, EQUAL, 24),
                      blockCursorInput(iters_input)}),
                text("Maximum tool calls in one standard ReAct pass.") |
                    color(pal.muted),
                hbox({text("Ralph passes           ") |
                          size(WIDTH, EQUAL, 24),
                      blockCursorInput(ralph_input)}),
                text("Goal repetitions when Ralph Loop is selected.") |
                    color(pal.muted),
                hbox({text("Plugin passes          ") |
                          size(WIDTH, EQUAL, 24),
                      blockCursorInput(plugin_iters_input)}),
                text("Maximum passes for runtime technique plugins.") |
                    color(pal.muted),
                hbox({text("Temperature            ") |
                          size(WIDTH, EQUAL, 24),
                      blockCursorInput(temp_input)}),
                text("Sampling temperature; negative values omit the setting.") |
                    color(pal.muted),
                text(""),
                text("Agent instructions") | bold | color(pal.accent),
                paragraph("Edit the workspace tool and skill instructions. "
                          "Changes apply on the next turn.") |
                    color(pal.muted),
                hbox({tools_button->Render(), text("  "),
                      skills_button->Render()}),
            });
        } else if (st->page_idx == 2) {
            body = vbox({
                text("Color theme") | bold | color(pal.accent),
                paragraph("Choose the palette used across OVI-CODE. Save "
                          "changes to apply it to the chat screen.") |
                    color(pal.muted),
                text(""),
                theme_menu->Render() | frame | size(HEIGHT, LESS_THAN, 6),
            });
        } else {
            body = vbox({
                text("Chat keyboard shortcuts") | bold | color(pal.accent),
                separator(),
                hbox({text("Insert mode") | bold | size(WIDTH, EQUAL, 18),
                      text("Type; Enter adds a line; Esc enters Normal mode")}),
                hbox({text("Move") | bold | size(WIDTH, EQUAL, 18),
                      text("h/j/k/l, w/b, 0/$, gg/G")}),
                hbox({text("Edit") | bold | size(WIDTH, EQUAL, 18),
                      text("i/a/o/O/I/A, x, dd, yy, p/P")}),
                hbox({text("Send") | bold | size(WIDTH, EQUAL, 18),
                      text("Enter from Normal mode")}),
                hbox({text("Output") | bold | size(WIDTH, EQUAL, 18),
                      text("Ctrl+W focus; j/k scroll; g/G top/bottom")}),
                hbox({text("Panels") | bold | size(WIDTH, EQUAL, 18),
                      text("z minimize input; Ctrl+W return to input")}),
                hbox({text("App") | bold | size(WIDTH, EQUAL, 18),
                      text("s settings; t technique; c clear; q quit")}),
                text(""),
                text("Settings navigation") | bold | color(pal.accent),
                separator(),
                hbox({text("Move") | bold | size(WIDTH, EQUAL, 18),
                      text("j/k/h/l or arrows; Tab changes field")}),
                hbox({text("Choose") | bold | size(WIDTH, EQUAL, 18),
                      text("Space or Enter")}),
                hbox({text("Edit field") | bold | size(WIDTH, EQUAL, 18),
                      text("i or a; Esc returns to Normal mode")}),
                hbox({text("Cancel") | bold | size(WIDTH, EQUAL, 18),
                      text("q or Esc from Normal mode")}),
            });
        }

        // Cap the whole panel to the live terminal height so it never exceeds
        // the screen (border included); the flexing yframe body absorbs the
        // squeeze while the fixed rows around it stay put.
        int max_h = std::max(8, screen.dimy());
        Element content = vbox({
                   banner(pal, "settings"),
                   text(""),
                   page_menu->Render() | center,
                   separator(),
                   body | yframe | flex,
                   separator(),
                   hbox({
                       save_button->Render(),
                       text("  "),
                       cancel_button->Render(),
                       text("  Changes apply to the next turn") |
                           color(pal.muted),
                       filler(),
                       text(mode == ChatMode::Insert ? " -- INSERT -- "
                                                     : " -- NORMAL -- ") |
                           color(mode == ChatMode::Insert ? pal.insert_mode
                                                          : pal.normal_mode) |
                           bold,
                   }),
                   text(mode == ChatMode::Insert
                             ? "INSERT · type · Esc → NORMAL"
                             : "NORMAL · j/k choose · Tab field/list · "
                               "Enter button · i/a edit · q cancel") |
                       color(pal.muted) | center,
               }) |
               color(pal.foreground);
        return onPaletteCanvas(
            pal, std::move(content) | border | color(pal.border) |
                     size(WIDTH, LESS_THAN, 96) |
                     size(HEIGHT, LESS_THAN, max_h + 1));
    });

    auto component = vimModalLayer(renderer, mode, [close] { close(false); });
    return SettingsPanel{component, sync};
}

}  // namespace

// -- Chat screen ------------------------------------------------------------

namespace {

Color kindColor(const Palette& pal, TranscriptEntry::Kind k) {
    switch (k) {
        case TranscriptEntry::Kind::User:       return pal.user_entry;
        case TranscriptEntry::Kind::Assistant:  return pal.assistant_entry;
        case TranscriptEntry::Kind::ToolCall:   return pal.tool_call;
        case TranscriptEntry::Kind::ToolResult: return pal.tool_result;
        case TranscriptEntry::Kind::System:     return pal.system_entry;
        case TranscriptEntry::Kind::Error:      return pal.error;
    }
    return pal.foreground;
}

const char* kindLabel(TranscriptEntry::Kind k) {
    switch (k) {
        case TranscriptEntry::Kind::User:       return "you";
        case TranscriptEntry::Kind::Assistant:  return "assistant";
        case TranscriptEntry::Kind::ToolCall:   return "tool →";
        case TranscriptEntry::Kind::ToolResult: return "tool ←";
        case TranscriptEntry::Kind::System:     return "system";
        case TranscriptEntry::Kind::Error:      return "error";
    }
    return "?";
}

// A transcript body that begins with "--- a/" is treated as a unified-diff
// block: per-line colorize `+`/`-`/`@` so edits read like a git diff.
Element renderDiffBody(const Palette& pal, const std::string& body) {
    std::vector<Element> rows;
    std::istringstream is(body);
    std::string line;
    while (std::getline(is, line)) {
        if (line.empty()) { rows.push_back(text(" ")); continue; }
        char c = line[0];
        if (c == '+')      rows.push_back(text(line) | color(pal.assistant_entry));
        else if (c == '-') rows.push_back(text(line) | color(pal.error));
        else if (c == '@') rows.push_back(text(line) | color(pal.accent) | bold);
        else               rows.push_back(text(line) | color(pal.muted));
    }
    if (rows.empty()) rows.push_back(text(" "));
    return vbox(std::move(rows));
}

Element renderEntry(const Palette& pal, const TranscriptEntry& e) {
    // "done" is the synthetic marker the chat loop pushes when a turn
    // finishes successfully — render it as a bold green banner.
    if (e.kind == TranscriptEntry::Kind::System && e.title == "done") {
        return vbox({
                   text(std::string("✓ done · ") + e.body) |
                       color(pal.assistant_entry) | bold,
                   text(""),
               }) |
               xflex;
    }
    auto header = text(std::string("● ") + kindLabel(e.kind) +
                       (e.title.empty() ? "" : " · " + e.title)) |
                  color(kindColor(pal, e.kind)) | bold;
    bool is_diff = e.body.rfind("--- a/", 0) == 0;
    Element body;
    if (is_diff) {
        body = renderDiffBody(pal, e.body);
    } else {
        std::vector<Element> body_lines;
        std::istringstream is(e.body);
        std::string line;
        while (std::getline(is, line)) body_lines.push_back(paragraph(line));
        if (body_lines.empty()) body_lines.push_back(text(""));
        body = vbox(body_lines) | color(pal.foreground);
    }
    return vbox({header, body, text("")}) | xflex;
}

// ---- Vim-style multi-line editor ----------------------------------------
//
// We don't reuse FTXUI's Input here: Input is single-line, paints its own
// inverted/blinking cursor (which the user reported as "white/flashing"),
// and has no notion of modes. Instead the chat screen owns a VimEditor and
// renders it manually. A hidden Input still sits in the component tree only
// to absorb focus so events reach our CatchEvent handler.
struct VimClipboard {
    std::vector<std::string> lines;
};

struct VimEditor {
    std::vector<std::string> lines{""};
    int row = 0;
    int col = 0;
    ChatMode mode = ChatMode::Insert;
    std::string pending_op;  // first key of a 2-key motion like "dd"/"gg"
    std::string clipboard_notice;

    bool isEmpty() const {
        return lines.size() == 1 && lines[0].empty();
    }

    std::string toString() const {
        std::string out;
        for (size_t i = 0; i < lines.size(); ++i) {
            out += lines[i];
            if (i + 1 < lines.size()) out += "\n";
        }
        return out;
    }

    void clearBuffer() {
        lines = {""};
        row = col = 0;
        pending_op.clear();
        clipboard_notice.clear();
    }

    void setText(const std::string& value) {
        lines.clear();
        size_t start = 0;
        while (true) {
            const size_t newline = value.find('\n', start);
            if (newline == std::string::npos) {
                lines.push_back(value.substr(start));
                break;
            }
            lines.push_back(value.substr(start, newline - start));
            start = newline + 1;
        }
        row = col = 0;
        mode = ChatMode::Normal;
        pending_op.clear();
        clipboard_notice.clear();
    }

    void clampCursor() {
        if (lines.empty()) lines.push_back("");
        if (row < 0) row = 0;
        if (row >= (int)lines.size()) row = (int)lines.size() - 1;
        int maxc = (int)lines[row].size();
        if (mode == ChatMode::Insert) {
            if (col < 0) col = 0;
            if (col > maxc) col = maxc;
        } else {
            if (col < 0) col = 0;
            if (col > std::max(0, maxc - 1)) col = std::max(0, maxc - 1);
        }
    }

    void insertString(const std::string& s) {
        lines[row].insert(col, s);
        col += (int)s.size();
    }

    void insertNewline() {
        std::string rest = lines[row].substr(col);
        lines[row].resize(col);
        lines.insert(lines.begin() + row + 1, rest);
        ++row;
        col = 0;
    }

    void backspace() {
        if (col > 0) {
            lines[row].erase(col - 1, 1);
            --col;
        } else if (row > 0) {
            col = (int)lines[row - 1].size();
            lines[row - 1] += lines[row];
            lines.erase(lines.begin() + row);
            --row;
        }
    }

    void deleteCharUnder() {
        if (col < (int)lines[row].size()) lines[row].erase(col, 1);
        clampCursor();
    }

    void deleteLine() {
        lines.erase(lines.begin() + row);
        if (lines.empty()) lines.push_back("");
        if (row >= (int)lines.size()) row = (int)lines.size() - 1;
        col = 0;
    }

    void yankLine(VimClipboard& clipboard) {
        clipboard.lines = {lines[row]};
        clipboard_notice = " [COPIED 1 LINE] ";
    }

    void pasteLine(const VimClipboard& clipboard, bool before) {
        if (clipboard.lines.empty()) {
            clipboard_notice = " [NOTHING TO PASTE] ";
            return;
        }
        const int insertion_row = before ? row : row + 1;
        lines.insert(lines.begin() + insertion_row,
                     clipboard.lines.begin(), clipboard.lines.end());
        row = insertion_row;
        col = 0;
        clipboard_notice = " [PASTED " +
                           std::to_string(clipboard.lines.size()) +
                           (clipboard.lines.size() == 1 ? " LINE] "
                                                        : " LINES] ");
        clampCursor();
    }

    void openLineBelow() {
        lines.insert(lines.begin() + row + 1, "");
        ++row;
        col = 0;
        mode = ChatMode::Insert;
    }

    void openLineAbove() {
        lines.insert(lines.begin() + row, "");
        col = 0;
        mode = ChatMode::Insert;
    }

    void moveLeft()  { if (col > 0) --col; }
    void moveRight() {
        int limit = (int)lines[row].size();
        if (mode == ChatMode::Normal && limit > 0) limit -= 1;
        if (col < limit) ++col;
    }
    void moveUp()   { if (row > 0) { --row; clampCursor(); } }
    void moveDown() { if (row + 1 < (int)lines.size()) { ++row; clampCursor(); } }
    void lineStart() { col = 0; }
    void lineEnd() {
        col = (int)lines[row].size();
        if (mode == ChatMode::Normal && col > 0) --col;
    }
    void firstLine() { row = 0; col = 0; clampCursor(); }
    void lastLine()  { row = (int)lines.size() - 1; col = 0; clampCursor(); }

    static bool isWordChar(char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    }

    void wordForward() {
        const std::string& l = lines[row];
        int n = (int)l.size();
        int c = col;
        while (c < n && isWordChar(l[c])) ++c;
        while (c < n && !isWordChar(l[c])) ++c;
        if (c >= n && row + 1 < (int)lines.size()) { ++row; col = 0; }
        else col = c;
        clampCursor();
    }

    void wordBack() {
        if (col == 0) {
            if (row > 0) { --row; col = (int)lines[row].size(); }
            clampCursor();
            return;
        }
        const std::string& l = lines[row];
        int c = col - 1;
        while (c > 0 && !isWordChar(l[c])) --c;
        while (c > 0 && isWordChar(l[c - 1])) --c;
        col = c;
        clampCursor();
    }
};

// Shared Vim editing behavior for both the chat prompt and agent configuration
// files. Return in Normal mode and app-level commands are left to the caller.
bool handleVimEditorEvent(VimEditor& editor, VimClipboard& clipboard,
                          Event event) {
    if (editor.mode == ChatMode::Insert) {
        if (event == Event::Escape) {
            editor.mode = ChatMode::Normal;
            editor.clampCursor();
            return true;
        }
        if (event == Event::Return) { editor.insertNewline(); return true; }
        if (event == Event::Backspace) { editor.backspace(); return true; }
        if (event == Event::Delete) { editor.deleteCharUnder(); return true; }
        if (event == Event::ArrowLeft)  { editor.moveLeft();  return true; }
        if (event == Event::ArrowRight) { editor.moveRight(); return true; }
        if (event == Event::ArrowUp)    { editor.moveUp();    return true; }
        if (event == Event::ArrowDown)  { editor.moveDown();  return true; }
        if (event == Event::Home)       { editor.lineStart(); return true; }
        if (event == Event::End)        { editor.lineEnd();   return true; }
        if (event == Event::Tab) {
            editor.insertString("    ");
            return true;
        }
        if (event.is_character()) {
            editor.insertString(event.character());
            return true;
        }
        return true;
    }

    if (!editor.pending_op.empty()) {
        std::string op = editor.pending_op;
        editor.pending_op.clear();
        if (op == "d" && event == Event::Character("d")) {
            editor.deleteLine();
            return true;
        }
        if (op == "g" && event == Event::Character("g")) {
            editor.firstLine();
            return true;
        }
        if (op == "y" && event == Event::Character("y")) {
            editor.yankLine(clipboard);
            return true;
        }
    }

    if (event == Event::Escape) return true;
    if (event == Event::Character("h") || event == Event::ArrowLeft) {
        editor.moveLeft(); return true;
    }
    if (event == Event::Character("l") || event == Event::ArrowRight) {
        editor.moveRight(); return true;
    }
    if (event == Event::Character("j") || event == Event::ArrowDown) {
        editor.moveDown(); return true;
    }
    if (event == Event::Character("k") || event == Event::ArrowUp) {
        editor.moveUp(); return true;
    }
    if (event == Event::Character("0") || event == Event::Home) {
        editor.lineStart(); return true;
    }
    if (event == Event::Character("$") || event == Event::End) {
        editor.lineEnd(); return true;
    }
    if (event == Event::Character("w")) { editor.wordForward(); return true; }
    if (event == Event::Character("b")) { editor.wordBack();    return true; }
    if (event == Event::Character("G")) { editor.lastLine();    return true; }
    if (event == Event::Character("p")) {
        editor.pasteLine(clipboard, false); return true;
    }
    if (event == Event::Character("P")) {
        editor.pasteLine(clipboard, true); return true;
    }

    if (event == Event::Character("x")) {
        editor.deleteCharUnder(); return true;
    }
    if (event == Event::Character("o")) {
        editor.openLineBelow(); return true;
    }
    if (event == Event::Character("O")) {
        editor.openLineAbove(); return true;
    }
    if (event == Event::Character("i")) {
        editor.mode = ChatMode::Insert;
        editor.clampCursor();
        return true;
    }
    if (event == Event::Character("a")) {
        editor.mode = ChatMode::Insert;
        if (editor.col < (int)editor.lines[editor.row].size()) ++editor.col;
        editor.clampCursor();
        return true;
    }
    if (event == Event::Character("I")) {
        editor.mode = ChatMode::Insert;
        editor.col = 0;
        return true;
    }
    if (event == Event::Character("A")) {
        editor.mode = ChatMode::Insert;
        editor.col = (int)editor.lines[editor.row].size();
        return true;
    }
    if (event == Event::Character("d")) {
        editor.pending_op = "d";
        return true;
    }
    if (event == Event::Character("g")) {
        editor.pending_op = "g";
        return true;
    }
    if (event == Event::Character("y")) {
        editor.pending_op = "y";
        return true;
    }
    return false;
}

Element renderEditor(const VimEditor& ed, const Palette& pal,
                     bool minimized = false) {
    int total = (int)ed.lines.size();
    int width_digits = 2;
    for (int n = total; n >= 100; n /= 10) ++width_digits;

    Color cursor_color = (ed.mode == ChatMode::Insert)
                             ? pal.insert_mode
                             : pal.normal_mode;

    auto renderRow = [&](int r) -> Element {
        std::string num = std::to_string(r + 1);
        while ((int)num.size() < width_digits) num.insert(num.begin(), ' ');
        bool current = (r == ed.row);
        Element gutter = current
            ? (text(num + " │ ") | color(cursor_color) | bold)
            : (text(num + " │ ") | color(pal.muted));

        const std::string& line = ed.lines[r];
        Element content;
        if (current) {
            bool past_end = ed.col >= (int)line.size();
            std::string left  = past_end ? line : line.substr(0, ed.col);
            std::string under = past_end ? " "  : line.substr(ed.col, 1);
            std::string right = past_end ? ""   : line.substr(ed.col + 1);
            auto cursor_cell = text(under) | inverted | color(cursor_color);
            content = hbox({text(left), cursor_cell, text(right), filler()});
        } else {
            content = hbox({text(line.empty() ? " " : line), filler()});
        }
        return hbox({gutter, content});
    };

    if (minimized) {
        // Single-line view: show only the row the cursor is on, plus an
        // indicator of how many lines the full buffer holds.
        std::string indicator = "  (" + std::to_string(ed.row + 1) + "/" +
                                std::to_string(total) + ")";
        return hbox({
            renderRow(ed.row),
            text(indicator) | color(pal.muted),
        });
    }

    std::vector<Element> rows;
    rows.reserve(ed.lines.size());
    for (size_t r = 0; r < ed.lines.size(); ++r) {
        rows.push_back(renderRow((int)r));
    }
    return vbox(std::move(rows));
}

struct ConfigFileEditorState {
    VimEditor editor;
    fs::path path;
    std::string original;
    std::string error;
};

struct ConfigFileEditorPanel {
    Component component;
    std::function<void(const fs::path&)> open;
};

ConfigFileEditorPanel makeConfigFileEditorPanel(
    Config& cfg, ScreenInteractive& screen, VimClipboard& clipboard,
    std::function<void(bool saved, const fs::path& path)> close) {
    auto state = std::make_shared<ConfigFileEditorState>();
    auto sink_value = std::make_shared<std::string>();
    auto sink = Input(sink_value.get(), "");

    auto save = [state, close] {
        std::error_code ec;
        fs::create_directories(state->path.parent_path(), ec);
        if (ec) {
            state->error = "Cannot create configuration directory: " +
                           ec.message();
            return;
        }
        std::ofstream file(state->path, std::ios::binary | std::ios::trunc);
        if (!file) {
            state->error = "Cannot write " + state->path.string();
            return;
        }
        const std::string contents = state->editor.toString();
        file << contents;
        file.close();
        if (!file) {
            state->error = "Failed while writing " + state->path.string();
            return;
        }
        state->original = contents;
        state->error.clear();
        close(true, state->path);
    };
    auto cancel = [state, close] { close(false, state->path); };
    auto save_button = Button("Save", save);
    auto cancel_button = Button("Cancel", cancel);
    auto layout = Container::Vertical({
        sink,
        Container::Horizontal({save_button, cancel_button}),
    });

    auto events = CatchEvent(layout, [=, &clipboard](Event event) {
        sink_value->clear();
        if (event == Event::Custom) return false;
        if (state->editor.mode == ChatMode::Normal) {
            if (event == Event::Character("s")) {
                save();
                return true;
            }
            if (event == Event::Character("q")) {
                cancel();
                return true;
            }
        }
        return handleVimEditorEvent(state->editor, clipboard, event);
    });

    auto renderer = Renderer(events, [=, &cfg, &screen] {
        const Palette& pal = palette(cfg.theme);
        screen.SetCursor(Screen::Cursor{0, 0, Screen::Cursor::Hidden});
        const bool dirty = state->editor.toString() != state->original;
        const bool insert = state->editor.mode == ChatMode::Insert;
        const Color mode_color = insert ? pal.insert_mode : pal.normal_mode;
        const std::string filename = state->path.filename().string();

        Element title = text(" " + filename + " ") |
                        color(pal.background) | bgcolor(pal.accent) | bold;
        Element editor = renderEditor(state->editor, pal) |
                         focusPositionRelative(0.f, 0.5f) | yframe | flex |
                         color(pal.foreground);
        Element editor_panel = window(std::move(title), std::move(editor)) |
                               color(pal.accent) | flex;
        Element error = state->error.empty()
                            ? text("")
                            : paragraph(state->error) | color(pal.error);
        Element content = vbox({
            banner(pal, "agent configuration"),
            text(state->path.string()) | color(pal.muted),
            error,
            editor_panel,
            hbox({
                save_button->Render(),
                text("  "),
                cancel_button->Render(),
                text(dirty ? "  modified" : "") | color(pal.normal_mode),
                text(state->editor.clipboard_notice) |
                    color(pal.assistant_entry) | bold,
                filler(),
                text(insert ? " -- INSERT -- " : " -- NORMAL -- ") |
                    color(mode_color) | bold,
            }),
            text(insert
                     ? "INSERT · type · Enter newline · Esc → NORMAL"
                     : "NORMAL · h j k l · w b · i a o O I A · x dd · "
                       "yy p P · gg G · s save · q cancel") |
                color(pal.muted) | center,
        }) | color(pal.foreground);
        return onPaletteCanvas(
            pal, std::move(content) | border | color(pal.border) |
                     size(WIDTH, LESS_THAN, 110) |
                     size(HEIGHT, LESS_THAN, std::max(10, screen.dimy())));
    });

    auto open = [state](const fs::path& path) {
        state->path = path;
        state->error.clear();
        std::string contents;
        std::ifstream file(path, std::ios::binary);
        if (file) {
            std::ostringstream buffer;
            buffer << file.rdbuf();
            if (file.bad()) {
                state->error = "Failed while reading " + path.string();
            } else {
                contents = buffer.str();
            }
        } else {
            std::error_code ec;
            if (fs::exists(path, ec)) {
                state->error = "Cannot read " + path.string();
            } else if (ec) {
                state->error = "Cannot inspect " + path.string() + ": " +
                               ec.message();
            }
        }
        state->editor.setText(contents);
        state->original = contents;
    };
    return ConfigFileEditorPanel{renderer, open};
}

}  // namespace

void runChatScreen(Config& cfg, const fs::path& workspace,
                   const fs::path& agent_config_dir,
                   const TechniquePluginRegistry& plugins) {
    AgentContext ctx{cfg, loadAgentMd(agent_config_dir), workspace, &plugins};
    std::vector<Message> messages;

    std::mutex mu;
    std::vector<TranscriptEntry> transcript;
    std::atomic<bool> busy{false};
    std::atomic<bool> stop_requested{false};
    std::thread worker;

    VimEditor editor;
    VimClipboard clipboard;
    // Ctrl+Tab toggles a single-line "minimized" view of the input panel so
    // the output area takes the whole screen while the prompt buffer is kept.
    bool input_minimized = false;

    // Which panel has keyboard control. Ctrl+W toggles it (NORMAL mode only).
    // When the output panel is focused, j/k scroll it; while the input panel
    // is focused the output auto-follows the latest entry (pinned to bottom).
    enum class FocusTarget { Input, Output };
    FocusTarget focus_target = FocusTarget::Input;
    // Relative scroll position of the output frame, 0 = top, 1 = bottom.
    float output_scroll = 1.f;
    constexpr float kScrollStep = 0.05f;
    // Ctrl+W = byte 0x17 (W is the 23rd letter). FTXUI has no Ctrl+letter
    // named constants, so build it from the raw control byte.
    const Event kCtrlW = Event::Special({23});

    // Cap transcript memory + per-frame render cost.
    constexpr size_t kTranscriptHardCap = 1000;
    constexpr size_t kTranscriptKeep    = 900;
    auto pushEntryLocked = [&](TranscriptEntry e) {
        transcript.push_back(std::move(e));
        if (transcript.size() > kTranscriptHardCap) {
            size_t drop = transcript.size() - kTranscriptKeep;
            transcript.erase(transcript.begin(),
                             transcript.begin() + drop);
            transcript.insert(
                transcript.begin(),
                TranscriptEntry{TranscriptEntry::Kind::System, "elided",
                                std::string("(") + std::to_string(drop) +
                                    " earlier entries dropped to keep the "
                                    "transcript bounded)"});
        }
    };

    {
        std::lock_guard<std::mutex> lk(mu);
        pushEntryLocked({TranscriptEntry::Kind::System, "welcome",
                          std::string("Workspace: ") + workspace.string() +
                               "  ·  Provider: " + providerLabel(cfg.provider) +
                               "  ·  Model: " + selectedModel(cfg) +
                               "  ·  Effort: " +
                               reasoningEffortLabel(effectiveReasoningEffort(
                                   cfg.provider, selectedModel(cfg),
                                   cfg.reasoning_effort)) +
                              "  ·  Technique: " +
                              selectedTechniqueLabel(cfg, plugins)});
        for (const auto& error : plugins.errors()) {
            pushEntryLocked(
                {TranscriptEntry::Kind::Error, "plugin loader", error});
        }
    }

    auto screen = ScreenInteractive::Fullscreen();

    // Settings render as a modal layered over the chat screen (ftxui::Modal)
    // rather than tearing this screen down for a separate ScreenInteractive —
    // one active screen, no alternate-buffer swap, no flash. `settings_open`
    // toggles the overlay; `settings_mode` is the panel's own NORMAL/INSERT
    // state, surfaced in its footer.
    bool settings_open = false;
    bool file_editor_open = false;
    ChatMode settings_mode = ChatMode::Normal;
    Provider provider_before_settings = cfg.provider;
    std::string model_before_settings = selectedModel(cfg);
    std::string key_before_settings =
        providerAccount(cfg, cfg.provider).api_key;
    ReasoningEffort effort_before_settings = cfg.reasoning_effort;

    // Hide the modal and, on Save, note the new config in the transcript. The
    // panel writes straight into `cfg`, so AgentContext picks it up on the next
    // turn (it is rebuilt from `cfg` in send_turn).
    auto close_settings = [&](bool changed) {
        settings_open = false;
        settings_mode = ChatMode::Normal;
        if (changed) {
            const bool account_changed =
                provider_before_settings != cfg.provider ||
                model_before_settings != selectedModel(cfg) ||
                key_before_settings !=
                    providerAccount(cfg, cfg.provider).api_key ||
                effort_before_settings != cfg.reasoning_effort;
            if (account_changed) messages.clear();
            std::lock_guard<std::mutex> lk(mu);
            pushEntryLocked(
                {TranscriptEntry::Kind::System, "settings",
                  std::string("Now using ") + providerLabel(cfg.provider) +
                      " / " + selectedModel(cfg) + " / " +
                      reasoningEffortLabel(effectiveReasoningEffort(
                          cfg.provider, selectedModel(cfg),
                          cfg.reasoning_effort)) + " effort / " +
                      selectedTechniqueLabel(cfg, plugins) +
                      (account_changed
                           ? ". Started a new conversation for this account "
                             "or model."
                           : "")});
        }
    };
    auto close_file_editor = [&](bool saved, const fs::path& path) {
        file_editor_open = false;
        if (saved) {
            std::lock_guard<std::mutex> lk(mu);
            pushEntryLocked(
                {TranscriptEntry::Kind::System, "configuration",
                 "Saved " + path.string() +
                     "; changes apply to the next turn."});
        }
    };
    auto file_editor =
        makeConfigFileEditorPanel(cfg, screen, clipboard, close_file_editor);
    auto settings = makeSettingsPanel(
        cfg, screen, settings_mode, close_settings,
        [&] {
            file_editor.open(agent_config_dir / "tools.md");
            file_editor_open = true;
        },
        [&] {
            file_editor.open(agent_config_dir / "skills.md");
            file_editor_open = true;
        }, plugins);

    auto send_turn = [&](std::string prompt) {
        if (busy.exchange(true)) return;
        if (worker.joinable()) worker.join();
        ctx.config = cfg;
        ctx.md = loadAgentMd(agent_config_dir);
        worker = std::thread([&, prompt = std::move(prompt)]() mutable {
            runTurn(ctx, messages, prompt,
                    [&](const TranscriptEntry& e) {
                        {
                            std::lock_guard<std::mutex> lk(mu);
                            pushEntryLocked(e);
                        }
                        screen.PostEvent(Event::Custom);
                        return !stop_requested.load();
                    });
            {
                std::lock_guard<std::mutex> lk(mu);
                if (!stop_requested.load())
                    pushEntryLocked({TranscriptEntry::Kind::System, "done",
                                     "Turn complete."});
            }
            busy.store(false);
            screen.PostEvent(Event::Custom);
        });
    };

    auto cycle_technique = [&] {
        const auto ids = techniqueIds(plugins);
        const std::string current =
            cfg.technique_id.empty() ? techniqueToString(cfg.technique)
                                     : cfg.technique_id;
        int idx = indexOfTechniqueId(ids, current);
        selectTechnique(cfg, ids[(idx + 1) % ids.size()]);
        std::lock_guard<std::mutex> lk(mu);
        pushEntryLocked(
            {TranscriptEntry::Kind::System, "technique",
             std::string("Switched to ") +
                 selectedTechniqueLabel(cfg, plugins)});
    };
    auto open_settings = [&] {
        if (busy.load()) {
            std::lock_guard<std::mutex> lk(mu);
            pushEntryLocked(
                {TranscriptEntry::Kind::System, "settings",
                 "Cannot open settings while a turn is in progress."});
            return;
        }
        settings.sync();
        provider_before_settings = cfg.provider;
        model_before_settings = selectedModel(cfg);
        key_before_settings = providerAccount(cfg, cfg.provider).api_key;
        effort_before_settings = cfg.reasoning_effort;
        settings_mode = ChatMode::Normal;
        settings_open = true;
    };
    auto clear_transcript = [&] {
        std::lock_guard<std::mutex> lk(mu);
        transcript.clear();
    };

    auto submit_buffer = [&] {
        if (busy.load() || editor.isEmpty()) return;
        auto p = editor.toString();
        editor.clearBuffer();
        editor.mode = ChatMode::Insert;
        send_turn(p);
    };

    // The hidden Input only exists to claim focus so events reach our
    // CatchEvent handler — we never render it.
    std::string sink_value;
    auto sink = Input(&sink_value, "");

    auto editor_events = CatchEvent(sink, [&](Event e) {
        // Ignore the screen-redraw "Custom" event.
        if (e == Event::Custom) return false;
        // Clear sink_value continuously so the hidden input never accumulates.
        sink_value.clear();

        if (editor.mode == ChatMode::Insert)
            return handleVimEditorEvent(editor, clipboard, e);

        // ---- Normal mode ----
        // When control is on the output panel, j/k scroll it and the editing
        // motions are suspended. Ctrl+W hands control back to the input panel.
        // App commands (q/s/t/c/z) stay live so they work from either panel.
        if (focus_target == FocusTarget::Output) {
            if (e == kCtrlW) {
                focus_target = FocusTarget::Input;
                return true;
            }
            if (e == Event::Character("j") || e == Event::ArrowDown) {
                output_scroll = std::min(1.f, output_scroll + kScrollStep);
                return true;
            }
            if (e == Event::Character("k") || e == Event::ArrowUp) {
                output_scroll = std::max(0.f, output_scroll - kScrollStep);
                return true;
            }
            if (e == Event::Character("g")) { output_scroll = 0.f; return true; }
            if (e == Event::Character("G")) { output_scroll = 1.f; return true; }
            if (e == Event::Character("q")) {
                screen.ExitLoopClosure()();
                return true;
            }
            if (e == Event::Character("s")) { open_settings();   return true; }
            if (e == Event::Character("t")) { cycle_technique(); return true; }
            if (e == Event::Character("c")) { clear_transcript(); return true; }
            if (e == Event::Character("z")) {
                input_minimized = !input_minimized;
                return true;
            }
            return true;  // swallow everything else while scrolling
        }

        // Ctrl+W hands keyboard control to the output panel for scrolling.
        if (e == kCtrlW) {
            focus_target = FocusTarget::Output;
            return true;
        }

        if (e == Event::Return) {
            editor.pending_op.clear();
            submit_buffer();
            return true;
        }
        if (handleVimEditorEvent(editor, clipboard, e)) return true;

        // App commands (override the vim equivalents we don't implement)
        if (e == Event::Character("q")) {
            screen.ExitLoopClosure()();
            return true;
        }
        if (e == Event::Character("s")) { open_settings();    return true; }
        if (e == Event::Character("t")) { cycle_technique();  return true; }
        if (e == Event::Character("c")) { clear_transcript(); return true; }
        if (e == Event::Character("z")) {
            input_minimized = !input_minimized;
            return true;
        }
        return true;
    });

    auto root = Container::Vertical({editor_events});

    auto renderer = Renderer(root, [&] {
        const Palette& pal = palette(cfg.theme);

        // The chat editor paints its own block cursor (an `inverted` cell), and
        // the only FTXUI component in the tree is the hidden `sink` Input, which
        // is never rendered — so *nothing* positions the real terminal cursor.
        // Left alone, FTXUI re-emits a show-cursor escape (\x1b[?25h) with the
        // Screen's default-constructed, uninitialized cursor shape every frame,
        // which surfaces as a stray blinking cursor parked in a stale cell. That
        // is the "white/flashing" artifact the VimEditor was built to avoid but
        // didn't fully kill. Pin the hardware cursor to Hidden here; no
        // focusCursor node exists in this tree to override it, so it stays gone.
        screen.SetCursor(Screen::Cursor{0, 0, Screen::Cursor::Hidden});

        std::vector<Element> rendered;
        {
            std::lock_guard<std::mutex> lk(mu);
            rendered.reserve(transcript.size());
            for (const auto& e : transcript)
                rendered.push_back(renderEntry(pal, e));
        }
        if (rendered.empty()) rendered.push_back(text(""));
        bool output_focused = (focus_target == FocusTarget::Output);
        // While the input panel is focused the output auto-follows the latest
        // entry (pinned to the bottom). When the output panel is focused the
        // user drives the scroll position with j/k. The focusPositionRelative
        // must sit *inside* the frame — `| yframe` applied last would wrap the
        // frame instead, leaving it with no focused child to scroll to (it
        // would stay stuck at the top).
        float scroll_y = output_focused ? output_scroll : 1.f;
        Element scroll =
            vbox(rendered) | focusPositionRelative(0.f, scroll_y) | yframe;

        std::string status = busy.load() ? " thinking… " : " ready ";
        Color status_color = busy.load() ? pal.busy : pal.ready;
        bool insert = (editor.mode == ChatMode::Insert);
        std::string mode_tag = insert ? " -- INSERT -- " : " -- NORMAL -- ";
        Color mode_color = insert ? pal.insert_mode : pal.normal_mode;

        Element header_content =
            hbox({
                text("OVI-CODE") | bold | color(pal.accent),
                text("  "),
                text(providerLabel(cfg.provider)) | color(pal.muted),
                text(" · "),
                text(selectedModel(cfg)) | color(pal.muted),
                text(" · "),
                text(reasoningEffortLabel(effectiveReasoningEffort(
                         cfg.provider, selectedModel(cfg),
                         cfg.reasoning_effort)) + " effort") |
                    color(pal.muted),
                text(" · "),
                text(selectedTechniqueLabel(cfg, plugins)) |
                    color(pal.technique),
                filler(),
                text(mode_tag) | color(mode_color) | bold,
                text(" "),
                text(status) | color(status_color) | bold,
            }) | color(pal.foreground);
        Element header_bar =
            std::move(header_content) |
            borderRounded | color(pal.border);

        Element output_panel =
            window(hbox({
                       text(" output ") |
                           color(output_focused ? pal.background : pal.accent) |
                           bgcolor(output_focused ? pal.accent : pal.background) |
                           bold,
                        text(output_focused ? "[SCROLL · j/k]" : "") |
                            color(pal.normal_mode),
                    }),
                   std::move(scroll) | color(pal.foreground)) |
            color(output_focused ? pal.accent : pal.border) | flex;

        Element input_body = input_minimized
            ? renderEditor(editor, pal, true)
            : renderEditor(editor, pal, false) |
                  focusPositionRelative(0.f, 1.f) | yframe;
        input_body |= color(pal.foreground);

        Element input_panel =
            window(hbox({
                       text(" input ") |
                           color(output_focused ? pal.accent : pal.background) |
                           bgcolor(output_focused ? pal.background : pal.accent) |
                           bold,
                        text(insert ? "[INSERT]" : "[NORMAL]") |
                            color(mode_color),
                        text(editor.clipboard_notice) |
                            color(pal.assistant_entry) | bold,
                        text(input_minimized ? "  [MIN]" : "") |
                            color(pal.muted),
                   }),
                   input_body) |
            color(pal.border);

        // In minimized mode pin the panel to a fixed three-row height (top
        // border + single editor row + bottom border) so the output area
        // takes the rest of the screen. Otherwise guarantee a minimum height
        // so the prompt panel is always visible (output_panel | flex absorbs
        // the rest); without this the vbox can squeeze the input to zero rows
        // on small terminals or when transcript output dominates.
        input_panel = input_minimized
            ? input_panel | size(HEIGHT, EQUAL, 3)
            : input_panel | size(HEIGHT, GREATER_THAN, 8);

        return onPaletteCanvas(
            pal, vbox({
                     header_bar,
                     output_panel,
                     input_panel,
                 }) |
                     flex);
    });

    // Keep every overlay in this screen. The file editor is layered above
    // Settings so closing it returns to the same configuration form.
    auto settings_app = Modal(renderer, settings.component, &settings_open);
    auto app = Modal(settings_app, file_editor.component, &file_editor_open);

    screen.Loop(app);
    if (worker.joinable()) {
        stop_requested.store(true);
        worker.join();
        stop_requested.store(false);
    }
}

}  // namespace ovicode
