#include "ovicode/tui.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <thread>
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
// Every Color::* reference in the TUI is routed through Palette so we get
// consistent contrast on both dark and light terminals. Auto-detection
// reads $COLORFGBG (set by xterm-flavored emulators, iTerm2, kitty,
// alacritty, wezterm); when absent we default to Dark. Users can override
// permanently via the settings screen.
struct Palette {
    Color border;
    Color accent;          // app name, prompt glyph (neutral)
    Color muted;           // dim status text (provider / model)
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
        /*border         */ Color::GrayDark,
        /*accent         */ Color::CyanLight,
        /*muted          */ Color::GrayLight,
        /*insert_mode    */ Color::GreenLight,
        /*normal_mode    */ Color::YellowLight,
        /*busy           */ Color::YellowLight,
        /*ready          */ Color::GrayLight,
        /*technique      */ Color::MagentaLight,
        /*user_entry     */ Color::YellowLight,
        /*assistant_entry*/ Color::GreenLight,
        /*tool_call      */ Color::CyanLight,
        /*tool_result    */ Color::BlueLight,
        /*system_entry   */ Color::MagentaLight,
        /*error          */ Color::RedLight,
        /*logo           */ Color::CyanLight,
        /*logo_subtitle  */ Color::GrayLight,
    };
}

Palette lightPalette() {
    // Light terminals: the bright/“Light” FTXUI variants wash out against a
    // white background. Use the saturated base colors plus dark gray for
    // muted text — these stay readable on Solarized Light, Apple Light, etc.
    return Palette{
        /*border         */ Color::GrayLight,
        /*accent         */ Color::Blue,
        /*muted          */ Color::GrayDark,
        /*insert_mode    */ Color::Green,
        /*normal_mode    */ Color::Red,
        /*busy           */ Color::Magenta,
        /*ready          */ Color::GrayDark,
        /*technique      */ Color::Magenta,
        /*user_entry     */ Color::Magenta,
        /*assistant_entry*/ Color::Green,
        /*tool_call      */ Color::Blue,
        /*tool_result    */ Color::Cyan,
        /*system_entry   */ Color::GrayDark,
        /*error          */ Color::Red,
        /*logo           */ Color::Blue,
        /*logo_subtitle  */ Color::GrayDark,
    };
}

// COLORFGBG is "fg;bg" or "fg;default;bg". Background index <= 6 means a
// dark terminal, >= 7 means light. (rxvt sometimes emits "default" — we
// then assume dark.)
bool detectDarkBackground() {
    const char* v = std::getenv("COLORFGBG");
    if (!v || !*v) return true;
    std::string s = v;
    auto last = s.find_last_of(';');
    if (last == std::string::npos) return true;
    std::string bg = s.substr(last + 1);
    try {
        int n = std::stoi(bg);
        return n < 7;
    } catch (...) {
        return true;
    }
}

const Palette& palette(Theme t) {
    static const Palette dark  = darkPalette();
    static const Palette light = lightPalette();
    switch (t) {
        case Theme::Dark:  return dark;
        case Theme::Light: return light;
        case Theme::Auto:  return detectDarkBackground() ? dark : light;
    }
    return dark;
}

std::vector<std::string> providerLabels() {
    std::vector<std::string> out;
    for (const auto& p : knownProviders()) out.push_back(p.label);
    return out;
}

std::vector<std::string> techniqueLabels() {
    std::vector<std::string> out;
    for (auto t : allTechniques()) {
        out.push_back(techniqueLabel(t) + " — " + techniqueDescription(t));
    }
    return out;
}

int indexOfProvider(Provider p) {
    const auto& list = knownProviders();
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i].provider == p) return static_cast<int>(i);
    }
    return 0;
}

int indexOfTechnique(Technique t) {
    const auto& list = allTechniques();
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i] == t) return static_cast<int>(i);
    }
    return 0;
}

int indexOfModel(const std::vector<std::string>& models,
                 const std::string& m) {
    for (size_t i = 0; i < models.size(); ++i) {
        if (models[i] == m) return static_cast<int>(i);
    }
    return 0;
}

