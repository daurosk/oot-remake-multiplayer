// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_voice.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

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
        if (mode == "off" || mode == "0" || mode == "no") {
            s.mode = coop::voice::Mode::off;
        } else if (mode == "ptt" || mode == "push" || mode == "push-to-talk" || mode == "1") {
            s.mode = coop::voice::Mode::push_to_talk;
        } else if (mode == "open" || mode == "open-mic" || mode == "on") {
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
    return s;
}

coop::voice::Settings EngineSettings(const Settings& s, bool camera_known) {
    coop::voice::Settings e;
    e.gate.mode = s.mode;
    e.muted = s.muted;
    e.deafened = s.deafened;
    e.volume = std::clamp(s.volume, 0.0f, 2.0f);
    e.spatial.range_m = std::clamp(s.range_m, 5.0f, 100.0f);
    e.spatial.fade_m = std::min(6.0f, e.spatial.range_m / 3.0f);
    e.spatial.pan = camera_known;
    return e;
}

coop::voice::Vec3 MouthOf(const std::array<float, 3>& feet, float head) {
    const float up = head > 0.3f ? head * 0.9f : 1.2f;
    return {feet[0], feet[1] + up, feet[2]};
}

} // namespace CoopVoice

#ifdef LICHII_COOP

#include <atomic>
#include <memory>
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

} // namespace

void SetBackend(Backend backend) {
    std::lock_guard lock(g_mutex);
    ReadSettingsOnce();
    if (g_engine.load() != nullptr) {
        LOG_WARNING(Core, "coop: voice: a second backend ignored");
        return;
    }
    g_backend = std::move(backend);
    g_engine.store(new coop::voice::Engine(g_backend.codecs, EngineSettings(g_settings, false)));
    static const char* const kModes[] = {"off", "push to talk", "open mic"};
    LOG_INFO(Core, "coop: voice: on ({}, key {:#x}, range {} m, volume {}%)",
             kModes[static_cast<int>(g_settings.mode) % 3], g_settings.key, g_settings.range_m,
             static_cast<int>(g_settings.volume * 100.0f));
}

bool Available() { return g_engine.load() != nullptr; }

Settings Get() {
    std::lock_guard lock(g_mutex);
    ReadSettingsOnce();
    return g_settings;
}

void Set(const Settings& settings) {
    std::lock_guard lock(g_mutex);
    g_settings_read = true;
    g_settings = settings;
    if (auto* e = g_engine.load()) {
        e->configure(EngineSettings(g_settings, g_camera_known));
    }
}

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
    if (our_feet) {
        for (const auto& s : others) {
            if (s.number == 0 || s.number > kMaxSpeakers || s.fade <= 0.0f || !s.scene ||
                std::find(hidden.begin(), hidden.end(), s.number) != hidden.end()) {
                continue;
            }
            e->set_speaker(s.number, MouthOf(s.position, s.head));
            placed[s.number] = true;
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
    {
        std::lock_guard lock(g_mutex);
        ReadSettingsOnce();
        if (view.has_value() != g_camera_known) {
            g_camera_known = view.has_value();
            e->configure(EngineSettings(g_settings, g_camera_known));
        }
        const bool want = session != nullptr && our_feet && g_settings.mode != coop::voice::Mode::off &&
                          g_backend.mic_start && g_backend.mic_read;
        if (want && !g_mic_on) {
            g_mic_on = g_backend.mic_start();
            if (!g_mic_on && g_mic_said.exchange(1) == 0) {
                LOG_WARNING(Core, "coop: voice: the microphone did not start; the others hear nothing");
            }
        } else if (!want && g_mic_on) {
            // Close the spurt if we were talking, then let the microphone go.
            std::vector<std::int16_t> silence(static_cast<std::size_t>(coop::voice::kSamplesPerFrame), 0);
            e->capture(silence, false, out);
            if (g_backend.mic_stop) {
                g_backend.mic_stop();
            }
            g_mic_on = false;
        }
        if (g_mic_on) {
            const bool key = g_backend.key_down && g_backend.key_down(g_settings.key);
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
