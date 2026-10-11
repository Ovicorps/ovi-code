#include "ovicode/clipboard.hpp"

#if defined(__APPLE__)
#include <ApplicationServices/ApplicationServices.h>
#else
#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char** environ;
#endif

namespace ovicode {

#if defined(__APPLE__)
namespace {

struct ClipboardReference {
    PasteboardRef value = nullptr;
    ClipboardReference() {
        if (PasteboardCreate(kPasteboardClipboard, &value) != noErr) {
            value = nullptr;
        }
    }
    ~ClipboardReference() {
        if (value) CFRelease(value);
    }
};

}  // namespace

std::optional<std::string> readSystemClipboard() {
    ClipboardReference clipboard;
    if (!clipboard.value) return std::nullopt;
    PasteboardSynchronize(clipboard.value);
    ItemCount count = 0;
    if (PasteboardGetItemCount(clipboard.value, &count) != noErr) {
        return std::nullopt;
    }
    for (ItemCount i = 1; i <= count; ++i) {
        PasteboardItemID item;
        if (PasteboardGetItemIdentifier(clipboard.value, i, &item) != noErr) {
            continue;
        }
        CFDataRef data = nullptr;
        if (PasteboardCopyItemFlavorData(clipboard.value, item,
                                        CFSTR("public.utf8-plain-text"),
                                        &data) != noErr) {
            continue;
        }
        const CFIndex size = CFDataGetLength(data);
        std::optional<std::string> text;
        if (size <= 16 * 1024) {
            text = size == 0 ? std::string{}
                             : std::string(reinterpret_cast<const char*>(
                                               CFDataGetBytePtr(data)),
                                           static_cast<std::size_t>(size));
        }
        CFRelease(data);
        return text;
    }
    return std::nullopt;
}

bool writeSystemClipboard(std::string_view text) {
    if (text.size() > 16 * 1024) return false;
    ClipboardReference clipboard;
    if (!clipboard.value) return false;
    CFDataRef data = CFDataCreate(
        kCFAllocatorDefault, reinterpret_cast<const UInt8*>(text.data()),
        static_cast<CFIndex>(text.size()));
    if (!data) return false;
    const bool success =
        PasteboardClear(clipboard.value) == noErr &&
        PasteboardPutItemFlavor(clipboard.value,
                               reinterpret_cast<PasteboardItemID>(1),
                               CFSTR("public.utf8-plain-text"), data,
                               kPasteboardFlavorNoFlags) == noErr;
    CFRelease(data);
    return success;
}

#else
namespace {

ssize_t writeClipboardPipe(int fd, const char* data, std::size_t size) {
    sigset_t blocked, previous, pending;
    ::sigemptyset(&blocked);
    ::sigaddset(&blocked, SIGPIPE);
    if (::pthread_sigmask(SIG_BLOCK, &blocked, &previous) != 0) return -1;
    ::sigpending(&pending);
    const bool already_pending = ::sigismember(&pending, SIGPIPE) == 1;
    const ssize_t count = ::write(fd, data, size);
    const int write_error = errno;
    if (count < 0 && write_error == EPIPE && !already_pending) {
        const timespec timeout{0, 0};
        ::sigtimedwait(&blocked, nullptr, &timeout);
    }
    ::pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    errno = write_error;
    return count;
}

std::optional<std::string> clipboardCommand(std::vector<const char*> arguments,
                                          std::optional<std::string_view> input) {
    int pipe_fds[2];
    if (::pipe(pipe_fds) != 0) return std::nullopt;
    for (int fd : pipe_fds) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t actions;
    if (::posix_spawn_file_actions_init(&actions) != 0) {
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        return std::nullopt;
    }
    if (input) {
        ::posix_spawn_file_actions_adddup2(&actions, pipe_fds[0], STDIN_FILENO);
        ::posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null",
                                           O_WRONLY, 0);
    } else {
        ::posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
        ::posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
                                           O_RDONLY, 0);
    }
    ::posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null",
                                       O_WRONLY, 0);
    arguments.push_back(nullptr);
    std::vector<char*> argv;
    for (const char* argument : arguments) {
        argv.push_back(const_cast<char*>(argument));
    }
    pid_t child;
    const int error =
        ::posix_spawnp(&child, argv.front(), &actions, nullptr, argv.data(), environ);
    ::posix_spawn_file_actions_destroy(&actions);
    ::close(pipe_fds[input ? 0 : 1]);
    const int fd = pipe_fds[input ? 1 : 0];
    if (error != 0) {
        ::close(fd);
        return std::nullopt;
    }
    ::fcntl(fd, F_SETFL, O_NONBLOCK);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    std::string output;
    std::size_t offset = 0;
    bool success = false;
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd readiness{fd, static_cast<short>(input ? POLLOUT : POLLIN), 0};
        if (::poll(&readiness, 1, 10) <= 0) continue;
        if (input) {
            if (readiness.revents & (POLLERR | POLLHUP)) break;
            const ssize_t written =
                writeClipboardPipe(fd, input->data() + offset,
                                   input->size() - offset);
            if (written < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (written <= 0) break;
            offset += static_cast<std::size_t>(written);
            if (offset == input->size()) {
                success = true;
                break;
            }
        } else {
            char buffer[512];
            const ssize_t read = ::read(fd, buffer, sizeof(buffer));
            if (read < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (read < 0) break;
            if (read == 0) {
                success = true;
                break;
            }
            output.append(buffer, static_cast<std::size_t>(read));
            if (output.size() > 16 * 1024) break;
        }
    }
    ::close(fd);
    int status = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const pid_t waited = ::waitpid(child, &status, WNOHANG);
        if (waited == child) {
            return success && WIFEXITED(status) && WEXITSTATUS(status) == 0
                       ? std::optional<std::string>(output)
                       : std::nullopt;
        }
        if (waited < 0 && errno == EINTR) continue;
        if (waited < 0) return std::nullopt;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ::kill(child, SIGKILL);
    ::waitpid(child, &status, 0);
    return std::nullopt;
}

}  // namespace

std::optional<std::string> readSystemClipboard() {
    if (auto text = clipboardCommand({"wl-paste", "--no-newline"}, std::nullopt)) {
        return text;
    }
    return clipboardCommand({"xclip", "-selection", "clipboard", "-out"},
                            std::nullopt);
}

bool writeSystemClipboard(std::string_view text) {
    if (text.empty() || text.size() > 4096) return false;
    return clipboardCommand({"wl-copy"}, text).has_value() ||
           clipboardCommand({"xclip", "-selection", "clipboard", "-in"}, text)
               .has_value();
}
#endif

}  // namespace ovicode
