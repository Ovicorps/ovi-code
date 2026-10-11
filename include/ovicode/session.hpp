#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "ovicode/config.hpp"
#include "ovicode/providers.hpp"
#include "ovicode/transcript.hpp"

namespace ovicode {

struct SessionState {
    std::string id;
    std::filesystem::path workspace;
    std::string provider;
    std::string model;
    std::string draft;
    std::string pending_prompt;
    bool turn_in_progress = false;
    std::vector<Message> messages;
    std::vector<TranscriptEntry> transcript;
};

struct SessionInfo {
    std::string id;
    std::string title;
    std::string model;
    std::int64_t updated_at = 0;  // Unix milliseconds
};

// One writer per session. All saves are private, atomic checkpoints. Drafts
// use a small sidecar so typing never rewrites the whole conversation.
class SessionStore {
  public:
    SessionStore(const std::filesystem::path& directory,
                 const std::filesystem::path& workspace, const Config& cfg,
                 bool start_new = false, const std::string& resume_id = {});
    ~SessionStore();
    SessionStore(const SessionStore&) = delete;
    SessionStore& operator=(const SessionStore&) = delete;

    static std::vector<SessionInfo> list(
        const std::filesystem::path& directory,
        const std::filesystem::path& workspace);

    SessionState state() const;
    const std::filesystem::path& path() const { return path_; }
    void saveDraft(const std::string& draft);
    void beginTurn(const std::string& prompt, const Config& cfg,
                   const TranscriptSnapshot& transcript);
    void checkpoint(const std::vector<Message>& messages,
                    const TranscriptSnapshot& transcript);
    void finishTurn(const std::vector<Message>& messages,
                    const TranscriptSnapshot& transcript, bool completed,
                    bool cancelled);
    void saveTranscript(const TranscriptSnapshot& transcript);
    void resetConversation(const Config& cfg,
                           const TranscriptSnapshot& transcript);

  private:
    void saveLocked();
    void setTranscriptLocked(const TranscriptSnapshot& transcript);

    std::filesystem::path path_;
    int lock_fd_ = -1;
    mutable std::mutex mutex_;
    SessionState state_;
    std::uint64_t transcript_generation_ = 0;
};

}  // namespace ovicode
