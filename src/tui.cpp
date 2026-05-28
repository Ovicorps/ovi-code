#include "ovicode/tui.hpp"

#include <atomic>
#include <chrono>
#include <memory>
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

Element splashLogo(const Palette& pal) {
    return vbox({
        text("    ██████╗ ██╗   ██╗██╗       ██████╗ ██████╗ ██████╗ ███████╗") | color(pal.logo),
        text("   ██╔═══██╗██║   ██║██║      ██╔════╝██╔═══██╗██╔══██╗██╔════╝") | color(pal.logo),
        text("   ██║   ██║██║   ██║██║█████╗██║     ██║   ██║██║  ██║█████╗  ") | color(pal.logo),
        text("   ██║   ██║╚██╗ ██╔╝██║╚════╝██║     ██║   ██║██║  ██║██╔══╝  ") | color(pal.logo),
        text("   ╚██████╔╝ ╚████╔╝ ██║      ╚██████╗╚██████╔╝██████╔╝███████╗") | color(pal.logo),
        text("    ╚═════╝   ╚═══╝  ╚═╝       ╚═════╝ ╚═════╝ ╚═════╝ ╚══════╝") | color(pal.logo),
        text("         Read · Edit · Reason · Refactor · Run") |
            color(pal.logo_subtitle) | dim,
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
        case TranscriptEntry::Kind::ToolCall:   return "tool→";
        case TranscriptEntry::Kind::ToolResult: return "tool←";
        case TranscriptEntry::Kind::System:     return "system";
        case TranscriptEntry::Kind::Error:      return "error";
    }
    return "?";
}

Element renderEntry(const Palette& pal, const TranscriptEntry& e) {
    auto header = text(std::string("● ") + kindLabel(e.kind) +
                       (e.title.empty() ? "" : " · " + e.title)) |
                  color(kindColor(pal, e.kind)) | bold;
    std::vector<Element> body_lines;
    std::istringstream is(e.body);
    std::string line;
    while (std::getline(is, line)) {
        body_lines.push_back(paragraph(line));
    }
    if (body_lines.empty()) body_lines.push_back(text(""));
    return vbox({
               header,
               vbox(body_lines) | dim,
               text(""),
           }) |
           xflex;
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
    std::string input_value;
    std::thread worker;

    // Cap transcript memory + per-frame render cost. When we exceed the
    // hard cap, trim back to the keep target in one shot to amortize the
    // vector::erase. Caller must hold `mu`.
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
             "Insert mode (default): type, Enter sends, Esc → Normal mode.  "
             "Normal mode: i/a/Enter insert · q quit · s settings · t "
             "technique · c clear."});
    }

    enum class Next { Quit, OpenSettings };
    Next next = Next::Quit;
    ChatMode mode = ChatMode::Insert;

    while (true) {
    auto screen = ScreenInteractive::Fullscreen();
    next = Next::Quit;

    auto input = Input(&input_value, "ask, edit, or ship code…");

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

    auto input_with_events = CatchEvent(input, [&](Event e) {
        if (mode == ChatMode::Normal) {
            // Single-letter commands; swallow every other key so the input
            // stays untouched while in Normal mode.
            if (e == Event::Character("i") || e == Event::Character("a") ||
                e == Event::Return) {
                mode = ChatMode::Insert;
                return true;
            }
            if (e == Event::Character("q")) {
                next = Next::Quit;
                screen.ExitLoopClosure()();
                return true;
            }
            if (e == Event::Character("s")) { open_settings();     return true; }
            if (e == Event::Character("t")) { cycle_technique();   return true; }
            if (e == Event::Character("c")) { clear_transcript();  return true; }
            return true;  // eat the event regardless
        }

        // Insert mode.
        if (e == Event::Escape) {
            mode = ChatMode::Normal;
            return true;
        }
        if (e == Event::Return) {
            if (busy.load() || input_value.empty()) return true;
            auto p = input_value;
            input_value.clear();
            send_turn(p);
            return true;
        }
        return false;
    });

    auto root = Container::Vertical({input_with_events});

    auto renderer = Renderer(root, [&] {
        const Palette& pal = palette(cfg.theme);
        bool wide_enough = Terminal::Size().dimx >= 72;

        std::vector<Element> rendered;
        {
            std::lock_guard<std::mutex> lk(mu);
            rendered.reserve(transcript.size());
            for (const auto& e : transcript)
                rendered.push_back(renderEntry(pal, e));
        }
        if (rendered.empty()) rendered.push_back(text(""));
        Element scroll =
            vbox(rendered) | yframe | focusPositionRelative(0.f, 1.f) | yflex;

        std::string status = busy.load() ? " thinking… " : " ready ";
        Color status_color = busy.load() ? pal.busy : pal.ready;
        bool insert = (mode == ChatMode::Insert);
        std::string mode_tag = insert ? " -- INSERT -- " : " -- NORMAL -- ";
        Color mode_color = insert ? pal.insert_mode : pal.normal_mode;
        const char* prompt_glyph = insert ? "> " : ": ";

        std::string footer =
            insert
                ? "INSERT  ·  Enter send  ·  Esc → Normal mode"
                : "NORMAL  ·  i/a/Enter insert  ·  q quit  ·  s settings  "
                  "·  t technique  ·  c clear";

        std::vector<Element> rows;
        if (wide_enough) rows.push_back(splashLogo(pal) | center);
        rows.push_back(
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
            borderRounded | color(pal.border));
        rows.push_back(scroll | border | color(pal.border) | flex);
        rows.push_back(
            hbox({
                text(prompt_glyph) | color(mode_color),
                input->Render() | flex,
            }) |
            border | color(pal.border));
        rows.push_back(text(footer) | color(pal.muted) | dim | center);

        return vbox(std::move(rows)) | flex;
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
