// Chain-of-Thought (CoT) prompting skeleton in C++17.
//
// Based on arXiv:2201.11903, "Chain-of-Thought Prompting Elicits Reasoning in
// Large Language Models".  The method is intentionally simple: construct a
// few-shot prompt whose exemplars are triples
//
//   <input question, intermediate natural-language reasoning chain, final output>
//
// rather than ordinary <input, output> examples.  At inference time the model is
// asked to produce a similar reasoning chain before the final answer.  The paper
// shows this unlocks arithmetic, commonsense, and symbolic reasoning in large
// enough language models.
//
// This file implements the algorithmic framework without requiring API keys:
//   * a PromptBuilder for standard vs chain-of-thought prompts;
//   * an abstract LanguageModel interface;
//   * a deterministic ArithmeticCoTModel demo that emits step-by-step reasoning
//     for simple arithmetic word problems;
//   * final-answer extraction from generated CoT text.
//
// To use a real LLM, replace ArithmeticCoTModel with an adapter that sends the
// built prompt to your model and returns the generated text.

#include <algorithm>
#include <cctype>
#include <iostream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace cot {

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

struct Example {
    std::string question;
    std::string reasoning;   // Empty for standard prompting.
    std::string answer;
};

class PromptBuilder {
public:
    enum class Mode { Standard, ChainOfThought };

    explicit PromptBuilder(Mode mode) : mode_(mode) {}

    void addExample(Example example) {
        examples_.push_back(std::move(example));
    }

    std::string build(const std::string& question) const {
        std::ostringstream prompt;
        for (const auto& ex : examples_) {
            prompt << "Q: " << ex.question << "\n";
            prompt << "A: ";
            if (mode_ == Mode::ChainOfThought && !ex.reasoning.empty()) {
                prompt << ex.reasoning << " ";
            }
            prompt << "The answer is " << ex.answer << ".\n\n";
        }
        prompt << "Q: " << question << "\n";
        prompt << "A:";
        return prompt.str();
    }

private:
    Mode mode_;
    std::vector<Example> examples_;
};

class LanguageModel {
public:
    virtual ~LanguageModel() = default;
    virtual std::string complete(const std::string& prompt) = 0;
};

static std::vector<long long> allIntegers(const std::string& text) {
    std::vector<long long> nums;
    static const std::regex number(R"((-?\d+))");
    for (std::sregex_iterator it(text.begin(), text.end(), number), end; it != end; ++it) {
        nums.push_back(std::stoll((*it)[1].str()));
    }
    return nums;
}

static std::string extractLastQuestion(const std::string& prompt) {
    const std::string marker = "\nQ: ";
    auto pos = prompt.rfind(marker);
    if (pos == std::string::npos) {
        if (prompt.rfind("Q: ", 0) == 0) pos = 0;
        else return "";
    } else {
        pos += 1;
    }

    auto start = pos + 3;
    auto end = prompt.find("\nA:", start);
    if (end == std::string::npos) end = prompt.size();
    return trim(prompt.substr(start, end - start));
}

class ArithmeticCoTModel final : public LanguageModel {
public:
    std::string complete(const std::string& prompt) override {
        const std::string q = extractLastQuestion(prompt);
        const std::string qlow = lower(q);

        if (qlow.find("tennis") != std::string::npos && qlow.find("can") != std::string::npos) {
            auto nums = allIntegers(q);
            if (nums.size() >= 3) {
                long long start = nums[0];
                long long cans = nums[1];
                long long perCan = nums[2];
                long long bought = cans * perCan;
                long long total = start + bought;
                std::ostringstream out;
                out << " " << "Roger started with " << start << " balls. "
                    << cans << " cans of " << perCan << " tennis balls each is "
                    << bought << " tennis balls. " << start << " + " << bought
                    << " = " << total << ". The answer is " << total << ".";
                return out.str();
            }
        }

        if (qlow.find("cafeteria") != std::string::npos && qlow.find("apple") != std::string::npos) {
            auto nums = allIntegers(q);
            if (nums.size() >= 3) {
                long long had = nums[0];
                long long used = nums[1];
                long long bought = nums[2];
                long long remaining = had - used;
                long long total = remaining + bought;
                std::ostringstream out;
                out << " The cafeteria started with " << had << " apples and used "
                    << used << ", leaving " << had << " - " << used << " = "
                    << remaining << ". Then they bought " << bought << " more, so "
                    << remaining << " + " << bought << " = " << total
                    << ". The answer is " << total << ".";
                return out.str();
            }
        }

        // Generic fallback: demonstrate the CoT protocol with a simple left-to-right
        // sum of signed integers mentioned in the question.  This is not a universal
        // word-problem solver; the core paper algorithm is the prompting pattern.
        auto nums = allIntegers(q);
        if (!nums.empty()) {
            long long sum = 0;
            std::ostringstream reasoning;
            reasoning << " I will combine the numbers mentioned in the problem step by step.";
            for (long long n : nums) {
                long long old = sum;
                sum += n;
                reasoning << " " << old << " + " << n << " = " << sum << ".";
            }
            reasoning << " The answer is " << sum << ".";
            return reasoning.str();
        }

        return " I do not have enough structured information to solve this. The answer is unknown.";
    }
};

