// Retrieval-Augmented Generation (RAG) skeleton in C++17.
//
// Based on arXiv:2005.11401, "Retrieval-Augmented Generation for
// Knowledge-Intensive NLP Tasks".  The paper combines:
//   * a retriever p_eta(z | x), implemented with DPR + MIPS over a document index;
//   * a seq2seq generator p_theta(y_i | x, z, y_1:i-1), implemented with BART;
//   * marginalization over top-K latent documents.
//
// The two central variants are:
//   RAG-Sequence: p(y|x) ~= sum_z p_eta(z|x) * p_theta(y|x,z)
//                 Same latent document explains the whole output sequence.
//   RAG-Token:    p(y|x) ~= prod_i sum_z p_eta(z|x) * p_theta(y_i|x,z,y_<i)
//                 A different latent document may explain each generated token.
//
// This implementation is a compact, dependency-free framework that compiles and
// runs without neural model credentials.  It uses TF-IDF/cosine retrieval as a
// stand-in for DPR/MIPS and a deterministic extractive generator as a stand-in
// for BART.  The interfaces and scoring equations mirror the paper, so a real
// embedding index and seq2seq model can be plugged in later.

#include <algorithm>
#include <cmath>
#include <cctype>
#include <iostream>
#include <map>
#include <numeric>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rag {

static std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\n\r");
    if (b == std::string::npos) return "";
    const auto e = s.find_last_not_of(" \t\n\r");
    return s.substr(b, e - b + 1);
}

static std::vector<std::string> tokenize(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (unsigned char c : text) {
        if (std::isalnum(c)) {
            cur.push_back(static_cast<char>(std::tolower(c)));
        } else if (!cur.empty()) {
            out.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static std::vector<std::string> splitSentences(const std::string& text) {
    std::vector<std::string> result;
    std::string cur;
    for (char c : text) {
        cur.push_back(c);
        if (c == '.' || c == '?' || c == '!') {
            std::string s = trim(cur);
            if (!s.empty()) result.push_back(s);
            cur.clear();
        }
    }
    std::string tail = trim(cur);
    if (!tail.empty()) result.push_back(tail);
    return result;
}

struct Document {
    std::string id;
    std::string title;
    std::string text;
};

struct RetrievedDocument {
    Document doc;
    double score = 0.0;       // retriever similarity, before softmax
    double probability = 0.0; // top-K truncated p_eta(z | x)
};

struct Candidate {
    std::string text;
    double probability = 0.0;
    std::string provenance;
};

class Retriever {
public:
    virtual ~Retriever() = default;
    virtual std::vector<RetrievedDocument> retrieve(const std::string& query, std::size_t k) const = 0;
};

class Generator {
public:
    virtual ~Generator() = default;

    // p_theta(y | x, z), used by RAG-Sequence.
    virtual std::vector<Candidate> generateSequenceCandidates(
        const std::string& query,
        const Document& doc,
        std::size_t beamSize
    ) const = 0;

    // p_theta(y_i | x, z, y_<i), used by RAG-Token.
    virtual std::map<std::string, double> nextTokenDistribution(
        const std::string& query,
        const Document& doc,
        const std::vector<std::string>& prefix
    ) const = 0;
};

class TfidfRetriever final : public Retriever {
public:
    explicit TfidfRetriever(std::vector<Document> docs) : docs_(std::move(docs)) {
        buildIndex();
    }

    std::vector<RetrievedDocument> retrieve(const std::string& query, std::size_t k) const override {
        std::vector<std::pair<double, std::size_t>> scored;
        const auto qvec = vectorize(tokenize(query));
        const double qnorm = norm(qvec);

        for (std::size_t i = 0; i < docs_.size(); ++i) {
            double sim = 0.0;
            if (qnorm > 0.0 && docNorms_[i] > 0.0) {
                sim = dot(qvec, docVectors_[i]) / (qnorm * docNorms_[i]);
            }
            scored.push_back({sim, i});
        }

        std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) {
            if (a.first != b.first) return a.first > b.first;
            return a.second < b.second;
        });
        if (scored.size() > k) scored.resize(k);

        // Top-K truncated softmax approximates p_eta(z | x) over retrieved docs.
        double maxScore = scored.empty() ? 0.0 : scored.front().first;
        double denom = 0.0;
        for (auto& [score, _] : scored) denom += std::exp(score - maxScore);

        std::vector<RetrievedDocument> out;
        for (auto& [score, idx] : scored) {
            out.push_back({docs_[idx], score, denom == 0.0 ? 0.0 : std::exp(score - maxScore) / denom});
        }
        return out;
    }

private:
    void buildIndex() {
        std::unordered_map<std::string, int> df;
        std::vector<std::vector<std::string>> tokenized;
        for (const auto& doc : docs_) {
            auto toks = tokenize(doc.title + " " + doc.text);
            tokenized.push_back(toks);
            std::set<std::string> seen(toks.begin(), toks.end());
            for (const auto& t : seen) df[t]++;
        }

        const double n = static_cast<double>(docs_.size());
        for (const auto& [term, freq] : df) {
            idf_[term] = std::log((1.0 + n) / (1.0 + freq)) + 1.0;
        }

        for (const auto& toks : tokenized) {
            docVectors_.push_back(vectorize(toks));
            docNorms_.push_back(norm(docVectors_.back()));
        }
    }

    std::unordered_map<std::string, double> vectorize(const std::vector<std::string>& toks) const {
        std::unordered_map<std::string, double> tf;
        for (const auto& t : toks) tf[t] += 1.0;
        for (auto& [term, value] : tf) {
            auto it = idf_.find(term);
            value *= (it == idf_.end() ? 1.0 : it->second);
        }
        return tf;
    }

    static double dot(const std::unordered_map<std::string, double>& a,
                      const std::unordered_map<std::string, double>& b) {
        const auto* small = &a;
        const auto* large = &b;
        if (small->size() > large->size()) std::swap(small, large);
        double sum = 0.0;
        for (const auto& [term, av] : *small) {
            auto it = large->find(term);
            if (it != large->end()) sum += av * it->second;
        }
        return sum;
    }

    static double norm(const std::unordered_map<std::string, double>& v) {
        double sum = 0.0;
        for (const auto& [_, x] : v) sum += x * x;
        return std::sqrt(sum);
    }

    std::vector<Document> docs_;
    std::unordered_map<std::string, double> idf_;
    std::vector<std::unordered_map<std::string, double>> docVectors_;
    std::vector<double> docNorms_;
};

class ExtractiveGenerator final : public Generator {
public:
    std::vector<Candidate> generateSequenceCandidates(
        const std::string& query,
        const Document& doc,
        std::size_t beamSize
    ) const override {
        std::vector<Candidate> candidates;
        const auto qtokens = tokenize(query);
        for (const auto& sentence : splitSentences(doc.text)) {
            double s = lexicalOverlap(qtokens, tokenize(sentence));
            if (s > 0.0) {
                candidates.push_back({sentence, std::exp(s), doc.title});
            }
        }
        if (candidates.empty()) {
            candidates.push_back({firstWords(doc.text, 18), 1.0, doc.title});
        }
        normalize(candidates);
        std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
            return a.probability > b.probability;
        });
        if (candidates.size() > beamSize) candidates.resize(beamSize);
        return candidates;
    }

    std::map<std::string, double> nextTokenDistribution(
        const std::string& query,
        const Document& doc,
        const std::vector<std::string>& prefix
    ) const override {
        auto candidates = generateSequenceCandidates(query, doc, 1);
        auto toks = tokenize(candidates.front().text);
        std::string next = "<eos>";
        if (prefix.size() < toks.size()) next = toks[prefix.size()];
        return {{next, 0.85}, {"<eos>", next == "<eos>" ? 0.85 : 0.15}};
    }

