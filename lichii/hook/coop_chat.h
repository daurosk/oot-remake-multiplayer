// The chat lines the co-op window shows: received chat, our own sent chat
// and room notices ("Saria joined"). The network thread adds, the Qt window
// reads; header only, standard C++ only.
#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace CoopChat {

using Clock = std::chrono::steady_clock;

// How many lines are kept; older ones are dropped.
inline constexpr std::size_t kKeep = 6;

struct Line {
    std::string text;
    bool notice = false;  // a join, leave or connection line, not chat
    Clock::time_point at{};
};

class Log {
public:
    void Add(std::string text, bool notice) {
        std::lock_guard lock(mutex_);
        lines_.push_back({std::move(text), notice, Clock::now()});
        while (lines_.size() > kKeep) {
            lines_.pop_front();
        }
    }

    // The kept lines, oldest first.
    std::vector<Line> Lines() const {
        std::lock_guard lock(mutex_);
        return {lines_.begin(), lines_.end()};
    }

private:
    mutable std::mutex mutex_;
    std::deque<Line> lines_;
};

} // namespace CoopChat
