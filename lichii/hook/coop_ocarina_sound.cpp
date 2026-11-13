// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_ocarina_sound.h"

#include <array>

namespace CoopOcarinaSound {

namespace {

// OcarinaMgr's table, read from its constructor (main + 0xe7c5d0 on).
constexpr std::array<const char*, kNoteCount> kNotes{
    "LowB", "LowC", "LowCsharp", "D", "Dsharp", "E",  "F",     "Fsharp",     "G",     "Gsharp",
    "A",    "Asharp", "B",       "C", "Csharp", "HighD", "HighDsharp", "HighE", "HighF"};

} // namespace

const char* NoteName(std::uint8_t note) {
    return note >= 1 && note <= kNoteCount ? kNotes[note - 1] : "";
}

std::string AssetName(std::uint8_t note) {
    const char* name = NoteName(note);
    return *name != '\0' ? std::string("Ocarina_") + name : std::string();
}

std::uint8_t NoteOf(std::uint64_t button) {
    switch (button) {
    case kButtonL: return 4;   // D
    case kButtonR: return 7;   // F
    case kButtonY: return 11;  // A
    case kButtonX: return 13;  // B
    case kButtonA: return 16;  // HighD
    default: return 0;
    }
}

bool Playing(std::string_view command) {
    return command.starts_with("DemoOcarina") && command.find("MelodyGet") == std::string_view::npos;
}

std::optional<std::uint8_t> NoteSender::Update(bool playing, std::uint64_t buttons) {
    const std::uint64_t held = playing ? (buttons & kNoteButtons) : 0;
    const std::uint64_t pressed = held & ~held_;
    held_ = held;
    if (pressed != 0) {
        // The newest press wins; of two in the same frame, the higher note.
        std::uint64_t pick = 0;
        std::uint8_t note = 0;
        for (std::uint64_t bit = 1; bit != 0 && bit <= kNoteButtons; bit <<= 1) {
            if ((pressed & bit) != 0 && NoteOf(bit) > note) {
                note = NoteOf(bit);
                pick = bit;
            }
        }
        button_ = pick;
        sounding_ = note;
        return note;
    }
    if (sounding_ != 0 && (held & button_) == 0) {
        // Let go. Another note button still held sounds again (as the game
        // goes back to it; the highest if several); none: silence.
        button_ = 0;
        sounding_ = 0;
        for (std::uint64_t bit = 1; bit <= kNoteButtons; bit <<= 1) {
            if ((held & bit) != 0 && NoteOf(bit) > sounding_) {
                button_ = bit;
                sounding_ = NoteOf(bit);
            }
        }
        return sounding_;
    }
    return std::nullopt;
}

} // namespace CoopOcarinaSound

#ifdef LICHII_COOP

#include "common/logging.h"
#include "coop/host/session.hpp"
#include "coop/net/client.hpp"
#include "coop/protocol/messages.hpp"
#include "core/core.h"
#ifdef COOP_HOOKSHOT
#include "coop_runtime.h"
#else
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "hid_core/hid_types.h"
#endif

namespace CoopOcarinaSound {

namespace {

NoteSender g_sender;  // the present thread only

std::uint64_t Buttons(Core::System& system) {
#ifdef COOP_HOOKSHOT
    // Hookshot: the word its nn::hid gave the game (our patch's
    // HosCoopApi::pad_buttons), the same bits as the emulated pad's.
    (void)system;
    std::uint64_t buttons = 0;
    return CoopRuntime::PadButtons(buttons) ? buttons : 0;
#else
    auto& hid = system.HIDCore();
    for (const auto id : {Core::HID::NpadIdType::Player1, Core::HID::NpadIdType::Handheld}) {
        auto* c = hid.GetEmulatedController(id);
        if (c != nullptr && c->IsConnected()) {
            return static_cast<std::uint64_t>(c->GetNpadButtons().raw);
        }
    }
    return 0;
#endif
}

} // namespace

void Frame(Core::System& system, const std::optional<std::string>& command,
           coop::host::Session* session) {
    const bool playing = command && Playing(*command);
    const auto note = g_sender.Update(playing, playing ? Buttons(system) : 0);
    if (!note || session == nullptr) {
        return;
    }
    session->with_client([&](coop::net::Client& c) {
        c.send(coop::protocol::OcarinaNote{coop::protocol::kPlayerSelf, *note});
    });
    LOG_INFO(Core, "coop: ocarina note {} ({}) sent", *note,
             *note != 0 ? AssetName(*note) : std::string("let go"));
}

} // namespace CoopOcarinaSound

#endif // LICHII_COOP
