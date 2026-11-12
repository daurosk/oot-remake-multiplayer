// SPDX-License-Identifier: GPL-3.0-or-later
//
// Proximity voice chat in the game (docs/voice.md). The voice itself is
// coop/voice (game-free); this ties it to the hook: our microphone out as
// VOICE_FRAME, the others' frames in, each voice placed at that player's
// Link as presence shows it, heard from the game's camera. The microphone,
// Opus and the game's mixer come from a Backend the host game hands in
// (Hookshot's module: hookshot/src/coop_voice_hookshot.cpp). Without one
// (the emulator) voice is off and nothing here does anything.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "coop/voice/engine.hpp"

namespace coop::host {
class Session;
}
namespace coop::net {
struct Event;
}
namespace coop::overlay {
struct Shown;
}

namespace CoopVoice {

struct Backend {
    std::function<bool()> mic_start;
    std::function<int(std::int16_t* out, int max)> mic_read;
    std::function<void()> mic_stop;
    coop::voice::CodecFactory codecs;
    std::function<std::uint64_t()> now_us;          // the mixer's clock
    std::function<bool(std::uint32_t vk)> key_down;  // a key held in the game's window
};

// What the player chooses: LICHII_COOP_VOICE* at start (coop-options.ini),
// then the game's own Co-op Settings screen (Set).
struct Settings {
    coop::voice::Mode mode = coop::voice::Mode::push_to_talk;
    std::uint32_t key = 0x56;   // push to talk: a Windows virtual-key code ('V')
    float volume = 1.0f;        // the others' voices, 0 to 2
    float range_m = 30.0f;      // heard up to here (the server sends a little further)
    bool muted = false;         // our microphone
    bool deafened = false;      // the others' voices
    bool operator==(const Settings&) const = default;
};

// LICHII_COOP_VOICE (off, ptt, open; default ptt), LICHII_COOP_VOICE_KEY (a
// virtual-key code, decimal or 0x hex, or one letter or digit),
// LICHII_COOP_VOICE_VOLUME (0 to 200, percent), LICHII_COOP_VOICE_RANGE
// (metres, 5 to 100). `env` gives a variable's value, empty when unset.
// Anything it cannot read stays at the default and is named in `errors`.
Settings SettingsFrom(const std::function<std::string(const char*)>& env,
                      std::vector<std::string>& errors);
// The engine's settings for these.
coop::voice::Settings EngineSettings(const Settings& s, bool camera_known);

// Whether our microphone is open at all (privacy: no live microphone the
// player did not ask for). Never when we cannot send (not connected, not in
// the game), voice is off or we are muted. Push to talk: only while the key
// is held, and while the spurt it started is still ending (`talking`: the
// gate's 100 ms tail and its end frame); a player who never presses the key
// never has the microphone opened. Open mic: always, as asked.
bool MicWanted(const Settings& s, bool can_send, bool key_held, bool talking);

// Where a voice comes from: a little under the top of the head (`head`
// above the feet, as presence has it), or 1.2 m up when that is not known.
coop::voice::Vec3 MouthOf(const std::array<float, 3>& feet, float head);

// --- the game ---
// The host game's backend, once (any thread; a second is ignored). Voice
// works from then on.
void SetBackend(Backend backend);
bool Available();
Settings Get();
void Set(const Settings& settings);

// The network thread: VOICE_FRAME in, a player gone.
void OnEvent(const coop::net::Event& event);
// The game thread, every frame: our Link (none: not in the game), the
// others as presence shows them and the ones hidden (another scene, our
// cutscene), and the session to send on (none: not connected).
void Frame(const std::optional<std::array<float, 3>>& our_feet,
           const std::vector<coop::overlay::Shown>& others, const std::vector<std::uint8_t>& hidden,
           coop::host::Session* session);
// The game's mixer thread (hos_coop_voice.h): the voices into its buffers.
void Render(float* const* out, int channels, int frames);
// Any thread: a player's voice plays here now; we are talking now.
bool Speaking(std::uint8_t player);
bool Talking();

} // namespace CoopVoice
