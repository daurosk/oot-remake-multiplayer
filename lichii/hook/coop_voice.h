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

// LICHII_COOP_VOICE_TEST, for test runs with nobody at the microphone
// (docs/voice.md, "Testing"): `log` says in the log who is heard here, `tone`
// also sends a tone instead of the microphone.
enum class Test : std::uint8_t { none, log, tone };

// What the player chooses: LICHII_COOP_VOICE* at start (coop-options.ini),
// then the game's own co-op settings (Set, SetEnabled).
struct Settings {
    // Off until the player turns voice chat on: no microphone, nothing heard.
    coop::voice::Mode mode = coop::voice::Mode::off;
    std::uint32_t key = 0x56;   // push to talk: a Windows virtual-key code ('V')
    float volume = 1.0f;        // the others' voices, 0 to 2
    float range_m = 30.0f;      // heard up to here (the server sends a little further)
    bool muted = false;         // our microphone
    bool deafened = false;      // the others' voices
    Test test = Test::none;     // the start's only: Set keeps it
    bool operator==(const Settings&) const = default;
};

// LICHII_COOP_VOICE (off, the default; open: open mic, what the menu writes
// when voice is turned on; ptt, or an older file's on, 1, yes: push to talk), LICHII_COOP_VOICE_KEY (a virtual-key code, decimal or 0x hex, or one
// letter or digit), LICHII_COOP_VOICE_VOLUME (0 to 200, percent),
// LICHII_COOP_VOICE_RANGE (metres, 5 to 100), LICHII_COOP_VOICE_TEST (log,
// tone). `env` gives a variable's value, empty when unset. Anything it
// cannot read stays at the default and is named in `errors`.
Settings SettingsFrom(const std::function<std::string(const char*)>& env,
                      std::vector<std::string>& errors);
// The engine's settings for these. Off also plays nobody's voice.
coop::voice::Settings EngineSettings(const Settings& s, bool camera_known);
// The mode as LICHII_COOP_VOICE takes it: "off", "ptt" or "open" (what a
// settings screen writes to coop-options.ini).
const char* ModeName(coop::voice::Mode mode);
// The settings screen's On/Off row. On: open mic, or push to talk when that
// was already chosen; off: off. The rest stays.
Settings WithEnabled(Settings s, bool on);
// The push-to-talk key as the player reads it: "V", "7", else "the
// push-to-talk key".
std::string KeyName(std::uint32_t vk);
// The chat's tip when voice chat is turned on (once a run): "Voice chat on:
// open mic (change to push-to-talk in Settings)"; "" when off.
std::string TurnedOnTip(const Settings& s);

// The voice icon by a name in the player HUD (docs/voice.md, "Who is
// talking"). Voice off here: none for anyone (nothing is heard or sent).
// Muted (ours: our microphone; another's: muted by us or the host): the
// muted icon. Talking (ours: we send; another's: their voice plays here, held
// 300 ms after the last frame): lit. Else ours is dim (voice on, quiet) and
// another's none (whether their voice is on is not known here).
enum class Icon : std::uint8_t { none, idle, talking, muted };
Icon IconFor(bool voice_on, bool muted, bool active, bool self);

// Whether our microphone is open at all (privacy: no live microphone the
// player did not ask for). Never when we cannot send (not connected, not in
// the game), voice is off or we are muted. Push to talk: only while the key
// is held, and while the spurt it started is still ending (`talking`: the
// gate's 100 ms tail and its end frame); a player who never presses the key
// never has the microphone opened. Open mic: always, as asked. Never while
// the host has muted our voice (the server would drop it anyway).
bool MicWanted(const Settings& s, bool can_send, bool key_held, bool talking, bool host_muted = false);

// Where a voice comes from: a little under the top of the head (`head`
// above the feet, as presence has it), or 1.2 m up when that is not known.
coop::voice::Vec3 MouthOf(const std::array<float, 3>& feet, float head);

// The test tone (LICHII_COOP_VOICE_TEST=tone): what a microphone would give,
// 48 kHz mono, as time passes on the caller's clock; a 440 Hz sine at a
// quarter of full scale. A game that stood still gets at most the last
// 200 ms, not the whole gap.
class ToneSource {
public:
    static constexpr std::uint64_t kRate = 48000;
    static constexpr double kToneHz = 440.0;
    static constexpr double kToneLevel = 8192.0;
    static constexpr std::uint64_t kMaxBehind = 9600;       // samples
    static constexpr std::uint64_t kSpurtUs = 2'000'000;    // talks this long, then is quiet as long

    void start(std::uint64_t now_us);
    int read(std::int16_t* out, int max, std::uint64_t now_us);
    // The tone's push-to-talk key: held for kSpurtUs, let go for kSpurtUs.
    static bool KeyHeld(std::uint64_t now_us);

private:
    std::uint64_t start_us_ = 0;
    std::uint64_t produced_ = 0;
    double phase_ = 0.0;
};
// Puts the tone in the microphone's place and its key in the push-to-talk
// key's: the backend's own microphone is then never opened, and the key does
// not need the game's window in front (two test games on one PC).
void UseTestTone(Backend& backend, std::function<std::uint64_t()> clock_us);
// The test log's line when a player starts or stops being heard here:
// "P1 heard at 2.4 m", "P1 not heard (out of range: 24.5 m, range 10 m)",
// "P1 not heard (quiet, 2.4 m)", "P1 not heard (not shown here)".
std::string HeardLine(std::uint8_t player, bool heard, std::optional<float> distance_m, float range_m);

// --- the game ---
// The host game's backend, once (any thread; a second is ignored). Voice
// works from then on.
void SetBackend(Backend backend);
bool Available();
Settings Get();
// Takes effect at once (the microphone closes within a frame when voice is
// turned off). The caller keeps the choice (coop-options.ini).
void Set(const Settings& settings);
// The settings screen's On/Off row: Get().mode != off, Set(WithEnabled).
bool Enabled();
void SetEnabled(bool on);
// The Voice mode row (and On/Off together): off, open mic or push to talk.
void SetMode(coop::voice::Mode mode);
// The game thread: whether the host has muted our voice (CoopLobby); the
// microphone closes while it has.
void SetHostMuted(bool muted);
// The turned-on tip once, for the chat as a notice; "" when there is none.
std::string TakeNotice();

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
