#include "ovicode/transcript.hpp"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <unordered_set>

#include "ovicode/metrics.hpp"
#include "ovicode/utf8.hpp"

namespace ovicode {

namespace {

std::size_t lineCount(const std::string& text) {
    return text.empty()
               ? 0
               : 1 + static_cast<std::size_t>(
                         std::count(text.begin(), text.end(), '\n'));
}

std::string boundedBody(const std::string& body, std::size_t max_bytes,
                        std::size_t max_lines) {
    static constexpr std::string_view marker =
        "\n...[entry truncated to transcript budget]";
    if (body.size() <= max_bytes && lineCount(body) <= max_lines) return body;
    if (max_lines <= 1) {
        return utf8::truncateBytes(marker.substr(1), max_bytes);
    }
    if (body.size() > max_bytes && max_bytes <= marker.size()) {
        return utf8::truncateBytes(marker, max_bytes);
    }
    const std::size_t content_budget =
        max_bytes > marker.size() ? max_bytes - marker.size() : 0;
    std::size_t end = std::min(body.size(), content_budget);
    const std::size_t content_lines = max_lines > 1 ? max_lines - 1 : 0;
    std::size_t lines = 0;
    for (std::size_t i = 0; i < end; ++i) {
        if (body[i] == '\n' && ++lines >= content_lines) {
            end = i;
            break;
        }
    }
    if (end == body.size()) return body;
    std::string out = utf8::truncateBytes(body, end);
    out += marker;
    return out;
}

std::vector<std::string> splitLines(const std::string& body) {
    std::vector<std::string> lines;
    std::istringstream input(body);
    std::string line;
    while (std::getline(input, line)) lines.push_back(std::move(line));
    if (lines.empty()) lines.emplace_back();
    return lines;
}

std::vector<std::string> wrapLine(const std::string& line, int width) {
    std::vector<std::string> rows;
    if (line.empty()) {
        rows.emplace_back();
        return rows;
    }
    std::string_view remaining = line;
    while (!remaining.empty()) {
        std::size_t end = utf8::byteOffsetForColumn(remaining, width);
        if (end == 0) end = utf8::nextBoundary(remaining, 0);
        if (end < remaining.size()) {
            const std::size_t space = remaining.substr(0, end).find_last_of(' ');
            if (space != std::string_view::npos && space > 0) end = space;
        }
        rows.emplace_back(remaining.substr(0, end));
        remaining.remove_prefix(end);
        while (!remaining.empty() && remaining.front() == ' ') {
            remaining.remove_prefix(1);
        }
    }
    return rows;
}

const char* kindLabel(TranscriptEntry::Kind kind) {
    switch (kind) {
        case TranscriptEntry::Kind::User:       return "you";
        case TranscriptEntry::Kind::Assistant:  return "assistant";
        case TranscriptEntry::Kind::ToolCall:   return "tool ->";
        case TranscriptEntry::Kind::ToolResult: return "tool <-";
        case TranscriptEntry::Kind::System:     return "system";
        case TranscriptEntry::Kind::Error:      return "error";
    }
    return "?";
}

}  // namespace

TranscriptStore::TranscriptStore(TranscriptLimits limits)
    : limits_(std::move(limits)) {
    limits_.keep_entries = std::min(limits_.keep_entries, limits_.max_entries);
    limits_.keep_bytes = std::min(limits_.keep_bytes, limits_.max_bytes);
    limits_.keep_lines = std::min(limits_.keep_lines, limits_.max_lines);
}

std::shared_ptr<const StoredTranscriptEntry> TranscriptStore::makeStored(
    TranscriptEntry entry) {
    entry.body = boundedBody(entry.body, limits_.max_entry_bytes,
                             limits_.max_entry_lines);
    auto stored = std::make_shared<StoredTranscriptEntry>();
    stored->id = next_id_++;
    stored->bytes = entry.title.size() + entry.body.size();
    stored->lines = 1 + lineCount(entry.body);
    stored->entry = std::move(entry);
    return stored;
}

void TranscriptStore::rebuildElisionEntry() {
    if (dropped_entries_ == 0) {
        elision_entry_.reset();
        return;
    }
    TranscriptEntry entry{
        TranscriptEntry::Kind::System,
        "elided",
        "(" + std::to_string(dropped_entries_) +
            " earlier entries dropped to keep the transcript bounded)",
    };
    elision_entry_ = makeStored(std::move(entry));
}

void TranscriptStore::push(TranscriptEntry entry) {
    const auto started = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    const auto locked = std::chrono::steady_clock::now();
    auto stored = makeStored(std::move(entry));
    bytes_ += stored->bytes;
    lines_ += stored->lines;
    entries_.push_back(std::move(stored));

    const bool over = entries_.size() > limits_.max_entries ||
                      bytes_ > limits_.max_bytes || lines_ > limits_.max_lines;
    std::size_t dropped_now = 0;
    if (over) {
        while (!entries_.empty() &&
               (entries_.size() > limits_.keep_entries ||
                bytes_ > limits_.keep_bytes || lines_ > limits_.keep_lines)) {
            bytes_ -= entries_.front()->bytes;
            lines_ -= entries_.front()->lines;
            entries_.pop_front();
            ++dropped_now;
        }
        dropped_entries_ += dropped_now;
        rebuildElisionEntry();
    }
    ++generation_;
    metrics::add(metrics::Counter::TranscriptEntries);
    metrics::add(metrics::Counter::TranscriptEntriesElided, dropped_now);
    const auto finished = std::chrono::steady_clock::now();
    metrics::observe(metrics::Distribution::TranscriptLockWaitUs,
                     std::chrono::duration_cast<std::chrono::microseconds>(
                         locked - started)
                         .count());
    metrics::observe(metrics::Distribution::TranscriptLockHoldUs,
                     std::chrono::duration_cast<std::chrono::microseconds>(
                         finished - locked)
                         .count());
}

void TranscriptStore::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    elision_entry_.reset();
    bytes_ = 0;
    lines_ = 0;
    dropped_entries_ = 0;
    ++generation_;
}

