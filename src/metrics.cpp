#include "ovicode/metrics.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

namespace ovicode::metrics {

namespace {

constexpr std::array<const char*, static_cast<std::size_t>(Counter::Count)>
    kCounterNames{
        "turns_total",
        "tool_calls_total",
        "tool_errors_total",
        "rag_files_scanned_total",
        "rag_cache_hits_total",
        "rag_input_bytes_total",
        "transcript_entries_total",
        "transcript_entries_elided_total",
        "tui_custom_events_total",
        "tui_render_builds_total",
        "http_requests_total",
        "http_transport_errors_total",
        "http_non_2xx_total",
    };

constexpr std::array<const char*, static_cast<std::size_t>(Distribution::Count)>
    kDistributionNames{
        "turn_duration_us",
        "tool_duration_us",
        "tool_result_bytes",
        "rag_duration_us",
        "rag_context_bytes",
        "http_duration_us",
        "http_request_body_bytes",
        "http_response_body_bytes",
        "tui_custom_event_to_render_us",
        "tui_render_build_us",
        "transcript_lock_wait_us",
        "transcript_lock_hold_us",
        "tui_shutdown_us",
        "tui_worker_join_us",
    };

struct Aggregate {
    std::atomic<std::uint64_t> count{0};
    std::atomic<std::uint64_t> sum{0};
    std::atomic<std::uint64_t> min{std::numeric_limits<std::uint64_t>::max()};
    std::atomic<std::uint64_t> max{0};
    std::array<std::atomic<std::uint64_t>, 10> buckets{};
};

constexpr std::array<std::uint64_t, 9> kBucketUpperBounds{
    10, 100, 1000, 5000, 10000, 33000, 100000, 1000000, 10000000};

std::array<std::atomic<std::uint64_t>,
           static_cast<std::size_t>(Counter::Count)>
    counters{};
std::array<Aggregate, static_cast<std::size_t>(Distribution::Count)>
    distributions{};
std::once_flag path_once;
std::filesystem::path output_path;
std::atomic<bool> flushed{false};

std::uint64_t nowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void initializePath() {
    std::call_once(path_once, [] {
        if (const char* value = std::getenv("OVI_CODE_METRICS_FILE");
            value && *value) {
            output_path = value;
        }
    });
}

void updateMin(std::atomic<std::uint64_t>& target, std::uint64_t value) {
    std::uint64_t current = target.load(std::memory_order_relaxed);
    while (value < current &&
           !target.compare_exchange_weak(current, value,
                                         std::memory_order_relaxed)) {
    }
}

void updateMax(std::atomic<std::uint64_t>& target, std::uint64_t value) {
    std::uint64_t current = target.load(std::memory_order_relaxed);
    while (value > current &&
           !target.compare_exchange_weak(current, value,
                                         std::memory_order_relaxed)) {
    }
}

}  // namespace

bool enabled() noexcept {
    try {
        initializePath();
        return !output_path.empty();
    } catch (...) {
        return false;
    }
}

void add(Counter counter, std::uint64_t value) noexcept {
    if (!enabled()) return;
    counters[static_cast<std::size_t>(counter)].fetch_add(
        value, std::memory_order_relaxed);
}

void observe(Distribution distribution, std::uint64_t value) noexcept {
    if (!enabled()) return;
    auto& aggregate = distributions[static_cast<std::size_t>(distribution)];
    aggregate.count.fetch_add(1, std::memory_order_relaxed);
    aggregate.sum.fetch_add(value, std::memory_order_relaxed);
    updateMin(aggregate.min, value);
    updateMax(aggregate.max, value);
    const auto bound = std::lower_bound(kBucketUpperBounds.begin(),
                                        kBucketUpperBounds.end(), value);
    aggregate.buckets[static_cast<std::size_t>(
        bound - kBucketUpperBounds.begin())]
        .fetch_add(1, std::memory_order_relaxed);
}

ScopedTimer::ScopedTimer(Distribution distribution) noexcept
    : distribution_(distribution), started_ns_(nowNs()) {}

ScopedTimer::~ScopedTimer() {
    observe(distribution_, (nowNs() - started_ns_) / 1000);
}

void flush() noexcept {
    try {
        if (!enabled() || flushed.exchange(true)) return;
        nlohmann::json document = {
            {"schema", "ovi-code.metrics.v1"},
            {"counters", nlohmann::json::object()},
            {"distributions", nlohmann::json::object()},
        };
        for (std::size_t i = 0; i < counters.size(); ++i) {
            document["counters"][kCounterNames[i]] =
                counters[i].load(std::memory_order_relaxed);
        }
        for (std::size_t i = 0; i < distributions.size(); ++i) {
            const auto count =
                distributions[i].count.load(std::memory_order_relaxed);
            if (count == 0) continue;
            document["distributions"][kDistributionNames[i]] = {
                {"count", count},
                {"sum", distributions[i].sum.load(std::memory_order_relaxed)},
                {"min", distributions[i].min.load(std::memory_order_relaxed)},
                {"max", distributions[i].max.load(std::memory_order_relaxed)},
                {"buckets", nlohmann::json::array()},
            };
            auto& buckets = document["distributions"][kDistributionNames[i]]
                                    ["buckets"];
            std::uint64_t cumulative = 0;
            for (std::size_t bucket = 0;
                 bucket < distributions[i].buckets.size(); ++bucket) {
                cumulative += distributions[i].buckets[bucket].load(
                    std::memory_order_relaxed);
                buckets.push_back({
                    {"le", bucket < kBucketUpperBounds.size()
                               ? nlohmann::json(kBucketUpperBounds[bucket])
                               : nlohmann::json("+Inf")},
                    {"count", cumulative},
                });
            }
        }
        std::error_code error;
        if (output_path.has_parent_path()) {
            std::filesystem::create_directories(output_path.parent_path(), error);
        }
        std::ofstream file(output_path, std::ios::app);
        if (!file) return;
        file << document.dump() << '\n';
        file.close();
        std::filesystem::permissions(
            output_path,
            std::filesystem::perms::owner_read |
                std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace, error);
    } catch (...) {
    }
}

}  // namespace ovicode::metrics
