// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_clipboard_posix.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

#include <chrono>
#include <vector>

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace CoopClipboard {
namespace {

// The first tool this desktop has, at most 2 s (`timeout`, when there is one). %s: the tool to look
// for and its command, for Wayland, then xclip's and xsel's commands. 127: none. Fixed strings only:
// no clipboard text is ever put in them.
constexpr const char* kPickTool =
    "t=; command -v timeout >/dev/null 2>&1 && t='timeout 2'; "
    "if [ -n \"$WAYLAND_DISPLAY\" ] && command -v %s >/dev/null 2>&1; then exec $t %s; "
    "elif [ -n \"$DISPLAY\" ] && command -v xclip >/dev/null 2>&1; then exec $t %s; "
    "elif [ -n \"$DISPLAY\" ] && command -v xsel >/dev/null 2>&1; then exec $t %s; "
    "else exit 127; fi";

thread_local bool t_sdl_thread = false;   // (OnSdlThread)

constexpr std::size_t kMax = 4096;         // what Paste gives back, at most
constexpr std::size_t kRead = 64 * 1024;   // what is read from the tool, at most

enum class Ran {
    kNotStarted,   // no process (no temporary file, pipe or fork)
    kNoTool,       // exit 127: this desktop has none of the tools
    kOk,           // exit 0
    kFailed,       // the tool ran and failed, or was stopped (timeout, the closed pipe)
};

// How long a tool may take, at most: its own `timeout 2` and a second more. Then it is killed, with all it started
// (no `timeout` on this system, or a tool that ignores it): the clipboard's thread never waits for good.
constexpr int kDeadlineMs = 3000;

// The game's open descriptors from 3 up, to be closed in the tool (Linux: /proc/self/fd; else all up to max_fd).
std::vector<int> OpenDescriptors(long max_fd) {
    std::vector<int> fds;
#ifdef __linux__
    if (DIR* dir = opendir("/proc/self/fd")) {
        const int own = dirfd(dir);
        while (const dirent* e = readdir(dir)) {
            const int fd = std::atoi(e->d_name);
            if (fd > 2 && fd != own) {
                fds.push_back(fd);
            }
        }
        closedir(dir);
        return fds;
    }
#endif
    for (long fd = 3; fd < max_fd; ++fd) {
        fds.push_back(static_cast<int>(fd));
    }
    return fds;
}

// `sh -c cmd` with `in`, `out` and `err` as its standard descriptors, none of the game's others, in a process group
// of its own (so a late tool is killed with what it started) and no blocked signals. posix_spawn, not fork: a fork
// copies the whole game's page tables and holds its memory lock meanwhile, which can stall the game's own threads;
// glibc's posix_spawn shares the memory until the exec. -1 when it could not start.
pid_t Spawn(const char* cmd, int in, int out, int err, long max_fd) {
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        return -1;
    }
    if (posix_spawnattr_init(&attr) != 0) {
        posix_spawn_file_actions_destroy(&actions);
        return -1;
    }
    bool ok = posix_spawn_file_actions_adddup2(&actions, in, 0) == 0 &&
              posix_spawn_file_actions_adddup2(&actions, out, 1) == 0 &&
              posix_spawn_file_actions_adddup2(&actions, err, 2) == 0;
    // (closing one that is not open is no error in posix_spawn's closes)
    for (const int fd : OpenDescriptors(max_fd)) {
        ok = ok && posix_spawn_file_actions_addclose(&actions, fd) == 0;
    }
    sigset_t none;
    sigemptyset(&none);
    ok = ok && posix_spawnattr_setpgroup(&attr, 0) == 0 && posix_spawnattr_setsigmask(&attr, &none) == 0 &&
         posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK) == 0;
    pid_t pid = -1;
    if (ok) {
        char sh[] = "sh";
        char dash_c[] = "-c";
        std::string line(cmd);
        char* argv[] = {sh, dash_c, line.data(), nullptr};
        if (posix_spawn(&pid, "/bin/sh", &actions, &attr, argv, environ) != 0) {
            pid = -1;
        }
    }
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    return pid;
}

