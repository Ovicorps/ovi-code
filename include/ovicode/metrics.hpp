#pragma once

#include <cstdint>

namespace ovicode::metrics {

enum class Counter {
    Turns,
    ToolCalls,
    ToolErrors,
    RagFilesScanned,
    RagCacheHits,
    RagInputBytes,
    TranscriptEntries,
    TranscriptEntriesElided,
    TuiCustomEvents,
    TuiRenderBuilds,
    HttpRequests,
    HttpTransportErrors,
    HttpNon2xx,
    Count,
};

enum class Distribution {
    TurnDurationUs,
    ToolDurationUs,
    ToolResultBytes,
    RagDurationUs,
    RagContextBytes,
    HttpDurationUs,
    HttpRequestBodyBytes,
    HttpResponseBodyBytes,
    TuiCustomEventToRenderUs,
    TuiRenderBuildUs,
    TranscriptLockWaitUs,
    TranscriptLockHoldUs,
    TuiShutdownUs,
    TuiWorkerJoinUs,
    Count,
};

bool enabled() noexcept;
void add(Counter counter, std::uint64_t value = 1) noexcept;
void observe(Distribution distribution, std::uint64_t value) noexcept;

class ScopedTimer {
  public:
    explicit ScopedTimer(Distribution distribution) noexcept;
    ~ScopedTimer();

    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

  private:
    Distribution distribution_;
    std::uint64_t started_ns_;
};

void flush() noexcept;

}  // namespace ovicode::metrics
