#pragma once
// What CoopHook::OnFrame runs each frame, and the ended session's one last frame (coop_hook.cpp). Header only, so
// the unit tests hold it.
#include <utility>

namespace CoopSessionEnd {

// The frame's path: a session's frame; solo (LocalFrameOff, the game's own state mended); or, with a code mod
// (issue 136), solo reading only (the Customize picture), nothing written to the game.
enum class Path { session, solo, read_only };
constexpr Path PathOf(bool session, bool refused) {
    return refused ? Path::read_only : session ? Path::session : Path::solo;
}

// A menu leave (End, on the menu's thread) clears the session first and keeps the stopped one a moment later. Every
// part forgets the session on a frame that is not connected, as on a drop, so the ended session gets one such frame,
// once it is kept (its client stopped). Never with a code mod (that frame writes to the game). The frame's thread.
template <class Session>
class LastFrame {
public:
    struct Change {
        bool started = false;
        bool ended = false;
    };
    // This frame's session (none: no session).
    Change Note(Session* now) {
        Change c;
        if (now != nullptr) {
            seen_ = now;
            due_ = nullptr;
        }
        if ((now != nullptr) != had_) {
            had_ = now != nullptr;
            (had_ ? c.started : c.ended) = true;
            if (!had_) {
                due_ = seen_;
            }
        }
        return c;
    }
    // The session to give its last frame now, or none: `kept` is the last one End kept.
    Session* Take(Session* kept, bool refused) {
        if (due_ != nullptr && !refused && kept == due_) {
            return std::exchange(due_, nullptr);
        }
        return nullptr;
    }

private:
    bool had_ = false;
    Session* seen_ = nullptr;
    Session* due_ = nullptr;
};

}  // namespace CoopSessionEnd
