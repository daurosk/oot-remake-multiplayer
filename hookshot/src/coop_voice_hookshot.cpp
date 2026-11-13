// SPDX-License-Identifier: GPL-3.0-or-later
//
// Proximity voice on Hookshot (docs/voice.md): the two optional exports of
// hos_coop_voice.h. Hookshot's mixer finds them once the module is loaded
// (our patch 0009, host/coop_voice.c), hands over the microphone and libopus
// through attach, and calls render every 5 ms with the game's final mix.
// This file turns that table into CoopVoice's Backend.
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>

#include "coop_hookshot.h"
#include "coop_voice.h"
#include "hos_coop_voice.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

const HosCoopVoiceApi* g_voice = nullptr;

constexpr int kBitrate = 24000;   // speech, wideband: clear at 3 KB a second
constexpr int kLossPercent = 10;  // in-band FEC for about this much loss

class VoiceEncoder final : public coop::voice::Encoder {
public:
    VoiceEncoder() : enc_(g_voice->opus_encoder_new(kBitrate, kLossPercent)) {}
    ~VoiceEncoder() override {
        if (enc_ != nullptr) {
            g_voice->opus_encoder_free(enc_);
        }
    }
    VoiceEncoder(const VoiceEncoder&) = delete;
    VoiceEncoder& operator=(const VoiceEncoder&) = delete;
    std::size_t encode(std::span<const std::int16_t> pcm, std::span<std::uint8_t> out) override {
        if (enc_ == nullptr || pcm.size() != static_cast<std::size_t>(HOS_COOP_VOICE_FRAME)) {
            return 0;
        }
        const int n = g_voice->opus_encode(enc_, pcm.data(), out.data(), static_cast<int>(out.size()));
        return n > 0 ? static_cast<std::size_t>(n) : 0;
    }

private:
    void* enc_;
};

class VoiceDecoder final : public coop::voice::Decoder {
public:
    VoiceDecoder() : dec_(g_voice->opus_decoder_new()) {}
    ~VoiceDecoder() override {
        if (dec_ != nullptr) {
            g_voice->opus_decoder_free(dec_);
        }
    }
    VoiceDecoder(const VoiceDecoder&) = delete;
    VoiceDecoder& operator=(const VoiceDecoder&) = delete;
    bool decode(std::span<const std::uint8_t> packet, bool fec, std::span<std::int16_t> pcm) override {
        if (dec_ == nullptr || pcm.size() != static_cast<std::size_t>(HOS_COOP_VOICE_FRAME)) {
            return false;
        }
        return g_voice->opus_decode(dec_, packet.empty() ? nullptr : packet.data(), static_cast<int>(packet.size()),
                                    fec ? 1 : 0, pcm.data()) > 0;
    }

private:
    void* dec_;
};

bool KeyDown(std::uint32_t vk) {
    if (vk == 0 || vk > 255 || CoopHookshot::KeysTaken()) {  // typing in the chat box is not talking
        return false;
    }
#ifdef _WIN32
    // Only while the game's window is in front: the key held in another
    // program is not push to talk.
    const HWND front = GetForegroundWindow();
    DWORD pid = 0;
    if (front == nullptr || (GetWindowThreadProcessId(front, &pid), pid) != GetCurrentProcessId()) {
        return false;
    }
    return (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
#else
    return false;
#endif
}

} // namespace

extern "C" {
// hos_coop_voice.h: once, from Hookshot's mixer thread, maybe before
// hos_coop_start has run (the mixer finds the module as soon as it is
// loaded): nothing here needs the game, and a 0 would be for good.
COOP_HOOKSHOT_EXPORT int hos_coop_voice_attach(const HosCoopVoiceApi* api) {
    if (api == nullptr || api->version != HOS_COOP_VOICE_VERSION || api->size < sizeof(HosCoopVoiceApi) ||
        g_voice != nullptr) {
        return 0;
    }
    g_voice = api;
    CoopVoice::Backend b;
    b.mic_start = [] { return g_voice->mic_start() != 0; };
    b.mic_read = [](std::int16_t* out, int max) { return g_voice->mic_read(out, max); };
    b.mic_stop = [] { g_voice->mic_stop(); };
    b.codecs.encoder = [] { return std::make_unique<VoiceEncoder>(); };
    b.codecs.decoder = [] { return std::make_unique<VoiceDecoder>(); };
    b.now_us = [] { return g_voice->now_us(); };
    b.key_down = KeyDown;
    CoopVoice::SetBackend(std::move(b));
    return 1;
}
COOP_HOOKSHOT_EXPORT void hos_coop_voice_render(float* const* out, int channels, int frames) {
    CoopVoice::Render(out, channels, frames);
}
}