TranscriptSnapshot TranscriptStore::snapshot() const {
    const auto started = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    const auto locked = std::chrono::steady_clock::now();
    TranscriptSnapshot result;
    result.generation = generation_;
    result.dropped_entries = dropped_entries_;
    result.bytes = bytes_;
    result.lines = lines_;
    result.entries.reserve(entries_.size() + (elision_entry_ ? 1 : 0));
    if (elision_entry_) result.entries.push_back(elision_entry_);
    result.entries.insert(result.entries.end(), entries_.begin(), entries_.end());
    const auto finished = std::chrono::steady_clock::now();
    metrics::observe(metrics::Distribution::TranscriptLockWaitUs,
                     std::chrono::duration_cast<std::chrono::microseconds>(
                         locked - started)
                         .count());
    metrics::observe(metrics::Distribution::TranscriptLockHoldUs,
                     std::chrono::duration_cast<std::chrono::microseconds>(
                         finished - locked)
                         .count());
    return result;
}

std::optional<TranscriptSnapshot> TranscriptStore::snapshotIfChanged(
    std::uint64_t known_generation) const {
    const auto started = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    const auto locked = std::chrono::steady_clock::now();
    if (generation_ == known_generation) {
        const auto finished = std::chrono::steady_clock::now();
        metrics::observe(metrics::Distribution::TranscriptLockWaitUs,
                         std::chrono::duration_cast<std::chrono::microseconds>(
                             locked - started)
                             .count());
        metrics::observe(metrics::Distribution::TranscriptLockHoldUs,
                         std::chrono::duration_cast<std::chrono::microseconds>(
                             finished - locked)
                             .count());
        return std::nullopt;
    }
    TranscriptSnapshot result;
    result.generation = generation_;
    result.dropped_entries = dropped_entries_;
    result.bytes = bytes_;
    result.lines = lines_;
    result.entries.reserve(entries_.size() + (elision_entry_ ? 1 : 0));
    if (elision_entry_) result.entries.push_back(elision_entry_);
    result.entries.insert(result.entries.end(), entries_.begin(), entries_.end());
    const auto finished = std::chrono::steady_clock::now();
    metrics::observe(metrics::Distribution::TranscriptLockWaitUs,
                     std::chrono::duration_cast<std::chrono::microseconds>(
                         locked - started)
                         .count());
    metrics::observe(metrics::Distribution::TranscriptLockHoldUs,
                     std::chrono::duration_cast<std::chrono::microseconds>(
                         finished - locked)
                         .count());
    return result;
}

