#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
#include <vector>

#include "ovicode/transcript.hpp"

namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t percentile(std::vector<std::uint64_t> values, double fraction) {
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(
        fraction * static_cast<double>(values.size() - 1));
    return values[index];
}

}  // namespace

int main(int argc, char** argv) {
    const bool verify = argc > 1 && std::string(argv[1]) == "--verify";
    ovicode::TranscriptStore store;
    for (int i = 0; i < 1000; ++i) {
        store.push({ovicode::TranscriptEntry::Kind::Assistant,
                    "fixture " + std::to_string(i),
                    std::string(240, static_cast<char>('a' + i % 26))});
    }
    const auto snapshot = store.snapshot();
    ovicode::TranscriptView view;
    view.update(snapshot, 112);
    std::vector<std::uint64_t> samples;
    samples.reserve(100);
    for (int i = 0; i < 100; ++i) {
        store.push({ovicode::TranscriptEntry::Kind::Assistant,
                    "incremental " + std::to_string(i),
                    std::string(240, 'z')});
        const auto incremental = store.snapshot();
        const auto started = Clock::now();
        view.update(incremental, 112);
        const auto visible = view.visibleRows(view.totalRows() - 20, 20, 0);
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                                 Clock::now() - started)
                                 .count();
        if (visible.size() != 20) return 1;
        samples.push_back(static_cast<std::uint64_t>(elapsed));
    }
    const auto p50 = percentile(samples, 0.50);
    const auto p95 = percentile(samples, 0.95);
    const auto p99 = percentile(samples, 0.99);
    std::cout << "transcript_format_us p50=" << p50 << " p95=" << p95
              << " p99=" << p99 << " entries=" << snapshot.entries.size()
              << '\n';
    // Verification catches accidental unbounded fixture behavior but does not
    // enforce machine-dependent timing thresholds.
    if (verify && store.snapshot().entries.size() > 1000) return 1;
    return 0;
}