// Runs the tool (copy: `input` as its standard input, from a file, so a tool that is missing or
// quits never closes a pipe the game writes to: that would end the game with SIGPIPE) and returns
// what it printed, at most kRead bytes (then the pipe is closed, and the tool stops), killed after kDeadlineMs.
// posix_spawn, not popen: wl-copy and xclip leave a process behind that serves the clipboard, and it must keep
// none of the game's descriptors (sockets, the hosted server's port, our pipe) open.
Ran Run(bool copy, const std::string& input, std::string* out) {
    char file[] = "/tmp/oot-coop-clip-XXXXXX";
    if (copy) {
        const int fd = mkstemp(file);
        if (fd < 0) {
            return Ran::kNotStarted;
        }
        const bool written = write(fd, input.data(), input.size()) == static_cast<ssize_t>(input.size());
        close(fd);
        if (!written) {
            unlink(file);
            return Ran::kNotStarted;
        }
    }
    char cmd[1024];
    if (copy) {
        std::snprintf(cmd, sizeof cmd, kPickTool, "wl-copy", "wl-copy", "xclip -selection clipboard -i", "xsel -b -i");
    } else {
        std::snprintf(cmd, sizeof cmd, kPickTool, "wl-paste", "wl-paste -n", "xclip -selection clipboard -o",
                      "xsel -b -o");
    }
    // (close-on-exec: a process another thread starts meanwhile must not hold the pipe's write end, or
    // the read below would wait for that process; our child gets it as its standard output by dup2)
    int pipe_fd[2] = {-1, -1};
    if (!copy) {
#ifdef __linux__
        const bool piped = pipe2(pipe_fd, O_CLOEXEC) == 0;
#else
        const bool piped = pipe(pipe_fd) == 0;
        if (piped) {
            fcntl(pipe_fd[0], F_SETFD, FD_CLOEXEC);
            fcntl(pipe_fd[1], F_SETFD, FD_CLOEXEC);
        }
#endif
        if (!piped) {
            return Ran::kNotStarted;
        }
    }
    long max_fd = sysconf(_SC_OPEN_MAX);
    max_fd = max_fd < 256 ? 256 : (max_fd > 65536 ? 65536 : max_fd);
    const int null_in = open("/dev/null", O_RDONLY | O_CLOEXEC);
    const int null_out = open("/dev/null", O_WRONLY | O_CLOEXEC);
    const int in = copy ? open(file, O_RDONLY | O_CLOEXEC) : null_in;
    pid_t pid = -1;
    if (null_in >= 0 && null_out >= 0 && in >= 0) {
        pid = Spawn(cmd, in, copy ? null_out : pipe_fd[1], null_out, max_fd);
    }
    for (const int fd : {null_in, null_out, copy ? in : -1, pipe_fd[1]}) {
        if (fd >= 0) {
            close(fd);
        }
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kDeadlineMs);
    const auto left_ms = [deadline] {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        return left.count() > 0 ? static_cast<int>(left.count()) : 0;
    };
    bool late = false;   // past the deadline: the tool and what it started are killed
    if (pipe_fd[0] >= 0) {
        if (pid > 0) {
            char buf[512];
            while (out == nullptr || out->size() < kRead) {
                pollfd p{pipe_fd[0], POLLIN, 0};
                const int ready = poll(&p, 1, left_ms());
                if (ready < 0 && errno == EINTR) {
                    continue;
                }
                if (ready == 0) {
                    late = true;
                    break;
                }
                const ssize_t n = ready < 0 ? -1 : read(pipe_fd[0], buf, sizeof buf);
                if (n < 0 && errno == EINTR) {
                    continue;
                }
                if (n <= 0) {
                    break;
                }
                if (out != nullptr) {
                    out->append(buf, std::min(static_cast<std::size_t>(n), kRead - out->size()));
                }
            }
        }
        close(pipe_fd[0]);   // (a tool with more to print gets EPIPE or SIGPIPE and stops)
    }
    int status = -1;
    if (pid > 0) {
        for (;;) {
            const pid_t done = late ? 0 : waitpid(pid, &status, WNOHANG);
            if (done == pid || (done < 0 && errno != EINTR)) {
                break;
            }
            if (late || left_ms() == 0) {
                kill(-pid, SIGKILL);   // (its own process group: the shell, `timeout` and the tool)
                kill(pid, SIGKILL);
                while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
                }
                break;
            }
            usleep(5000);
        }
    }
    if (copy) {
        unlink(file);
    }
    if (pid <= 0) {
        return Ran::kNotStarted;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
        return Ran::kNoTool;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? Ran::kOk : Ran::kFailed;
}

std::optional<std::string> ToolPaste() {
    std::string out;
    const Ran ran = Run(false, {}, &out);
    if (ran == Ran::kNotStarted || ran == Ran::kNoTool) {
        return std::nullopt;
    }
    return out;   // (kFailed is believed: what the tool printed stands, "" for an empty clipboard)
}

