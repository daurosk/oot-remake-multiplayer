// SPDX-License-Identifier: GPL-3.0-or-later
//
// Which of the DLL's frames reach the shared enemies (Objects::Frame), and
// with which scene.
//
// The hook's sharing forgets everything when it is told "no scene" (a load,
// the title screen): the ids it gave its rolled enemies, who owns what, the
// copies it made. That is right for a real scene change. It is wrong for a
// frame in which the player merely could not be read:
// - the game's thread made no frame for a second (a hitch: another program
//   took the CPU, a long shader build, the window dragged). The DLL's own
//   thread then makes the session's frame with no reading, so the others see
//   "not in game"; nothing in the game moved meanwhile;
// - a single frame of the game's thread in which the reading failed.
// Seen in run s1 (two games, the build of this very DLL running beside them):
// seven such frames in a minute. After each one the host's game shared its
// three Stalchildren again under new ids, the joiner's game made a second and
// a third copy of each, and the copies of the old ids, whose owner had fallen
// silent, were given to the joiner, who then fought enemies that existed in no
// other game.
//
// So: a frame with no game thread behind it never reaches the objects once a
// scene was known (their state stays as the stopped game's), and a frame of
// the game's thread without a scene is left out for kBlipUs before "no scene"
// is passed on. A real load lasts longer than that and is passed on as before.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace CoopNx2navi::Objects {

class SceneGate {
public:
    static constexpr std::uint64_t kBlipUs = 500'000;

    // `game_frame`: the frame was made on the game's thread (the game runs).
    // `scene`: the player's scene as read this frame, if any. False: leave the
    // objects' frame out. True: call it with `out` as the scene.
    bool Pass(bool game_frame, const std::optional<std::string>& scene, std::uint64_t now_us,
              std::optional<std::string>& out) {
        if (scene) {
            last_ = scene;
            last_us_ = now_us;
            out = scene;
            return true;
        }
        if (last_) {
            if (!game_frame) {
                ++held_;
                return false;  // the game stands still: so does what is shared
            }
            if (now_us - last_us_ < kBlipUs) {
                ++held_;
                return false;  // one unreadable frame is not a load
            }
            last_.reset();
        }
        out.reset();
        return true;
    }

    // Frames left out since the last ask (for the status line).
    std::uint64_t TakeHeld() {
        const std::uint64_t n = held_;
        held_ = 0;
        return n;
    }

private:
    std::optional<std::string> last_;
    std::uint64_t last_us_ = 0;
    std::uint64_t held_ = 0;
};

}  // namespace CoopNx2navi::Objects
