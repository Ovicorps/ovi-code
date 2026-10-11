#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace ovicode {

struct TranscriptEntry {
    enum class Kind { User, Assistant, ToolCall, ToolResult, System, Error };
    Kind kind;
    std::string title;
    std::string body;
};

struct TranscriptLimits {
    std::size_t max_entries = 1000;
    std::size_t keep_entries = 900;
    std::size_t max_bytes = 2 * 1024 * 1024;
    std::size_t keep_bytes = 1800 * 1024;
    std::size_t max_lines = 20000;
    std::size_t keep_lines = 18000;
    std::size_t max_entry_bytes = 128 * 1024;
    std::size_t max_entry_lines = 2000;
};

struct StoredTranscriptEntry {
    std::uint64_t id = 0;
    TranscriptEntry entry;
    std::size_t bytes = 0;
    std::size_t lines = 0;
};

struct TranscriptSnapshot {
    std::uint64_t generation = 0;
    std::vector<std::shared_ptr<const StoredTranscriptEntry>> entries;
    std::size_t dropped_entries = 0;
    std::size_t bytes = 0;
    std::size_t lines = 0;
};

class TranscriptStore {
  public:
    explicit TranscriptStore(TranscriptLimits limits = {});

    void push(TranscriptEntry entry);
    void clear();
    TranscriptSnapshot snapshot() const;
    std::optional<TranscriptSnapshot> snapshotIfChanged(
        std::uint64_t known_generation) const;

  private:
    std::shared_ptr<const StoredTranscriptEntry> makeStored(
        TranscriptEntry entry);
    void rebuildElisionEntry();

    TranscriptLimits limits_;
    mutable std::mutex mutex_;
    std::deque<std::shared_ptr<const StoredTranscriptEntry>> entries_;
    std::shared_ptr<const StoredTranscriptEntry> elision_entry_;
    std::uint64_t generation_ = 0;
    std::uint64_t next_id_ = 1;
    std::size_t bytes_ = 0;
    std::size_t lines_ = 0;
    std::size_t dropped_entries_ = 0;
};

enum class TranscriptRowStyle {
    Header,
    Body,
    DiffAdd,
    DiffRemove,
    DiffHunk,
    DiffMeta,
    Done,
    Spacer,
};

struct TranscriptRow {
    std::uint64_t entry_id = 0;
    TranscriptEntry::Kind kind = TranscriptEntry::Kind::System;
    TranscriptRowStyle style = TranscriptRowStyle::Body;
    std::string text;
};

class TranscriptView {
  public:
    void update(const TranscriptSnapshot& snapshot, int width);
    std::size_t totalRows() const noexcept;
    std::vector<TranscriptRow> visibleRows(std::size_t top,
                                           std::size_t height,
                                           std::size_t overscan = 2) const;

  private:
    struct CachedRows {
        int width = 0;
        std::vector<TranscriptRow> rows;
    };

    struct RowGroup {
        std::shared_ptr<const CachedRows> cached;
        std::size_t row_begin = 0;
        std::size_t row_count = 0;
    };

    std::vector<TranscriptRow> format(const StoredTranscriptEntry& stored,
                                      int width) const;

    std::uint64_t generation_ = 0;
    int width_ = 0;
    std::unordered_map<std::uint64_t, std::shared_ptr<CachedRows>> cache_;
    std::vector<RowGroup> groups_;
    std::vector<std::size_t> group_ends_;
    std::size_t total_rows_ = 0;
};

}  // namespace ovicode