private:
    static double lexicalOverlap(const std::vector<std::string>& a, const std::vector<std::string>& b) {
        std::set<std::string> as(a.begin(), a.end());
        double score = 0.0;
        for (const auto& t : b) {
            if (as.count(t)) score += 1.0;
        }
        return score;
    }

    static std::string firstWords(const std::string& text, std::size_t n) {
        std::istringstream in(text);
        std::ostringstream out;
        std::string w;
        for (std::size_t i = 0; i < n && in >> w; ++i) {
            if (i) out << ' ';
            out << w;
        }
        return out.str();
    }

    static void normalize(std::vector<Candidate>& cands) {
        double denom = 0.0;
        for (const auto& c : cands) denom += c.probability;
        if (denom == 0.0) return;
        for (auto& c : cands) c.probability /= denom;
    }
};

enum class RagMode { Sequence, Token };

class RagModel {
public:
    RagModel(const Retriever& retriever, const Generator& generator, std::size_t topK, std::size_t beam)
        : retriever_(retriever), generator_(generator), topK_(topK), beam_(beam) {}

    Candidate answer(const std::string& query, RagMode mode) const {
        auto docs = retriever_.retrieve(query, topK_);
        if (docs.empty()) throw std::runtime_error("No documents available for retrieval");
        return mode == RagMode::Sequence ? ragSequence(query, docs) : ragToken(query, docs);
    }

