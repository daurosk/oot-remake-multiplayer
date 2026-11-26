#pragma once
// Our own Link with no session (co-op idle: no Host or Join), for what is not multiplayer but reads him through
// CoopHook::CurrentMapView: the Customize page's live 3D picture (row 112, #74: with no session the picture was the
// flat one, because only a session's frame filled the view). LocalFrameOff publishes each frame what it reads of
// the first Link; CurrentMapView falls back to it when there is no session. The same facts a session's frame gives
// (scene, place, age, our own event), no other players. Header only, so the unit tests hold it.
#include <array>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>

namespace CoopSoloView {

using Clock = std::chrono::steady_clock;
// As a session's frame (coop_hook.cpp kFrameStale): an older reading is no Link (the game stopped or left).
inline constexpr auto kStale = std::chrono::seconds(1);

// One frame's reading of our Link with no session. No position or no scene: no Link this frame (the title, a load).
struct Reading {
    std::optional<std::array<float, 3>> position;
    std::optional<std::string> scene;
    bool adult = false;
    bool event = false;  // our own event hides (coop/host/scene.hpp local_event_hides)
};

// What CurrentMapView hands out from a reading (CoopHook::MapView's own fields, no others).
struct View {
    std::string scene;
    std::array<float, 3> self{};
    bool adult = false;
    bool event = false;
};

// The last reading and when. Any thread.
class Store {
public:
    void Publish(const Reading& r, Clock::time_point now) {
        std::lock_guard lock(mutex_);
        if (!r.position || !r.scene) {
            has_ = false;
            return;
        }
        has_ = true;
        at_ = now;
        view_.scene.assign(*r.scene);
        view_.self = *r.position;
        view_.adult = r.adult;
        view_.event = r.event;
    }
    // A session started: its frame is the one shown, and this one is old once the session ends.
    void Clear() {
        std::lock_guard lock(mutex_);
        has_ = false;
    }
    // False (and `out` untouched) with no Link read in the last kStale.
    bool Get(View& out, Clock::time_point now) const {
        std::lock_guard lock(mutex_);
        if (!has_ || now - at_ >= kStale) {
            return false;
        }
        out.scene.assign(view_.scene);
        out.self = view_.self;
        out.adult = view_.adult;
        out.event = view_.event;
        return true;
    }

private:
    mutable std::mutex mutex_;
    bool has_ = false;
    Clock::time_point at_{};
    View view_;
};

}  // namespace CoopSoloView
