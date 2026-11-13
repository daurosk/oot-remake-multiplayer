// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_voice.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace CoopVoice {

namespace {

std::optional<long> Number(const std::string& text) {
    if (text.empty()) {
        return std::nullopt;
    }
    char* end = nullptr;
    const long v = std::strtol(text.c_str(), &end, 0);  // decimal or 0x hex
    if (end == nullptr || *end != '\0') {
        return std::nullopt;
    }
    return v;
}

std::string Lower(std::string s) {
    for (auto& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

} // namespace

Settings SettingsFrom(const std::function<std::string(const char*)>& env,
                      std::vector<std::string>& errors) {
    Settings s;
    if (const std::string mode = Lower(env("LICHII_COOP_VOICE")); !mode.empty()) {
        if (mode == "off" || mode == "0" || mode == "no" || mode == "false") {
            s.mode = coop::voice::Mode::off;
        } else if (mode == "ptt" || mode == "push" || mode == "push-to-talk" || mode == "1" ||
                   mode == "on" || mode == "yes" || mode == "true") {  // "on": the menu's On row
            s.mode = coop::voice::Mode::push_to_talk;
        } else if (mode == "open" || mode == "open-mic") {
            s.mode = coop::voice::Mode::open_mic;
        } else {
            errors.push_back("LICHII_COOP_VOICE '" + mode + "' is not off, ptt or open");
        }
    }
    if (const std::string key = env("LICHII_COOP_VOICE_KEY"); !key.empty()) {
        if (key.size() == 1 && std::isalnum(static_cast<unsigned char>(key[0]))) {
            s.key = static_cast<std::uint32_t>(std::toupper(static_cast<unsigned char>(key[0])));
        } else if (const auto v = Number(key); v && *v > 0 && *v < 256) {
            s.key = static_cast<std::uint32_t>(*v);
        } else {
            errors.push_back("LICHII_COOP_VOICE_KEY '" + key + "' is not a letter, a digit or a key code");
        }
    }
    if (const std::string volume = env("LICHII_COOP_VOICE_VOLUME"); !volume.empty()) {
        if (const auto v = Number(volume); v && *v >= 0 && *v <= 200) {
            s.volume = static_cast<float>(*v) / 100.0f;
        } else {
            errors.push_back("LICHII_COOP_VOICE_VOLUME '" + volume + "' is not 0 to 200");
        }
    }
    if (const std::string range = env("LICHII_COOP_VOICE_RANGE"); !range.empty()) {
        if (const auto v = Number(range); v && *v >= 5 && *v <= 100) {
            s.range_m = static_cast<float>(*v);
        } else {
            errors.push_back("LICHII_COOP_VOICE_RANGE '" + range + "' is not 5 to 100 (metres)");
        }
    }
    if (const std::string test = Lower(env("LICHII_COOP_VOICE_TEST")); !test.empty()) {
        if (test == "tone") {
            s.test = Test::tone;
        } else if (test == "log" || test == "1") {
            s.test = Test::log;
        } else if (test != "0" && test != "off") {
            errors.push_back("LICHII_COOP_VOICE_TEST '" + test + "' is not log or tone");
        }
    }
    return s;
}

const char* ModeName(coop::voice::Mode mode) {
    switch (mode) {
    case coop::voice::Mode::push_to_talk:
        return "ptt";
    case coop::voice::Mode::open_mic:
        return "open";
    default:
        return "off";
    }
}

Settings WithEnabled(Settings s, bool on) {
    if (!on) {
        s.mode = coop::voice::Mode::off;
    } else if (s.mode == coop::voice::Mode::off) {
        s.mode = coop::voice::Mode::push_to_talk;
    }
    return s;
}

coop::voice::Settings EngineSettings(const Settings& s, bool camera_known) {
    coop::voice::Settings e;
    e.gate.mode = s.mode;
    e.muted = s.muted;
    e.deafened = s.deafened || s.mode == coop::voice::Mode::off;  // off: nothing heard either
    e.volume = std::clamp(s.volume, 0.0f, 2.0f);
    e.spatial.range_m = std::clamp(s.range_m, 5.0f, 100.0f);
    e.spatial.fade_m = std::min(6.0f, e.spatial.range_m / 3.0f);
    e.spatial.pan = camera_known;
    return e;
}

bool MicWanted(const Settings& s, bool can_send, bool key_held, bool talking) {
    if (!can_send || s.muted) {
        return false;
    }
    switch (s.mode) {
    case coop::voice::Mode::open_mic:
        return true;
    case coop::voice::Mode::push_to_talk:
        return key_held || talking;
    default:
        return false;
    }
}

coop::voice::Vec3 MouthOf(const std::array<float, 3>& feet, float head) {
    const float up = head > 0.3f ? head * 0.9f : 1.2f;
    return {feet[0], feet[1] + up, feet[2]};
}

void ToneSource::start(std::uint64_t now_us) {
    start_us_ = now_us;
    produced_ = 0;
}

int ToneSource::read(std::int16_t* out, int max, std::uint64_t now_us) {
    if (out == nullptr || max <= 0 || now_us < start_us_) {
        return 0;
    }
    const std::uint64_t total = (now_us - start_us_) * kRate / 1'000'000;
    std::uint64_t due = total > produced_ ? total - produced_ : 0;
    if (due > kMaxBehind) {  // the game stood still: do not play the gap late
        produced_ += due - kMaxBehind;
        due = kMaxBehind;
    }
    const int n = static_cast<int>(std::min<std::uint64_t>(due, static_cast<std::uint64_t>(max)));
    constexpr double kTurn = 2.0 * 3.14159265358979323846;
    constexpr double kStep = kTurn * kToneHz / kRate;
    for (int i = 0; i < n; ++i) {
        out[i] = static_cast<std::int16_t>(kToneLevel * std::sin(phase_));
        phase_ += kStep;
        if (phase_ > kTurn) {
            phase_ -= kTurn;
        }
    }
    produced_ += static_cast<std::uint64_t>(n);
    return n;
}

bool ToneSource::KeyHeld(std::uint64_t now_us) { return (now_us / kSpurtUs) % 2 == 0; }

void UseTestTone(Backend& backend, std::function<std::uint64_t()> clock_us) {
    auto tone = std::make_shared<ToneSource>();
    backend.mic_start = [tone, clock_us] {
        tone->start(clock_us());
        return true;
    };
    backend.mic_read = [tone, clock_us](std::int16_t* out, int max) { return tone->read(out, max, clock_us()); };
    backend.mic_stop = [] {};
    backend.key_down = [clock_us](std::uint32_t) { return ToneSource::KeyHeld(clock_us()); };
}

std::string HeardLine(std::uint8_t player, bool heard, std::optional<float> distance_m, float range_m) {
    char text[96];
    const unsigned p = player;
    if (heard) {
        std::snprintf(text, sizeof text, "P%u heard at %.1f m", p, static_cast<double>(distance_m.value_or(0.0f)));
    } else if (!distance_m) {
        std::snprintf(text, sizeof text, "P%u not heard (not shown here)", p);
    } else if (*distance_m >= range_m) {
        std::snprintf(text, sizeof text, "P%u not heard (out of range: %.1f m, range %.0f m)", p,
                      static_cast<double>(*distance_m), static_cast<double>(range_m));
    } else {
        std::snprintf(text, sizeof text, "P%u not heard (quiet, %.1f m)", p, static_cast<double>(*distance_m));
    }
    return text;
}

} // namespace CoopVoice

#ifdef LICHII_COOP

#include <atomic>
#include <chrono>
#include <mutex>

#include "common/logging.h"
#include "coop/host/session.hpp"
#include "coop/net/client.hpp"
#include "coop/overlay/overlay.hpp"
#include "coop/protocol/messages.hpp"
#include "coop/voice/spatial.hpp"
#include "coop_camera.h"

namespace CoopVoice {

namespace {

constexpr std::size_t kMaxSpeakers = coop::voice::kMaxSpeakers;

std::mutex g_mutex;  // g_backend, g_settings, the mic state
Backend g_backend;
std::atomic<coop::voice::Engine*> g_engine{nullptr};  // made once, never freed (the mixer may be in it)
Settings g_settings;
bool g_settings_read = false;
bool g_camera_known = false;
bool g_mic_on = false;
std::atomic<std::uint64_t> g_mic_said{0};
std::atomic<bool> g_mic_opened{false};

std::uint64_t SteadyUs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// LICHII_COOP_VOICE_TEST (the game thread only): who is heard here, said
// when it changes, and the engine's counts every 2 s while they move, so a
// two-game test run can be read from the logs alone.
struct TestLog {
    std::array<bool, kMaxSpeakers + 1> heard{};
    std::uint64_t counts_at_us = 0;
    coop::voice::Engine::Stats counts;
};
TestLog g_test_log;

std::string EnvOf(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr ? std::string(v) : std::string();
}

void ReadSettingsOnce() {  // under g_mutex
    if (g_settings_read) {
        return;
    }
    g_settings_read = true;
    std::vector<std::string> errors;
    g_settings = SettingsFrom(EnvOf, errors);
    for (const auto& e : errors) {
        LOG_WARNING(Core, "coop: voice: {}", e);
    }
}

// g_backend is set once, before g_engine (SetBackend): once the engine is
// there, its clock may be read without the lock.
std::uint64_t Now() {
    return g_engine.load() != nullptr && g_backend.now_us ? g_backend.now_us() : 0;
}

void LogTest(coop::voice::Engine& e, const std::array<std::optional<float>, kMaxSpeakers + 1>& distance,
             float range_m, const char* mode) {
    const std::uint64_t now = Now();
    for (std::uint8_t p = 1; p <= kMaxSpeakers; ++p) {
        const bool heard = e.speaking(p, now);
        if (heard != g_test_log.heard[p]) {
            g_test_log.heard[p] = heard;
            LOG_INFO(Core, "coop: voice: test: {}", HeardLine(p, heard, distance[p], range_m));
        }
    }
    const std::uint64_t steady = SteadyUs();
    if (steady - g_test_log.counts_at_us < 2'000'000) {
        return;
    }
    g_test_log.counts_at_us = steady;
    const auto s = e.stats();
    if (s.sent != g_test_log.counts.sent || s.received != g_test_log.counts.received ||
        s.unplaced != g_test_log.counts.unplaced) {
        LOG_INFO(Core, "coop: voice: test: {}: sent {} received {} decoded {} concealed {} unplaced {}", mode,
                 s.sent, s.received, s.decoded, s.concealed, s.unplaced);
    }
    g_test_log.counts = s;
}

} // namespace

void SetBackend(Backend backend) {
    std::lock_guard lock(g_mutex);
    ReadSettingsOnce();
    if (g_engine.load() != nullptr) {
        LOG_WARNING(Core, "coop: voice: a second backend ignored");
        return;
    }
    g_backend = std::move(backend);
    if (g_settings.test == Test::tone) {
        UseTestTone(g_backend, SteadyUs);
        LOG_INFO(Core, "coop: voice: test: a tone instead of the microphone, 2 s on and 2 s off "
                       "(LICHII_COOP_VOICE_TEST=tone); the microphone is never opened");
    }
    g_engine.store(new coop::voice::Engine(g_backend.codecs, EngineSettings(g_settings, false)));
    if (g_settings.mode == coop::voice::Mode::off) {
        LOG_INFO(Core, "coop: voice: off (the default: no microphone, nothing heard) until it is turned on "
                       "in the co-op settings");
    } else {
        LOG_INFO(Core, "coop: voice: on ({}, key {:#x}, range {} m, volume {}%)", ModeName(g_settings.mode),
                 g_settings.key, g_settings.range_m, static_cast<int>(g_settings.volume * 100.0f));
    }
}

bool Available() { return g_engine.load() != nullptr; }

Settings Get() {
    std::lock_guard lock(g_mutex);
    ReadSettingsOnce();
    return g_settings;
}

void Set(const Settings& settings) {
    std::lock_guard lock(g_mutex);
    ReadSettingsOnce();
    Settings next = settings;
    next.test = g_settings.test;  // the test switch is the start's, never the menu's
    if (next == g_settings) {
        return;
    }
    const bool mode_changed = next.mode != g_settings.mode;
    g_settings = next;
    if (auto* e = g_engine.load()) {
        e->configure(EngineSettings(g_settings, g_camera_known));
    }
    if (mode_changed) {
        LOG_INFO(Core, "coop: voice: now {}", ModeName(g_settings.mode));
    }
}

bool Enabled() { return Get().mode != coop::voice::Mode::off; }

void SetEnabled(bool on) { Set(WithEnabled(Get(), on)); }

void OnEvent(const coop::net::Event& event) {
    auto* e = g_engine.load();
    if (e == nullptr) {
        return;
    }
    if (event.kind == coop::net::EventKind::message) {
        if (const auto* v = std::get_if<coop::protocol::VoiceFrame>(&event.message)) {
            e->receive(*v, Now());
        }
    } else if (event.kind == coop::net::EventKind::player_left) {
        e->forget(event.player);
    }
}

void Frame(const std::optional<std::array<float, 3>>& our_feet,
           const std::vector<coop::overlay::Shown>& others, const std::vector<std::uint8_t>& hidden,
           coop::host::Session* session) {
    auto* e = g_engine.load();
    if (e == nullptr) {
        return;
    }
    // Who we hear and from where: presence's place of each Link not hidden.
    std::array<bool, kMaxSpeakers + 1> placed{};
    std::array<std::optional<float>, kMaxSpeakers + 1> distance{};  // ours to each, for the test log
    if (our_feet) {
        for (const auto& s : others) {
            if (s.number == 0 || s.number > kMaxSpeakers || s.fade <= 0.0f || !s.scene ||
                std::find(hidden.begin(), hidden.end(), s.number) != hidden.end()) {
                continue;
            }
            const auto mouth = MouthOf(s.position, s.head);
            e->set_speaker(s.number, mouth);
            placed[s.number] = true;
            const auto ears = MouthOf(*our_feet, 0.0f);
            distance[s.number] = std::hypot(mouth[0] - ears[0], mouth[1] - ears[1], mouth[2] - ears[2]);
        }
    }
    for (std::uint8_t p = 1; p <= kMaxSpeakers; ++p) {
        if (!placed[p]) {
            e->set_speaker(p, std::nullopt);
        }
    }
    // The listener: at our Link's head, facing as the camera does.
    const auto camera = CoopCamera::Latest();
    std::optional<coop::voice::Listener> view;
    if (camera && std::chrono::steady_clock::now() - camera->at < std::chrono::milliseconds(500)) {
        view = coop::voice::listener_from_clip(camera->clip_from_world);
    }
    if (our_feet) {
        coop::voice::Listener l = view.value_or(coop::voice::Listener{});
        l.position = MouthOf(*our_feet, 0.0f);
        e->set_listener(l);
    }
    // Our microphone, sent while connected and in the game.
    std::vector<coop::protocol::VoiceFrame> out;
    Settings now_set;
    {
        std::lock_guard lock(g_mutex);
        ReadSettingsOnce();
        now_set = g_settings;
        if (view.has_value() != g_camera_known) {
            g_camera_known = view.has_value();
            e->configure(EngineSettings(g_settings, g_camera_known));
        }
        const bool can_send = session != nullptr && our_feet.has_value() && g_backend.mic_start &&
                              g_backend.mic_read;
        const bool key = g_settings.mode == coop::voice::Mode::push_to_talk && g_backend.key_down &&
                         g_backend.key_down(g_settings.key);
        const bool want = MicWanted(g_settings, can_send, key, e->talking());
        if (want && !g_mic_on) {
            g_mic_on = g_backend.mic_start();
            if (!g_mic_on && g_mic_said.exchange(1) == 0) {
                LOG_WARNING(Core, "coop: voice: the microphone did not start; the others hear nothing");
            } else if (g_mic_on && g_settings.test != Test::tone && g_mic_opened.exchange(true) == false) {
                LOG_INFO(Core, "coop: voice: microphone opened ({}); closed again when not talking",
                         g_settings.mode == coop::voice::Mode::open_mic ? "open mic" : "push to talk held");
            }
        } else if (!want && g_mic_on) {
            // Close the spurt if we were talking, then let the microphone go.
            e->stop_talking(out);
            if (g_backend.mic_stop) {
                g_backend.mic_stop();
            }
            g_mic_on = false;
        }
        if (g_mic_on) {
            std::array<std::int16_t, 4800> buffer{};
            for (int round = 0; round < 4; ++round) {  // at most 400 ms a frame
                const int n = g_backend.mic_read(buffer.data(), static_cast<int>(buffer.size()));
                if (n <= 0) {
                    break;
                }
                e->capture(std::span<const std::int16_t>(buffer.data(), static_cast<std::size_t>(n)), key,
                           out);
                if (n < static_cast<int>(buffer.size())) {
                    break;
                }
            }
        }
    }
    if (!out.empty() && session != nullptr) {
        session->with_client([&](coop::net::Client& c) {
            for (const auto& v : out) {
                c.send(v);
            }
        });
    }
    if (now_set.test != Test::none) {
        LogTest(*e, distance, std::clamp(now_set.range_m, 5.0f, 100.0f), ModeName(now_set.mode));
    }
}

void Render(float* const* out, int channels, int frames) {
    auto* e = g_engine.load();
    if (e == nullptr) {
        return;
    }
    e->render(out, channels, frames, Now());
}

bool Speaking(std::uint8_t player) {
    auto* e = g_engine.load();
    return e != nullptr && e->speaking(player, Now());
}

bool Talking() {
    auto* e = g_engine.load();
    return e != nullptr && e->talking();
}

} // namespace CoopVoice

#endif // LICHII_COOP