std::vector<TranscriptRow> TranscriptView::format(
    const StoredTranscriptEntry& stored, int width) const {
    const TranscriptEntry& entry = stored.entry;
    std::vector<TranscriptRow> rows;
    const int safe_width = std::max(8, width);
    if (entry.kind == TranscriptEntry::Kind::System && entry.title == "done") {
        rows.push_back({stored.id, entry.kind, TranscriptRowStyle::Done,
                        utf8::truncateColumns("[done] " + entry.body,
                                              safe_width)});
        rows.push_back({stored.id, entry.kind, TranscriptRowStyle::Spacer, ""});
        return rows;
    }

    const std::string header = std::string("* ") + kindLabel(entry.kind) +
                               (entry.title.empty() ? "" : " | " + entry.title);
    rows.push_back({stored.id, entry.kind, TranscriptRowStyle::Header,
                    utf8::truncateColumns(header, safe_width)});

    const bool diff = entry.body.rfind("--- a/", 0) == 0;
    constexpr std::size_t kMaxRowsPerEntry = 2048;
    bool truncated = false;
    for (const auto& line : splitLines(entry.body)) {
        if (rows.size() + 2 >= kMaxRowsPerEntry) {
            truncated = true;
            break;
        }
        if (diff) {
            TranscriptRowStyle style = TranscriptRowStyle::DiffMeta;
            if (!line.empty() && line.front() == '+') {
                style = TranscriptRowStyle::DiffAdd;
            } else if (!line.empty() && line.front() == '-') {
                style = TranscriptRowStyle::DiffRemove;
            } else if (!line.empty() && line.front() == '@') {
                style = TranscriptRowStyle::DiffHunk;
            }
            rows.push_back({stored.id, entry.kind, style,
                            utf8::truncateColumns(line, safe_width)});
            continue;
        }
        for (auto& wrapped : wrapLine(line, safe_width)) {
            if (rows.size() + 2 >= kMaxRowsPerEntry) {
                truncated = true;
                break;
            }
            rows.push_back(
                {stored.id, entry.kind, TranscriptRowStyle::Body,
                 std::move(wrapped)});
        }
        if (truncated) break;
    }
    if (truncated) {
        rows.push_back({stored.id, entry.kind, TranscriptRowStyle::DiffMeta,
                        "...[entry rows truncated]"});
    }
    rows.push_back({stored.id, entry.kind, TranscriptRowStyle::Spacer, ""});
    return rows;
}

void TranscriptView::update(const TranscriptSnapshot& snapshot, int width) {
    width = std::max(8, width);
    if (snapshot.generation == generation_ && width == width_) return;

    constexpr std::size_t kMaxFormattedRows = 30000;
    std::unordered_set<std::uint64_t> live;
    std::vector<RowGroup> reverse_groups;
    std::size_t retained_rows = 0;
    bool hidden_older_rows = false;
    for (auto it = snapshot.entries.rbegin(); it != snapshot.entries.rend();
         ++it) {
        const auto& stored = *it;
        live.insert(stored->id);
        auto& cached = cache_[stored->id];
        if (!cached || cached->width != width) {
            cached = std::make_shared<CachedRows>();
            cached->width = width;
            cached->rows = format(*stored, width);
        }
        if (retained_rows + cached->rows.size() > kMaxFormattedRows) {
            hidden_older_rows = true;
            break;
        }
        reverse_groups.push_back({cached, 0, cached->rows.size()});
        retained_rows += cached->rows.size();
    }
    for (auto it = cache_.begin(); it != cache_.end();) {
        if (!live.contains(it->first)) {
            it = cache_.erase(it);
        } else {
            ++it;
        }
    }
    groups_.assign(reverse_groups.rbegin(), reverse_groups.rend());
    group_ends_.clear();
    total_rows_ = 0;
    if (hidden_older_rows) {
        auto marker = std::make_shared<CachedRows>();
        marker->width = width;
        marker->rows.push_back(
            {0, TranscriptEntry::Kind::System, TranscriptRowStyle::DiffMeta,
             "...[older formatted rows elided]"});
        groups_.insert(groups_.begin(), {std::move(marker), 0, 1});
    }
    group_ends_.reserve(groups_.size());
    for (const auto& group : groups_) {
        total_rows_ += group.row_count;
        group_ends_.push_back(total_rows_);
    }
    generation_ = snapshot.generation;
    width_ = width;
}

std::size_t TranscriptView::totalRows() const noexcept { return total_rows_; }

std::vector<TranscriptRow> TranscriptView::visibleRows(
    std::size_t top, std::size_t height, std::size_t overscan) const {
    const std::size_t begin = top > overscan ? top - overscan : 0;
    const std::size_t end =
        std::min(total_rows_, top + height + overscan);
    std::vector<TranscriptRow> result;
    if (begin >= end) return result;
    result.reserve(end - begin);
    auto group_it = std::upper_bound(group_ends_.begin(), group_ends_.end(),
                                     begin);
    std::size_t group_index =
        static_cast<std::size_t>(group_it - group_ends_.begin());
    std::size_t group_start =
        group_index == 0 ? 0 : group_ends_[group_index - 1];
    std::size_t row = begin;
    while (row < end && group_index < groups_.size()) {
        const auto& group = groups_[group_index];
        const std::size_t local = row - group_start;
        const std::size_t count =
            std::min(group.row_count - local, end - row);
        const auto first = group.cached->rows.begin() +
                           static_cast<std::ptrdiff_t>(group.row_begin + local);
        result.insert(result.end(), first,
                      first + static_cast<std::ptrdiff_t>(count));
        row += count;
        ++group_index;
        group_start += group.row_count;
    }
    return result;
}

}  // namespace ovicode