Element banner(const Palette& pal, const std::string& title) {
    return vbox({
        text("┌─ ovi-code ─ " + title + " " +
             std::string(40, '-')) |
            color(pal.accent),
    });
}

}  // namespace

// -- Setup wizard -----------------------------------------------------------

bool runSetupWizard(Config& cfg) {
    auto screen = ScreenInteractive::TerminalOutput();

    int step = 0;  // 0=provider, 1=key, 2=model, 3=technique
    int provider_idx = indexOfProvider(cfg.provider);
    int technique_idx = indexOfTechnique(cfg.technique);
    std::string api_key = cfg.api_key;
    std::string base_url;
    std::string model;
    int model_idx = 0;
    bool aborted = false;
    bool finished = false;

    auto provider_choices = providerLabels();
    auto technique_choices = techniqueLabels();

    auto provider_menu = Menu(&provider_choices, &provider_idx);

    InputOption key_opts;
    key_opts.password = true;
    auto key_input = Input(&api_key, "sk-…", key_opts);

    InputOption url_opts;
    auto url_input = Input(&base_url, "");

    std::vector<std::string> model_choices;
    auto model_menu = Menu(&model_choices, &model_idx);
    auto model_input = Input(&model, "");

    auto technique_menu = Menu(&technique_choices, &technique_idx);

    auto refreshModels = [&] {
        Provider p = knownProviders()[provider_idx].provider;
        model_choices = knownModels(p);
        model_choices.push_back("(custom — type below)");
        if (model.empty() || indexOfModel(model_choices, model) < 0)
            model = knownProviders()[provider_idx].default_model;
        model_idx = indexOfModel(model_choices, model);
        if (base_url.empty())
            base_url = knownProviders()[provider_idx].default_base_url;
    };
    refreshModels();

    auto next_button = Button("Next  ▶", [&] {
        if (step == 0) {
            cfg.provider = knownProviders()[provider_idx].provider;
            refreshModels();
            step = knownProviders()[provider_idx].needs_api_key ? 1 : 2;
        } else if (step == 1) {
            ++step;
        } else if (step == 2) {
            if (model_idx >= 0 &&
                model_idx < static_cast<int>(model_choices.size()) - 1) {
                model = model_choices[model_idx];
            }
            ++step;
        } else if (step == 3) {
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
            Container::Vertical({key_input, url_input}),
            Container::Vertical({model_menu, model_input, url_input}),
            technique_menu,
        },
        &step);

    auto root = Container::Vertical({layout, buttons});

    auto renderer = Renderer(root, [&] {
        const Palette& pal = palette(cfg.theme);
        Element body;
        if (step == 0) {
            body = vbox({
                text("1. Choose your LLM provider:") | bold | color(pal.accent),
                separator(),
                provider_menu->Render() | frame |
                    size(HEIGHT, LESS_THAN, 12),
            });
        } else if (step == 1) {
            const auto& info = knownProviders()[provider_idx];
            body = vbox({
                text("2. " + info.label + " — API key") | bold |
                    color(pal.accent),
                separator(),
                hbox({text("API key: "), key_input->Render()}),
                text(""),
                hbox({text("Base URL: "), url_input->Render()}),
                text(""),
                paragraph(
                    "Your key is saved to ~/.ovi-code/config.json "
                    "with 0600 permissions. Leave blank to skip if you set "
                    "the key via an environment variable.") |
                    color(pal.muted),
            });
        } else if (step == 2) {
            body = vbox({
                text("3. Choose a model") | bold | color(pal.accent),
                separator(),
                model_menu->Render() | frame |
                    size(HEIGHT, LESS_THAN, 10),
                text(""),
                hbox({text("Custom model: "), model_input->Render()}),
                text(""),
                hbox({text("Base URL:     "), url_input->Render()}),
            });
        } else {
            body = vbox({
                text("4. Choose default agentic technique") | bold |
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
        return vbox({
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
                text("step " + std::to_string(step + 1) + "/4") | dim,
            }),
        }) | border | color(pal.border);
    });

    screen.Loop(renderer);

    if (aborted) return false;
    if (!finished) return false;

    cfg.provider = knownProviders()[provider_idx].provider;
    if (model_idx >= 0 &&
        model_idx < static_cast<int>(model_choices.size()) - 1) {
        cfg.model = model_choices[model_idx];
    } else if (!model.empty()) {
        cfg.model = model;
    } else {
        cfg.model = knownProviders()[provider_idx].default_model;
    }
    cfg.api_key = api_key;
    cfg.base_url =
        base_url.empty() ? knownProviders()[provider_idx].default_base_url
                         : base_url;
    cfg.technique = allTechniques()[technique_idx];
    return true;
}

// -- Settings screen --------------------------------------------------------

bool runSettingsScreen(Config& cfg) {
    auto screen = ScreenInteractive::TerminalOutput();
    bool changed = false;
    bool keep_going = true;

    int provider_idx = indexOfProvider(cfg.provider);
    int technique_idx = indexOfTechnique(cfg.technique);
    int theme_idx = 0;
    {
        const auto& list = allThemes();
        for (size_t i = 0; i < list.size(); ++i) {
            if (list[i] == cfg.theme) { theme_idx = static_cast<int>(i); break; }
        }
    }
    std::string api_key = cfg.api_key;
    std::string base_url = cfg.base_url;
    std::string model = cfg.model;
    std::string max_iters_s = std::to_string(cfg.max_tool_iterations);
    std::string ralph_s = std::to_string(cfg.ralph_iterations);
    std::string temp_s = std::to_string(cfg.temperature);

    auto provider_choices = providerLabels();
    auto provider_menu = Menu(&provider_choices, &provider_idx);

    auto technique_choices = techniqueLabels();
    auto technique_menu = Menu(&technique_choices, &technique_idx);

    std::vector<std::string> theme_choices;
    for (auto t : allThemes()) {
        theme_choices.push_back(themeLabel(t) + " — " + themeDescription(t));
    }
    auto theme_menu = Menu(&theme_choices, &theme_idx);

    std::vector<std::string> model_choices = knownModels(cfg.provider);
    model_choices.push_back("(custom — type below)");
    int model_idx = indexOfModel(model_choices, cfg.model);
    auto model_menu = Menu(&model_choices, &model_idx);
    auto model_input = Input(&model, "model id");

    InputOption pw;
    pw.password = true;
    auto key_input = Input(&api_key, "API key", pw);
    auto url_input = Input(&base_url, "base url");
    auto iters_input = Input(&max_iters_s, "max tool iters");
    auto ralph_input = Input(&ralph_s, "ralph iterations");
    auto temp_input = Input(&temp_s, "temperature");

    auto save = [&] {
        cfg.provider = knownProviders()[provider_idx].provider;
        if (model_idx >= 0 &&
            model_idx < static_cast<int>(model_choices.size()) - 1) {
            cfg.model = model_choices[model_idx];
        } else if (!model.empty()) {
            cfg.model = model;
        }
        cfg.api_key = api_key;
        cfg.base_url = base_url;
        cfg.technique = allTechniques()[technique_idx];
        if (theme_idx >= 0 &&
            theme_idx < static_cast<int>(allThemes().size())) {
            cfg.theme = allThemes()[theme_idx];
        }
        try {
            cfg.max_tool_iterations = std::max(1, std::stoi(max_iters_s));
        } catch (...) {
        }
        try {
            cfg.ralph_iterations = std::max(1, std::stoi(ralph_s));
        } catch (...) {
        }
        try {
            cfg.temperature = std::stod(temp_s);
        } catch (...) {
        }
        changed = true;
        keep_going = false;
        screen.ExitLoopClosure()();
    };
    auto refresh_models = [&] {
        Provider p = knownProviders()[provider_idx].provider;
        model_choices = knownModels(p);
        model_choices.push_back("(custom — type below)");
        model_idx = indexOfModel(model_choices, model);
    };

    auto save_button = Button("Save", save);
    auto cancel_button = Button("Cancel", [&] {
        keep_going = false;
        screen.ExitLoopClosure()();
    });
    auto refresh_button = Button("Refresh models", refresh_models);

    auto layout = Container::Vertical({
        provider_menu,
        refresh_button,
        model_menu,
        model_input,
        key_input,
        url_input,
        technique_menu,
        theme_menu,
        iters_input,
        ralph_input,
        temp_input,
        Container::Horizontal({save_button, cancel_button}),
    });

    auto renderer = Renderer(layout, [&] {
        const Palette& pal = palette(cfg.theme);
        return vbox({
            banner(pal, "settings"),
            text(""),
            hbox({
                vbox({
                    text("Provider") | bold | color(pal.accent),
                    provider_menu->Render() | frame |
                        size(HEIGHT, LESS_THAN, 6),
                    text(""),
                    refresh_button->Render(),
                    text(""),
                    text("Model") | bold | color(pal.accent),
                    model_menu->Render() | frame |
                        size(HEIGHT, LESS_THAN, 6),
                    hbox({text("Custom: "), model_input->Render()}),
                }) | flex,
                separator(),
                vbox({
                    text("API key") | bold | color(pal.accent),
                    key_input->Render(),
                    text(""),
                    text("Base URL") | bold | color(pal.accent),
                    url_input->Render(),
                    text(""),
                    text("Technique") | bold | color(pal.accent),
                    technique_menu->Render() | frame |
                        size(HEIGHT, LESS_THAN, 6),
                    text(""),
                    text("Theme") | bold | color(pal.accent),
                    theme_menu->Render() | frame |
                        size(HEIGHT, LESS_THAN, 5),
                    text(""),
                    hbox({text("Max tool iters: "), iters_input->Render()}),
                    hbox({text("Ralph iters:    "), ralph_input->Render()}),
                    hbox({text("Temperature:    "), temp_input->Render()}),
                }) | flex,
            }),
            separator(),
            hbox({
                save_button->Render(),
                text("  "),
                cancel_button->Render(),
                filler(),
                text("ESC = cancel") | dim,
            }),
        }) | border | color(pal.border);
    });

    auto with_escape = CatchEvent(renderer, [&](Event e) {
        if (e == Event::Escape) {
            keep_going = false;
            screen.ExitLoopClosure()();
            return true;
        }
        return false;
    });

    while (keep_going) {
        screen.Loop(with_escape);
        break;
    }
    return changed;
}

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
    return Color::Default;
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
        body = vbox(body_lines) | dim;
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
struct VimEditor {
    std::vector<std::string> lines{""};
    int row = 0;
    int col = 0;
    ChatMode mode = ChatMode::Insert;
    std::string pending_op;  // first key of a 2-key motion like "dd"/"gg"

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
            text(indicator) | color(pal.muted) | dim,
        });
    }

    std::vector<Element> rows;
    rows.reserve(ed.lines.size());
    for (size_t r = 0; r < ed.lines.size(); ++r) {
        rows.push_back(renderRow((int)r));
    }
    return vbox(std::move(rows));
}

}  // namespace