    void printRetrieved(const std::string& query, std::ostream& os) const {
        auto docs = retriever_.retrieve(query, topK_);
        os << "Retrieved top-" << docs.size() << " documents:\n";
        for (const auto& r : docs) {
            os << "  " << r.doc.id << " | " << r.doc.title
               << " | score=" << r.score
               << " | p_eta=" << r.probability << "\n";
        }
    }

private:
    Candidate ragSequence(const std::string& query, const std::vector<RetrievedDocument>& docs) const {
        // Paper equation: sum_z p_eta(z|x) p_theta(y|x,z).  Beam candidates from
        // each doc are merged by exact text and marginalized over documents.
        std::map<std::string, Candidate> merged;
        for (const auto& rd : docs) {
            for (const auto& cand : generator_.generateSequenceCandidates(query, rd.doc, beam_)) {
                double joint = rd.probability * cand.probability;
                auto& m = merged[cand.text];
                m.text = cand.text;
                m.probability += joint;
                if (!m.provenance.empty()) m.provenance += ", ";
                m.provenance += rd.doc.title;
            }
        }
        return best(merged);
    }

    Candidate ragToken(const std::string& query, const std::vector<RetrievedDocument>& docs) const {
        // Paper equation: for each token, marginalize next-token distributions
        // over the same retrieved top-K set, then autoregressively append best.
        std::vector<std::string> prefix;
        double prob = 1.0;
        for (std::size_t step = 0; step < 32; ++step) {
            std::map<std::string, double> marginal;
            for (const auto& rd : docs) {
                auto dist = generator_.nextTokenDistribution(query, rd.doc, prefix);
                for (const auto& [tok, p] : dist) marginal[tok] += rd.probability * p;
            }
            auto it = std::max_element(marginal.begin(), marginal.end(), [](const auto& a, const auto& b) {
                return a.second < b.second;
            });
            if (it == marginal.end() || it->first == "<eos>") break;
            prefix.push_back(it->first);
            prob *= it->second;
        }
        std::ostringstream text;
        for (std::size_t i = 0; i < prefix.size(); ++i) {
            if (i) text << ' ';
            text << prefix[i];
        }
        return {text.str(), prob, "token-level marginal over top-K documents"};
    }

    static Candidate best(const std::map<std::string, Candidate>& merged) {
        auto it = std::max_element(merged.begin(), merged.end(), [](const auto& a, const auto& b) {
            return a.second.probability < b.second.probability;
        });
        if (it == merged.end()) return {"", 0.0, ""};
        return it->second;
    }

    const Retriever& retriever_;
    const Generator& generator_;
    std::size_t topK_;
    std::size_t beam_;
};

}  // namespace rag

int main(int argc, char** argv) {
    std::string query = "What does RAG combine?";
    rag::RagMode mode = rag::RagMode::Sequence;
    std::size_t topK = 3;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--query" && i + 1 < argc) {
            query = argv[++i];
        } else if (arg == "--mode" && i + 1 < argc) {
            std::string m = argv[++i];
            if (m == "sequence") mode = rag::RagMode::Sequence;
            else if (m == "token") mode = rag::RagMode::Token;
            else throw std::runtime_error("--mode must be 'sequence' or 'token'");
        } else if (arg == "--top-k" && i + 1 < argc) {
            topK = static_cast<std::size_t>(std::stoul(argv[++i]));
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: ./rag [--query TEXT] [--mode sequence|token] [--top-k K]\n\n"
                      << "Demonstrates the RAG equations from arXiv:2005.11401 with a local\n"
                      << "TF-IDF retriever and deterministic generator.\n";
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            return 1;
        }
    }

    std::vector<rag::Document> docs = {
        {"d1", "RAG", "Retrieval-Augmented Generation combines a pre-trained parametric seq2seq generator with non-parametric memory from retrieved text passages. It retrieves top-K documents and marginalizes generation probabilities over latent documents."},
        {"d2", "DPR", "Dense Passage Retrieval uses a bi-encoder architecture. A query encoder embeds the input and a document encoder embeds passages, then maximum inner product search returns the highest scoring passages."},
        {"d3", "BART", "BART is a pre-trained sequence-to-sequence Transformer used as the generator in the RAG paper. The generator conditions on the input and a retrieved passage."},
        {"d4", "RAG-Token", "RAG-Token marginalizes over retrieved documents for every generated token, allowing different documents to support different tokens in the same answer."},
        {"d5", "RAG-Sequence", "RAG-Sequence uses the same retrieved document to generate the complete output sequence and sums probabilities over top-K documents."}
    };

    rag::TfidfRetriever retriever(std::move(docs));
    rag::ExtractiveGenerator generator;
    rag::RagModel model(retriever, generator, topK, 3);

    std::cout << "Query: " << query << "\n";
    retriever.retrieve(query, topK); // force retrieval path in smoke test
    model.printRetrieved(query, std::cout);
    auto ans = model.answer(query, mode);
    std::cout << "Mode: " << (mode == rag::RagMode::Sequence ? "RAG-Sequence" : "RAG-Token") << "\n";
    std::cout << "Answer: " << ans.text << "\n";
    std::cout << "Marginal probability: " << ans.probability << "\n";
    std::cout << "Provenance: " << ans.provenance << "\n";
    return 0;
}
