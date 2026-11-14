// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_clipboard_posix.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

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

constexpr std::size_t kMax = 4096;         // what Paste gives back, at most
constexpr std::size_t kRead = 64 * 1024;   // what is read from the tool, at most

enum class Ran {
    kNotStarted,   // no process (no temporary file, pipe or fork)
    kNoTool,       // exit 127: this desktop has none of the tools
    kOk,           // exit 0
    kFailed,       // the tool ran and failed, or was stopped (timeout, the closed pipe)
};

// Runs the tool (copy: `input` as its standard input, from a file, so a tool that is missing or
// quits never closes a pipe the game writes to: that would end the game with SIGPIPE) and returns
// what it printed, at most kRead bytes (then the pipe is closed, and the tool stops). fork + exec,
// not popen: wl-copy and xclip leave a process behind that serves the clipboard, and it must keep
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
        pid = fork();
    }
    if (pid == 0) {   // (the child: only async-signal-safe calls until exec)
        dup2(in, 0);
        dup2(copy ? null_out : pipe_fd[1], 1);
        dup2(null_out, 2);
        for (long fd = 3; fd < max_fd; ++fd) {
            close(static_cast<int>(fd));
        }
        execl("/bin/sh", "sh", "-c", cmd, static_cast<char*>(nullptr));
        _exit(127);
    }
    for (const int fd : {null_in, null_out, copy ? in : -1, pipe_fd[1]}) {
        if (fd >= 0) {
            close(fd);
        }
    }
    if (pipe_fd[0] >= 0) {
        if (pid > 0) {
            char buf[512];
            while (out == nullptr || out->size() < kRead) {
                const ssize_t n = read(pipe_fd[0], buf, sizeof buf);
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
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
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

}  // namespace

bool Copy(const std::string& text) {
    return Run(true, text, nullptr) == Ran::kOk;
}

std::optional<std::string> Paste() {
    std::string out;
    const Ran ran = Run(false, {}, &out);
    if (ran == Ran::kNotStarted || ran == Ran::kNoTool) {
        return std::nullopt;
    }
    // (kFailed is believed: what the tool printed stands, "" for an empty clipboard)
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
        out.pop_back();
    }
    if (out.size() > kMax) {
        out.resize(kMax);
    }
    return out;
}

}  // namespace CoopClipboard