// SDL's clipboard (SDL_GetClipboardText, SDL2 and SDL3 alike: a string to SDL_free), at most kRead bytes;
// nothing without SDL. Only on SDL's thread.
std::optional<std::string> SdlPaste() {
    using GetText = char* (*)();
    using Free = void (*)(void*);
    const auto get = reinterpret_cast<GetText>(GameSdl("SDL_GetClipboardText"));
    const auto free_fn = reinterpret_cast<Free>(GameSdl("SDL_free"));
    if (get == nullptr || free_fn == nullptr) {
        return std::nullopt;
    }
    char* text = get();
    if (text == nullptr) {
        return std::nullopt;
    }
    std::string out(text, strnlen(text, kRead));
    free_fn(text);
    return out;
}

// Trailing line ends dropped, at most kMax bytes (what Paste gives).
std::optional<std::string> Trimmed(std::optional<std::string> got) {
    if (!got) {
        return std::nullopt;
    }
    std::string out = std::move(*got);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
        out.pop_back();
    }
    if (out.size() > kMax) {
        out.resize(kMax);
    }
    return out;
}

// The clipboard's own thread (CopyLater, PasteLater): started with the first job; at unload it is told to stop
// and waited for (at most one tool's 2 s), so it never runs code that is gone. Jobs still queued then are dropped.
class Worker {
public:
    std::uint64_t Add(bool copy, std::string text, bool keep) {
        std::lock_guard lock(mutex_);
        const std::uint64_t ticket = ++last_;
        jobs_.push_back(Job{ticket, copy, keep, std::move(text)});
        if (!thread_.joinable()) {
            thread_ = std::thread([this] { Work(); });
        }
        wake_.notify_one();
        return ticket;
    }

    std::optional<Done> Take(std::uint64_t ticket) {
        std::lock_guard lock(mutex_);
        const auto it = done_.find(ticket);
        if (it == done_.end()) {
            return std::nullopt;
        }
        Done d = std::move(it->second);
        done_.erase(it);
        return d;
    }

    ~Worker() {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        wake_.notify_one();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    struct Job {
        std::uint64_t ticket = 0;
        bool copy = false;
        bool keep = false;
        std::string text;
    };

    void Work() {
        for (;;) {
            Job job;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
                if (stop_) {
                    return;
                }
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            Done d;
            if (job.copy) {
                d.ok = Run(true, job.text, nullptr) == Ran::kOk;
            } else {
                d.text = Trimmed(ToolPaste());
                d.ok = d.text.has_value();
            }
            if (job.keep) {
                std::lock_guard lock(mutex_);
                done_[job.ticket] = std::move(d);
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> jobs_;
    std::map<std::uint64_t, Done> done_;
    std::uint64_t last_ = 0;
    bool stop_ = false;
    std::thread thread_;
};

Worker& TheWorker() {
    static Worker worker;
    return worker;
}

}  // namespace

bool Copy(const std::string& text) {
    return Run(true, text, nullptr) == Ran::kOk;
}

void* GameSdl(const char* name) {
    // (NOLOAD: only the library the game has loaded, found by its soname; kept once found)
    static std::atomic<void*> lib{nullptr};
    void* h = lib.load(std::memory_order_acquire);
    if (h == nullptr) {
        h = dlopen("libSDL3.so.0", RTLD_NOW | RTLD_NOLOAD);
        if (h == nullptr) {
            h = dlopen("libSDL2-2.0.so.0", RTLD_NOW | RTLD_NOLOAD);
        }
        if (h == nullptr) {
            return nullptr;
        }
        void* expected = nullptr;
        if (!lib.compare_exchange_strong(expected, h, std::memory_order_acq_rel)) {
            dlclose(h);   // (another thread kept one first)
            h = expected;
        }
    }
    return dlsym(h, name);
}

OnSdlThread::OnSdlThread() : was_(t_sdl_thread) {
    t_sdl_thread = true;
}

OnSdlThread::~OnSdlThread() {
    t_sdl_thread = was_;
}

std::optional<std::string> PickPaste(const std::optional<std::string>& sdl,
                                     const std::function<std::optional<std::string>()>& tools) {
    if (sdl && !sdl->empty()) {
        return sdl;
    }
    std::optional<std::string> got = tools();
    return got ? got : sdl;
}

std::optional<std::string> Paste() {
    return Trimmed(PickPaste(t_sdl_thread ? SdlPaste() : std::nullopt, ToolPaste));
}

std::uint64_t CopyLater(std::string text, bool keep) {
    return TheWorker().Add(true, std::move(text), keep);
}

std::uint64_t PasteLater() {
    return TheWorker().Add(false, {}, true);
}

std::optional<std::string> PasteNow() {
    if (!t_sdl_thread) {
        return std::nullopt;
    }
    std::optional<std::string> got = Trimmed(SdlPaste());
    if (got && got->empty()) {
        return std::nullopt;
    }
    return got;
}

std::optional<Done> Take(std::uint64_t ticket) {
    return TheWorker().Take(ticket);
}

}  // namespace CoopClipboard
