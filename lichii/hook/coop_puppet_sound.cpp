// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_puppet_sound.h"

#include <array>
#include <cstdio>

#include "coop_ocarina_sound.h"
#include "coop_puppet.h"

namespace CoopPuppetSound {

namespace {

template <typename T>
bool Read(CoopPuppet::Guest& guest, std::uint64_t address, T& out) {
    return address != 0 && guest.Read(address, &out, sizeof(out));
}

std::uint64_t Pointer(CoopPuppet::Guest& guest, std::uint64_t address) {
    std::uint64_t value = 0;
    return Read(guest, address, value) ? value : 0;
}

std::string Text(CoopPuppet::Guest& guest, std::uint64_t address) {
    if (address == 0) {
        return {};
    }
    std::array<char, 65> buf{};
    if (!guest.Read(address, buf.data(), buf.size() - 1)) {
        return {};
    }
    std::string out;
    for (const char c : buf) {
        if (c == '\0') {
            return out;
        }
        if (c < 0x20 || c > 0x7e) {
            return {};
        }
        out += c;
    }
    return {};  // not ended within 64
}

std::string Hex(std::uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(v));
    return buf;
}

// The resource's asset accessor of a user; 0 when it has none.
std::uint64_t Accessor(CoopPuppet::Guest& guest, std::uint64_t user) {
    const std::uint64_t resource = Pointer(guest, user + addr::kUserResource);
    return resource != 0 ? Pointer(guest, resource + addr::kResourceAccessor) : 0;
}

} // namespace

std::uint64_t SoundUser(CoopPuppet::Guest& guest, std::uint64_t actor) {
    const std::uint64_t parts = Pointer(guest, actor + addr::kActorParts);
    const std::uint64_t xlink = parts != 0 ? Pointer(guest, parts + addr::kPartsXLink) : 0;
    return xlink != 0 ? Pointer(guest, xlink + addr::kXLinkSoundUser) : 0;
}

void Reset(State& state) {
    state.sounding = 0;
    state.step = State::Step::none;
    state.pending = State::Step::none;
    state.user = 0;
    state.user_said = false;
    state.missing_said = false;
    state.playing.clear();
}

std::optional<CoopPuppet::Call> Next(State& state, const Wanted& wanted, CoopPuppet::Guest& guest,
                                     std::uint64_t main_base, std::uint64_t actor,
                                     std::uint64_t block, std::uint64_t now_ms,
                                     const std::string& who, const Say& say) {
    if (main_base == 0 || actor == 0 || block == 0) {
        return std::nullopt;
    }
    if (actor != state.actor && state.pending == State::Step::none) {
        Reset(state);  // made again: nothing sounds on it, and its handle is old
        state.actor = actor;
    }
    if (state.step == State::Step::none && wanted.serial != 0 && wanted.serial != state.serial_done) {
        state.serial_done = wanted.serial;
        const bool stale = now_ms > wanted.at_ms && now_ms - wanted.at_ms > kStaleMs;
        state.want = stale ? 0 : wanted.note;
        if (state.sounding != 0) {
            state.step = State::Step::kill;  // the note before stops first
        } else if (state.want != 0) {
            state.step = State::Step::search;
        }
    }
    CoopPuppet::Call c;
    switch (state.step) {
    case State::Step::none:
    case State::Step::emit:
        if (state.step == State::Step::none) {
            return std::nullopt;
        }
        c.function = main_base + addr::kEmit;
        c.x[0] = state.user;
        c.x[1] = block + block::kAsset;
        c.x[2] = block + block::kHandle;
        c.x[3] = 0;
        break;
    case State::Step::kill:
        c.function = main_base + addr::kKill;
        c.x[0] = block + block::kHandle;
        break;
    case State::Step::search: {
        const std::uint64_t user = SoundUser(guest, actor);
        const std::uint64_t accessor = user != 0 ? Accessor(guest, user) : 0;
        if (accessor == 0) {
            if (!state.missing_said) {
                state.missing_said = true;
                say(who + " has no sound user (XLink component +0x78 " + Hex(user) +
                    "): its Ocarina is not played");
            }
            state.step = State::Step::none;
            return std::nullopt;
        }
        if (user != state.user || !state.user_said) {
            state.user = user;
            state.user_said = true;
            const std::uint64_t resource = Pointer(guest, user + addr::kUserResource);
            std::uint32_t flags = 0;
            Read(guest, user + addr::kUserFlags, flags);
            say(who + "'s sound user " + Hex(user) + " is '" +
                Text(guest, Pointer(guest, resource + addr::kResourceName)) + "' (" +
                ((flags & addr::kUserFlagEffect) != 0 ? "ELink" : "SLink") + " flags " +
                Hex(flags) + ")");
        }
        const std::string name = CoopOcarinaSound::AssetName(state.want);
        std::array<char, block::kTextCapacity> text{};
        for (std::size_t i = 0; i < name.size() && i + 1 < text.size(); ++i) {
            text[i] = name[i];
        }
        const std::uint64_t zero = 0;
        // The handle as the game's node starts one: u8 0xff, u16 0xffff, u32 0.
        const std::array<std::uint8_t, 8> handle{0xff, 0, 0xff, 0xff, 0, 0, 0, 0};
        if (!guest.Write(block + block::kText, text.data(), text.size()) ||
            !guest.Write(block + block::kAsset, &zero, sizeof(zero)) ||
            !guest.Write(block + block::kHandle, handle.data(), handle.size())) {
            state.step = State::Step::none;
            return std::nullopt;
        }
        c.function = main_base + addr::kSearchAsset;
        c.x[0] = accessor;
        c.x[1] = block + block::kAsset;
        c.x[2] = block + block::kText;
        break;
    }
    }
    state.pending = state.step;
    return c;
}

