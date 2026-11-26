#include "coop_camera.h"

#include <algorithm>
#include <atomic>
#include <mutex>

namespace CoopCamera {
namespace {

std::atomic<bool> g_wanted{false};       // the hook: name tags while Ocarina runs with co-op on
std::atomic<bool> g_tool_wanted{false};  // the play-test bridge's `move` (coop_playtest.cpp)
std::mutex g_mutex;
std::optional<Frame> g_pending;  // the frame the renderer is drawing
std::optional<Frame> g_latest;   // the last finished one

// Forgets the camera once neither side wants it, so a stale one is never steered or drawn by.
void Turned(std::atomic<bool>& mine, const std::atomic<bool>& other, bool wanted) {
    if (mine.exchange(wanted) && !wanted && !other.load()) {
        std::lock_guard lock(g_mutex);
        g_pending.reset();
        g_latest.reset();
    }
}

} // namespace

void SetWanted(bool wanted) {
    Turned(g_wanted, g_tool_wanted, wanted);
}

void SetToolWanted(bool wanted) {
    Turned(g_tool_wanted, g_wanted, wanted);
}

bool Wanted() {
    return g_wanted.load(std::memory_order_relaxed) || g_tool_wanted.load(std::memory_order_relaxed);
}

bool Better(std::uint64_t frame, float width, float height) {
    std::lock_guard lock(g_mutex);
    return !g_pending || g_pending->frame != frame ||
           width * height > g_pending->width * g_pending->height;
}

void Publish(std::uint64_t frame, const float* clip_from_world, float width, float height,
             bool y_down) {
    Frame f;
    std::copy_n(clip_from_world, 16, f.clip_from_world.begin());
    f.width = width;
    f.height = height;
    f.y_down = y_down;
    f.frame = frame;
    f.at = std::chrono::steady_clock::now();
    std::lock_guard lock(g_mutex);
    if (g_pending && g_pending->frame != frame) {
        g_latest = g_pending;  // a new frame began: the last one is finished
    }
    g_pending = f;
}

std::optional<Frame> Latest() {
    std::lock_guard lock(g_mutex);
    return g_latest;
}

} // namespace CoopCamera