void runChatScreen(Config& cfg, const fs::path& workspace,
                   const fs::path& agent_config_dir) {
    AgentContext ctx{cfg, loadAgentMd(agent_config_dir), workspace};
    std::vector<Message> messages;

    std::mutex mu;
    std::vector<TranscriptEntry> transcript;
    std::atomic<bool> busy{false};
    std::atomic<bool> stop_requested{false};
    std::thread worker;

    VimEditor editor;
    // Ctrl+Tab toggles a single-line "minimized" view of the input panel so
    // the output area takes the whole screen while the prompt buffer is kept.
    bool input_minimized = false;

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
                             "  ·  Model: " + cfg.model +
                             "  ·  Technique: " +
                             techniqueLabel(cfg.technique)});
        pushEntryLocked(
            {TranscriptEntry::Kind::System, "hints",
             "INSERT: type freely · Enter = newline · Esc → NORMAL.   "
             "NORMAL: Enter sends · h j k l move · w b word · 0 $ line · "
             "gg G top/bot · i a o O I A insert · x delete · dd cut line · "
             "q quit · s settings · t technique · c clear · z toggle min."});
    }

    enum class Next { Quit, OpenSettings };
    Next next = Next::Quit;

    while (true) {
    auto screen = ScreenInteractive::Fullscreen();
    next = Next::Quit;

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
        const auto& list = allTechniques();
        int idx = indexOfTechnique(cfg.technique);
        cfg.technique = list[(idx + 1) % list.size()];
        std::lock_guard<std::mutex> lk(mu);
        pushEntryLocked(
            {TranscriptEntry::Kind::System, "technique",
             std::string("Switched to ") + techniqueLabel(cfg.technique)});
    };
    auto open_settings = [&] {
        if (busy.load()) {
            std::lock_guard<std::mutex> lk(mu);
            pushEntryLocked(
                {TranscriptEntry::Kind::System, "settings",
                 "Cannot open settings while a turn is in progress."});
            return;
        }
        next = Next::OpenSettings;
        screen.ExitLoopClosure()();
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

        if (editor.mode == ChatMode::Insert) {
            if (e == Event::Escape) {
                editor.mode = ChatMode::Normal;
                editor.clampCursor();
                return true;
            }
            if (e == Event::Return) { editor.insertNewline(); return true; }
            if (e == Event::Backspace) { editor.backspace(); return true; }
            if (e == Event::Delete) { editor.deleteCharUnder(); return true; }
            if (e == Event::ArrowLeft)  { editor.moveLeft();  return true; }
            if (e == Event::ArrowRight) { editor.moveRight(); return true; }
            if (e == Event::ArrowUp)    { editor.moveUp();    return true; }
            if (e == Event::ArrowDown)  { editor.moveDown();  return true; }
            if (e == Event::Home)       { editor.lineStart(); return true; }
            if (e == Event::End)        { editor.lineEnd();   return true; }
            if (e == Event::Tab) { editor.insertString("    "); return true; }
            if (e.is_character()) {
                editor.insertString(e.character());
                return true;
            }
            return true;
        }

        // ---- Normal mode ----
        // Two-key motions: pending_op stores the leader.
        if (!editor.pending_op.empty()) {
            std::string op = editor.pending_op;
            editor.pending_op.clear();
            if (op == "d" && e == Event::Character("d")) {
                editor.deleteLine();
                return true;
            }
            if (op == "g" && e == Event::Character("g")) {
                editor.firstLine();
                return true;
            }
            // Any other key: fall through and re-process below.
        }

        if (e == Event::Return) { submit_buffer(); return true; }
        if (e == Event::Escape) { return true; }

        // Cursor motions
        if (e == Event::Character("h") || e == Event::ArrowLeft) {
            editor.moveLeft(); return true;
        }
        if (e == Event::Character("l") || e == Event::ArrowRight) {
            editor.moveRight(); return true;
        }
        if (e == Event::Character("j") || e == Event::ArrowDown) {
            editor.moveDown(); return true;
        }
        if (e == Event::Character("k") || e == Event::ArrowUp) {
            editor.moveUp(); return true;
        }
        if (e == Event::Character("0") || e == Event::Home) {
            editor.lineStart(); return true;
        }
        if (e == Event::Character("$") || e == Event::End) {
            editor.lineEnd(); return true;
        }
        if (e == Event::Character("w")) { editor.wordForward(); return true; }
        if (e == Event::Character("b")) { editor.wordBack();    return true; }
        if (e == Event::Character("G")) { editor.lastLine();    return true; }

        // Editing
        if (e == Event::Character("x")) { editor.deleteCharUnder(); return true; }
        if (e == Event::Character("o")) { editor.openLineBelow();   return true; }
        if (e == Event::Character("O")) { editor.openLineAbove();   return true; }
        if (e == Event::Character("i")) {
            editor.mode = ChatMode::Insert; editor.clampCursor(); return true;
        }
        if (e == Event::Character("a")) {
            editor.mode = ChatMode::Insert;
            if (editor.col < (int)editor.lines[editor.row].size()) ++editor.col;
            editor.clampCursor();
            return true;
        }
        if (e == Event::Character("I")) {
            editor.mode = ChatMode::Insert; editor.col = 0; return true;
        }
        if (e == Event::Character("A")) {
            editor.mode = ChatMode::Insert;
            editor.col = (int)editor.lines[editor.row].size();
            return true;
        }

        // Leaders for 2-key motions
        if (e == Event::Character("d")) { editor.pending_op = "d"; return true; }
        if (e == Event::Character("g")) { editor.pending_op = "g"; return true; }

        // App commands (override the vim equivalents we don't implement)
        if (e == Event::Character("q")) {
            next = Next::Quit;
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

        std::vector<Element> rendered;
        {
            std::lock_guard<std::mutex> lk(mu);
            rendered.reserve(transcript.size());
            for (const auto& e : transcript)
                rendered.push_back(renderEntry(pal, e));
        }
        if (rendered.empty()) rendered.push_back(text(""));
        // Pin to bottom so new output is always visible.
        Element scroll =
            vbox(rendered) | yframe | focusPositionRelative(0.f, 1.f);

        std::string status = busy.load() ? " thinking… " : " ready ";
        Color status_color = busy.load() ? pal.busy : pal.ready;
        bool insert = (editor.mode == ChatMode::Insert);
        std::string mode_tag = insert ? " -- INSERT -- " : " -- NORMAL -- ";
        Color mode_color = insert ? pal.insert_mode : pal.normal_mode;

        std::string footer =
            insert
                ? "INSERT  ·  Enter newline  ·  Esc → NORMAL"
                : "NORMAL  ·  Enter send  ·  h j k l move  ·  w b word  ·  "
                  "i a o O I A insert  ·  x dd  ·  z min  ·  q s t c";

        Element header_bar =
            hbox({
                text("ovi-code") | bold | color(pal.accent),
                text("  "),
                text(providerLabel(cfg.provider)) | color(pal.muted),
                text(" · "),
                text(cfg.model) | color(pal.muted),
                text(" · "),
                text(techniqueLabel(cfg.technique)) | color(pal.technique),
                filler(),
                text(mode_tag) | color(mode_color) | bold,
                text(" "),
                text(status) | color(status_color) | bold,
            }) |
            borderRounded | color(pal.border);

        Element output_panel =
            window(text(" output ") | color(pal.accent) | bold,
                   scroll) |
            color(pal.border) | flex;

        Element input_body = input_minimized
            ? renderEditor(editor, pal, true)
            : renderEditor(editor, pal, false) | yframe |
                  focusPositionRelative(0.f, 1.f);

        Element input_panel =
            window(hbox({
                       text(" input ") | color(pal.accent) | bold,
                       text(insert ? "[INSERT]" : "[NORMAL]") |
                           color(mode_color),
                       text(input_minimized ? "  [MIN]" : "") |
                           color(pal.muted),
                   }),
                   input_body) |
            color(pal.border);

        // In minimized mode pin the panel to a fixed three-row height (top
        // border + single editor row + bottom border) so the output area
        // takes the rest of the screen. Otherwise let it flex.
        input_panel = input_minimized
            ? input_panel | size(HEIGHT, EQUAL, 3)
            : input_panel | flex;

        return vbox({
                   header_bar,
                   output_panel,
                   input_panel,
                   text(footer) | color(pal.muted) | dim | center,
               }) |
               flex;
    });

    screen.Loop(renderer);
    if (worker.joinable()) {
        stop_requested.store(true);
        worker.join();
        stop_requested.store(false);
    }
    if (next == Next::OpenSettings) {
        runSettingsScreen(cfg);
        std::lock_guard<std::mutex> lk(mu);
        pushEntryLocked(
            {TranscriptEntry::Kind::System, "settings",
             std::string("Now using ") + providerLabel(cfg.provider) + " / " +
                 cfg.model + " / " + techniqueLabel(cfg.technique)});
        continue;
    }
    break;
    }
}

}  // namespace ovicode
