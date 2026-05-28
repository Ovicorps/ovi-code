// ReAct (Reason + Act) agent skeleton in C++17.
//
// Based on arXiv:2210.03629, "ReAct: Synergizing Reasoning and Acting in
// Language Models".  The paper describes an agent whose trajectory interleaves
// free-form language thoughts with task-specific external actions:
//
//   Thought: reason over the current context; no environment side effect.
//   Action:  call an external tool/environment, receive an Observation.
//   Finish:  terminate with an answer.
//
// For knowledge-intensive QA, the paper uses dense Thought -> Action ->
// Observation steps with actions such as search[entity], lookup[string], and
// finish[answer].  This file implements that loop generically.  It includes a
// tiny local Wiki-like environment and a scripted model so the algorithm can be
// compiled and run without network credentials.  To use a real LLM, implement
// LanguageModel::generateNext() by calling your model API and keep the prompt
// format unchanged.

#include <algorithm>
#include <cctype>
#include <iostream>
#include <map>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace react {

static std::string trim(const std::string& s) {
    const auto begin = s.find_first_not_of(" \t\n\r");
    if (begin == std::string::npos) return "";
    const auto end = s.find_last_not_of(" \t\n\r");
    return s.substr(begin, end - begin + 1);
}

static std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

static bool containsCaseInsensitive(const std::string& haystack, const std::string& needle) {
    return lower(haystack).find(lower(needle)) != std::string::npos;
}

enum class StepKind { Thought, Action, Observation, Finish };

struct Step {
    StepKind kind;
    std::string text;
};

static std::string label(StepKind kind) {
    switch (kind) {
        case StepKind::Thought: return "Thought";
        case StepKind::Action: return "Action";
        case StepKind::Observation: return "Observation";
        case StepKind::Finish: return "Finish";
    }
    return "Unknown";
}

class Trajectory {
public:
    explicit Trajectory(std::string task) : task_(std::move(task)) {}

    void add(StepKind kind, std::string text) {
        steps_.push_back({kind, std::move(text)});
    }

    const std::string& task() const { return task_; }
    const std::vector<Step>& steps() const { return steps_; }

    std::string toPrompt() const {
        std::ostringstream out;
        out << "Question: " << task_ << "\n";
        for (const auto& step : steps_) {
            out << label(step.kind) << ": " << step.text << "\n";
        }
        if (steps_.empty() || steps_.back().kind == StepKind::Observation) {
            out << "Thought:";
        } else if (steps_.back().kind == StepKind::Thought) {
            out << "Action:";
        }
        return out.str();
    }

    void print(std::ostream& os) const {
        os << "Question: " << task_ << "\n";
        for (const auto& step : steps_) {
            os << label(step.kind) << ": " << step.text << "\n";
        }
    }

private:
    std::string task_;
    std::vector<Step> steps_;
};

struct ActionCall {
    std::string name;
    std::string argument;
};

static ActionCall parseAction(const std::string& raw) {
    static const std::regex bracketed(R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s*\[(.*)\]\s*$)");
    static const std::regex colon(R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s*:\s*(.*)\s*$)");
    std::smatch m;
    if (std::regex_match(raw, m, bracketed) || std::regex_match(raw, m, colon)) {
        return {lower(m[1].str()), trim(m[2].str())};
    }
    throw std::runtime_error("Could not parse action: " + raw);
}

class Environment {
public:
    virtual ~Environment() = default;
    virtual std::string observe(const ActionCall& action) = 0;
};

class LanguageModel {
public:
    virtual ~LanguageModel() = default;

    // Return exactly one next item, including its role:
    //   "Thought: ..."
    //   "Action: search[...]/lookup[...]/finish[...]"
    virtual std::string generateNext(const std::string& prompt) = 0;
};

class MiniWikiEnvironment final : public Environment {
public:
    explicit MiniWikiEnvironment(std::map<std::string, std::string> pages)
        : pages_(std::move(pages)) {}

    std::string observe(const ActionCall& action) override {
        if (action.name == "search") return search(action.argument);
        if (action.name == "lookup") return lookup(action.argument);
        return "Unknown action '" + action.name + "'. Available: search[x], lookup[x], finish[x].";
    }

private:
    std::string search(const std::string& entity) {
        currentPage_.clear();
        for (const auto& [title, body] : pages_) {
            if (lower(title) == lower(entity) || containsCaseInsensitive(title, entity)) {
                currentPage_ = title;
                lookupIndex_ = 0;
                return title + ": " + firstSentences(body, 5);
            }
        }

        std::ostringstream suggestions;
        suggestions << "Could not find [" << entity << "]. Similar pages:";
        int count = 0;
        for (const auto& [title, _] : pages_) {
            if (count++ == 5) break;
            suggestions << (count == 1 ? " " : ", ") << title;
        }
        return suggestions.str();
    }

    std::string lookup(const std::string& key) {
        if (currentPage_.empty()) return "No page selected. Use search[entity] first.";
        const auto& body = pages_.at(currentPage_);
        auto sentences = splitSentences(body);
        for (std::size_t i = lookupIndex_; i < sentences.size(); ++i) {
            if (containsCaseInsensitive(sentences[i], key)) {
                lookupIndex_ = i + 1;
                return sentences[i];
            }
        }
        lookupIndex_ = 0;
        return "No more results for [" + key + "] on page " + currentPage_ + ".";
    }