static std::string extractFinalAnswer(const std::string& completion) {
    static const std::regex answerPattern(R"((?:the\s+answer\s+is|answer\s*:)\s*([^\.\n]+))", std::regex::icase);
    std::smatch m;
    std::string::const_iterator searchStart = completion.begin();
    std::string last;
    while (std::regex_search(searchStart, completion.cend(), m, answerPattern)) {
        last = trim(m[1].str());
        searchStart = m.suffix().first;
    }
    if (!last.empty()) return last;
    return trim(completion);
}

struct Result {
    std::string prompt;
    std::string completion;
    std::string answer;
};

class ChainOfThoughtSolver {
public:
    explicit ChainOfThoughtSolver(LanguageModel& model) : model_(model) {}

    void addExemplar(const Example& example) {
        exemplars_.push_back(example);
    }

    Result solve(const std::string& question) {
        PromptBuilder builder(PromptBuilder::Mode::ChainOfThought);
        for (const auto& ex : exemplars_) builder.addExample(ex);
        Result r;
        r.prompt = builder.build(question);
        r.completion = model_.complete(r.prompt);
        r.answer = extractFinalAnswer(r.completion);
        return r;
    }

private:
    LanguageModel& model_;
    std::vector<Example> exemplars_;
};

}  // namespace cot

int main(int argc, char** argv) {
    std::string question = "The cafeteria had 23 apples. If they used 20 to make lunch and bought 6 more, how many apples do they have?";
    bool showPrompt = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--question" && i + 1 < argc) {
            question = argv[++i];
        } else if (arg == "--show-prompt") {
            showPrompt = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: ./cot [--question TEXT] [--show-prompt]\n\n"
                      << "Demonstrates chain-of-thought prompting from arXiv:2201.11903.\n"
                      << "Replace ArithmeticCoTModel with a real LLM adapter for production use.\n";
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            return 1;
        }
    }

    cot::ArithmeticCoTModel model;
    cot::ChainOfThoughtSolver solver(model);

    // The paper's key move: exemplars include intermediate reasoning before the
    // final answer, instead of only input-output pairs.
    solver.addExemplar({
        "Roger has 5 tennis balls. He buys 2 more cans of tennis balls. Each can has 3 tennis balls. How many tennis balls does he have now?",
        "Roger started with 5 balls. 2 cans of 3 tennis balls each is 6 tennis balls. 5 + 6 = 11.",
        "11"
    });
    solver.addExemplar({
        "There are 9 cars in the parking lot. 3 more cars arrive and then 2 leave. How many cars are in the lot?",
        "There are initially 9 cars. After 3 arrive, there are 9 + 3 = 12. After 2 leave, there are 12 - 2 = 10.",
        "10"
    });

    cot::Result result = solver.solve(question);

    if (showPrompt) {
        std::cout << "=== Prompt ===\n" << result.prompt << "\n\n";
    }
    std::cout << "Question: " << question << "\n";
    std::cout << "Chain of thought:" << result.completion << "\n";
    std::cout << "Final answer: " << result.answer << "\n";
    return 0;
}
