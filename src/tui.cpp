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

// FTXUI v5 has no Event::CtrlX named constants. Ctrl+letter is the
// corresponding control character (A=1, …, Z=26) wrapped in Event::Special.
const Event kCtrlL = Event::Special({0x0c});
const Event kCtrlQ = Event::Special({0x11});
const Event kCtrlS = Event::Special({0x13});
const Event kCtrlT = Event::Special({0x14});

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

Element banner(const std::string& title) {
    return vbox({
        text("┌─ ovi-code ─ " + title + " " +
             std::string(40, '-')) |
            color(Color::Cyan),
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
        Element body;
        if (step == 0) {
            body = vbox({
                text("1. Choose your LLM provider:") | bold,
                separator(),
                provider_menu->Render() | frame |
                    size(HEIGHT, LESS_THAN, 12),
            });
        } else if (step == 1) {
            const auto& info = knownProviders()[provider_idx];
            body = vbox({
                text("2. " + info.label + " — API key") | bold,
                separator(),
                hbox({text("API key: "), key_input->Render()}),
                text(""),
                hbox({text("Base URL: "), url_input->Render()}),
                text(""),
                paragraph(
                    "Your key is saved to ~/.config/ovi-code/config.json "
                    "with 0600 permissions. Leave blank to skip if you set "
                    "the key via an environment variable."),
            });
        } else if (step == 2) {
            body = vbox({
                text("3. Choose a model") | bold,
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
                text("4. Choose default agentic technique") | bold,
                separator(),
                technique_menu->Render() | frame |
                    size(HEIGHT, LESS_THAN, 12),
                text(""),
                paragraph(
                    "You can switch techniques any time from the chat screen "
                    "with Ctrl+T."),
            });
        }
        return vbox({
            banner("setup"),
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
        }) | border;
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
        iters_input,
        ralph_input,
        temp_input,
        Container::Horizontal({save_button, cancel_button}),
    });

    auto renderer = Renderer(layout, [&] {
        return vbox({
            banner("settings"),
            text(""),
            hbox({
                vbox({
                    text("Provider") | bold,
                    provider_menu->Render() | frame |
                        size(HEIGHT, LESS_THAN, 6),
                    text(""),
                    refresh_button->Render(),
                    text(""),
                    text("Model") | bold,
                    model_menu->Render() | frame |
                        size(HEIGHT, LESS_THAN, 6),
                    hbox({text("Custom: "), model_input->Render()}),
                }) | flex,
                separator(),
                vbox({
                    text("API key") | bold,
                    key_input->Render(),
                    text(""),
                    text("Base URL") | bold,
                    url_input->Render(),
                    text(""),
                    text("Technique") | bold,
                    technique_menu->Render() | frame |
                        size(HEIGHT, LESS_THAN, 6),
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
        }) | border;
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

Color kindColor(TranscriptEntry::Kind k) {
    switch (k) {
        case TranscriptEntry::Kind::User:       return Color::Yellow;
        case TranscriptEntry::Kind::Assistant:  return Color::Green;
        case TranscriptEntry::Kind::ToolCall:   return Color::Cyan;
        case TranscriptEntry::Kind::ToolResult: return Color::Blue;
        case TranscriptEntry::Kind::System:     return Color::Magenta;
        case TranscriptEntry::Kind::Error:      return Color::Red;
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

Element renderEntry(const TranscriptEntry& e) {
    auto header = text(std::string("● ") + kindLabel(e.kind) +
                       (e.title.empty() ? "" : " · " + e.title)) |
                  color(kindColor(e.kind)) | bold;
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

    transcript.push_back({TranscriptEntry::Kind::System, "welcome",
                          std::string("Workspace: ") + workspace.string() +
                              "  ·  Provider: " + providerLabel(cfg.provider) +
                              "  ·  Model: " + cfg.model +
                              "  ·  Technique: " + techniqueLabel(cfg.technique)});
    transcript.push_back(
        {TranscriptEntry::Kind::System, "hints",
         "Enter = send  ·  Ctrl+S = settings  ·  Ctrl+T = cycle technique  "
         "·  Ctrl+L = clear  ·  Ctrl+Q = quit"});

    enum class Next { Quit, OpenSettings };
    Next next = Next::Quit;

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
                            transcript.push_back(e);
                        }
                        screen.PostEvent(Event::Custom);
                        return !stop_requested.load();
                    });
            busy.store(false);
            screen.PostEvent(Event::Custom);
        });
    };

    auto input_with_events = CatchEvent(input, [&](Event e) {
        if (e == Event::Return) {
            if (busy.load() || input_value.empty()) return true;
            auto p = input_value;
            input_value.clear();
            send_turn(p);
            return true;
        }
        if (e == kCtrlQ) {
            next = Next::Quit;
            screen.ExitLoopClosure()();
            return true;
        }
        if (e == kCtrlL) {
            std::lock_guard<std::mutex> lk(mu);
            transcript.clear();
            return true;
        }
        if (e == kCtrlT) {
            const auto& list = allTechniques();
            int idx = indexOfTechnique(cfg.technique);
            cfg.technique = list[(idx + 1) % list.size()];
            std::lock_guard<std::mutex> lk(mu);
            transcript.push_back(
                {TranscriptEntry::Kind::System, "technique",
                 std::string("Switched to ") + techniqueLabel(cfg.technique)});
            return true;
        }
        if (e == kCtrlS) {
            if (busy.load()) {
                std::lock_guard<std::mutex> lk(mu);
                transcript.push_back(
                    {TranscriptEntry::Kind::System, "settings",
                     "Cannot open settings while a turn is in progress."});
                return true;
            }
            next = Next::OpenSettings;
            screen.ExitLoopClosure()();
            return true;
        }
        return false;
    });

    auto root = Container::Vertical({input_with_events});

    auto renderer = Renderer(root, [&] {
        std::vector<Element> rendered;
        {
            std::lock_guard<std::mutex> lk(mu);
            for (const auto& e : transcript) rendered.push_back(renderEntry(e));
        }
        if (rendered.empty()) rendered.push_back(text(""));
        Element scroll =
            vbox(rendered) | yframe | focusPositionRelative(0.f, 1.f) | yflex;

        std::string status = busy.load() ? " thinking… " : " ready ";
        Color status_color = busy.load() ? Color::Yellow : Color::GrayLight;

        return vbox({
                   hbox({
                       text("ovi-code") | bold | color(Color::Cyan),
                       text("  "),
                       text(providerLabel(cfg.provider)) | color(Color::GrayLight),
                       text(" · "),
                       text(cfg.model) | color(Color::GrayLight),
                       text(" · "),
                       text(techniqueLabel(cfg.technique)) |
                           color(Color::Magenta),
                       filler(),
                       text(status) | color(status_color) | bold,
                   }) |
                       borderRounded,
                   scroll | border | flex,
                   hbox({
                       text("> ") | color(Color::Cyan),
                       input->Render() | flex,
                   }) | border,
                   text("Enter send · Ctrl+S settings · Ctrl+T technique "
                        "· Ctrl+L clear · Ctrl+Q quit") |
                       dim | center,
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
        transcript.push_back(
            {TranscriptEntry::Kind::System, "settings",
             std::string("Now using ") + providerLabel(cfg.provider) + " / " +
                 cfg.model + " / " + techniqueLabel(cfg.technique)});
        continue;
    }
    break;
    }
}

}  // namespace ovicode