    static std::vector<std::string> splitSentences(const std::string& text) {
        std::vector<std::string> result;
        std::string cur;
        for (char c : text) {
            cur.push_back(c);
            if (c == '.' || c == '?' || c == '!') {
                result.push_back(trim(cur));
                cur.clear();
            }
        }
        if (!trim(cur).empty()) result.push_back(trim(cur));
        return result;
    }

    static std::string firstSentences(const std::string& text, std::size_t n) {
        auto sentences = splitSentences(text);
        std::ostringstream out;
        for (std::size_t i = 0; i < sentences.size() && i < n; ++i) {
            if (i) out << ' ';
            out << sentences[i];
        }
        return out.str();
    }

    std::map<std::string, std::string> pages_;
    std::string currentPage_;
    std::size_t lookupIndex_ = 0;
};

class ScriptedReActModel final : public LanguageModel {
public:
    explicit ScriptedReActModel(std::vector<std::string> script)
        : script_(std::move(script)) {}

    std::string generateNext(const std::string&) override {
        if (cursor_ >= script_.size()) {
            return "Action: finish[unknown]";
        }
        return script_[cursor_++];
    }

private:
    std::vector<std::string> script_;
    std::size_t cursor_ = 0;
};

class Agent {
public:
    Agent(LanguageModel& model, Environment& env, int maxSteps)
        : model_(model), env_(env), maxSteps_(maxSteps) {}

    Trajectory run(const std::string& task) {
        Trajectory trajectory(task);

        for (int step = 0; step < maxSteps_; ++step) {
            const std::string modelOutput = trim(model_.generateNext(trajectory.toPrompt()));
            const ParsedModelOutput parsed = parseModelOutput(modelOutput);

            if (parsed.kind == StepKind::Thought) {
                trajectory.add(StepKind::Thought, parsed.text);
                continue;
            }

            ActionCall action = parseAction(parsed.text);
            if (action.name == "finish") {
                trajectory.add(StepKind::Finish, action.argument);
                return trajectory;
            }

            trajectory.add(StepKind::Action, parsed.text);
            trajectory.add(StepKind::Observation, env_.observe(action));
        }

        trajectory.add(StepKind::Finish, "failed: max steps exceeded");
        return trajectory;
    }

private:
    struct ParsedModelOutput {
        StepKind kind;
        std::string text;
    };

    static ParsedModelOutput parseModelOutput(const std::string& output) {
        const std::string low = lower(output);
        if (low.rfind("thought:", 0) == 0) {
            return {StepKind::Thought, trim(output.substr(8))};
        }
        if (low.rfind("action:", 0) == 0) {
            return {StepKind::Action, trim(output.substr(7))};
        }
        if (low.rfind("finish:", 0) == 0) {
            return {StepKind::Action, "finish[" + trim(output.substr(7)) + "]"};
        }
        // ReAct usually alternates thought/action. If the model omits the label,
        // treat bracketed commands as actions and other text as thoughts.
        if (std::regex_search(output, std::regex(R"([A-Za-z_][A-Za-z0-9_]*\s*\[.*\])"))) {
            return {StepKind::Action, output};
        }
        return {StepKind::Thought, output};
    }

    LanguageModel& model_;
    Environment& env_;
    int maxSteps_;
};

}  // namespace react

int main(int argc, char** argv) {
    int maxSteps = 8;
    std::string question = "What conference published the ReAct paper?";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--max-steps" && i + 1 < argc) {
            maxSteps = std::stoi(argv[++i]);
        } else if (arg == "--question" && i + 1 < argc) {
            question = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: ./react [--max-steps N] [--question TEXT]\n\n"
                      << "Demonstrates the ReAct loop from arXiv:2210.03629.\n"
                      << "Replace ScriptedReActModel with a real LLM adapter for production use.\n";
            return 0;
        }
    }

    std::map<std::string, std::string> pages = {
        {"ReAct", "ReAct: Synergizing Reasoning and Acting in Language Models is a paper by Shunyu Yao, Jeffrey Zhao, Dian Yu, Nan Du, Izhak Shafran, Karthik Narasimhan, and Yuan Cao. It explores prompting language models to generate reasoning traces and task-specific actions in an interleaved manner. The paper was published as a conference paper at ICLR 2023."},
        {"ICLR", "The International Conference on Learning Representations is a machine learning conference. ICLR includes peer reviewed conference papers about representation learning and deep learning."}
    };

    react::MiniWikiEnvironment env(std::move(pages));
    react::ScriptedReActModel model({
        "Thought: I need to retrieve information about the ReAct paper and identify the venue.",
        "Action: search[ReAct]",
        "Thought: The observation should mention the publication venue, so I can finish.",
        "Action: finish[ICLR 2023]"
    });

    react::Agent agent(model, env, maxSteps);
    react::Trajectory trajectory = agent.run(question);
    trajectory.print(std::cout);
    return 0;
}