void Finish(State& state, std::uint64_t result, const std::string& who, const Say& say) {
    const auto done = state.pending;
    state.pending = State::Step::none;
    switch (done) {
    case State::Step::kill:
        state.sounding = 0;
        state.step = state.want != 0 ? State::Step::search : State::Step::none;
        break;
    case State::Step::search:
        if ((result & 1) != 0) {
            state.step = State::Step::emit;
        } else {
            say(who + ": the sound '" + CoopOcarinaSound::AssetName(state.want) +
                "' is not in its sound user");
            state.step = State::Step::none;
        }
        break;
    case State::Step::emit:
        state.sounding = state.want;
        state.step = State::Step::none;
        say(who + " plays sound '" + CoopOcarinaSound::AssetName(state.want) + "' (returned " +
            Hex(result & 0xff) + ")");
        break;
    case State::Step::none:
        break;
    }
}

std::string Playing(CoopPuppet::Guest& guest, std::uint64_t user) {
    if (user == 0) {
        return {};
    }
    std::int32_t offset = 0;
    if (!Read(guest, user + addr::kUserLinkOffset, offset)) {
        return {};
    }
    const std::uint64_t head = user + addr::kUserEvents;
    std::uint64_t link = Pointer(guest, user + addr::kUserFirstLink);
    std::string out;
    for (int i = 0; link != 0 && link != head && i < kMaxEvents; ++i) {
        const std::uint64_t event = link - static_cast<std::uint64_t>(static_cast<std::int64_t>(offset));
        const std::uint64_t asset = Pointer(guest, event + addr::kEventAsset);
        const std::string name = asset != 0 ? Text(guest, Pointer(guest, asset)) : std::string();
        if (!out.empty()) {
            out += ", ";
        }
        out += name.empty() ? "?" : name;
        link = Pointer(guest, link + addr::kLinkNext);
    }
    return out;
}

void LogPlaying(State& state, CoopPuppet::Guest& guest, std::uint64_t actor,
                std::uint64_t now_ms, const std::string& who, const Say& say) {
    if (now_ms < state.playing_ms + kPlayingEveryMs) {
        return;
    }
    state.playing_ms = now_ms;
    const std::string now = Playing(guest, SoundUser(guest, actor));
    if (now != state.playing) {
        state.playing = now;
        say(who + " sounds: " + (now.empty() ? std::string("none") : now));
    }
}

} // namespace CoopPuppetSound
