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

std::string UserName(CoopPuppet::Guest& guest, std::uint64_t user) {
    const std::uint64_t resource = user != 0 ? Pointer(guest, user + addr::kUserResource) : 0;
    return resource != 0 ? Text(guest, Pointer(guest, resource + addr::kResourceName)) : std::string();
}

// Looks like a sound user: its event list (Playing) runs from its first link back to its
// head, and it is not an effect user. A field that only holds the resource pointer does not.
bool LooksLikeUser(CoopPuppet::Guest& guest, std::uint64_t user) {
    std::uint32_t flags = 0;
    std::int32_t offset = 0;
    if (!Read(guest, user + addr::kUserFlags, flags) || (flags & addr::kUserFlagEffect) != 0 ||
        !Read(guest, user + addr::kUserLinkOffset, offset) || offset < 0 || offset > 0x400) {
        return false;
    }
    const std::uint64_t head = user + addr::kUserEvents;
    std::uint64_t link = Pointer(guest, user + addr::kUserFirstLink);
    for (int i = 0; i <= kMaxEvents; ++i) {
        if (link == head) {
            return true;
        }
        if (link == 0) {
            return false;
        }
        link = Pointer(guest, link + addr::kLinkNext);
    }
    return false;
}

std::uint64_t FindUser(CoopPuppet::Guest& guest, std::uint64_t from, const std::string& name,
                       std::uint64_t* link) {
    if (from == 0) {
        return 0;
    }
    for (std::uint64_t off = 0; off < kUserScan; off += 8) {
        const std::uint64_t head = from + off;
        std::uint64_t node = Pointer(guest, head);
        for (int i = 0; node != 0 && node != head && i < kListSteps; ++i) {
            if (node > off) {
                const std::uint64_t user = node - off;
                if (user != from && Accessor(guest, user) != 0 && UserName(guest, user) == name &&
                    LooksLikeUser(guest, user)) {
                    if (link != nullptr) {
                        *link = off;
                    }
                    return user;
                }
            }
            node = Pointer(guest, node);
        }
    }
    return 0;
}

std::uint64_t SoundRoot(CoopPuppet::Guest& guest, std::uint64_t main_base) {
    const std::uint64_t got = main_base != 0 ? Pointer(guest, main_base + addr::kSoundRootGot) : 0;
    return got != 0 ? Pointer(guest, got) : 0;
}

std::uint64_t SLinkSystem(CoopPuppet::Guest& guest, std::uint64_t main_base) {
    const std::uint64_t root = SoundRoot(guest, main_base);
    return root != 0 ? Pointer(guest, root + addr::kRootSLinkSystem) : 0;
}

std::uint64_t FindUserInModules(CoopPuppet::Guest& guest, std::uint64_t root,
                                const std::string& name, std::string* how) {
    if (root == 0) {
        return 0;
    }
    for (std::uint64_t off = 0; off + 16 <= kSystemScan; off += 8) {
        std::int32_t count = 0;
        std::int32_t capacity = 0;
        if (!Read(guest, root + off, count) || !Read(guest, root + off + 4, capacity) ||
            count <= 0 || count > capacity || capacity > 8192) {
            continue;
        }
        const std::uint64_t data = Pointer(guest, root + off + 8);
        for (std::int32_t i = 0; data != 0 && i < count; ++i) {
            const std::uint64_t module = Pointer(guest, data + 8 * static_cast<std::uint64_t>(i));
            for (std::uint64_t field = 0; module != 0 && field < kModuleScan; field += 8) {
                const std::uint64_t user = Pointer(guest, module + field);
                if (user != 0 && Accessor(guest, user) != 0 && UserName(guest, user) == name &&
                    LooksLikeUser(guest, user)) {
                    if (how != nullptr) {
                        *how = "module " + std::to_string(i) + " of the array at root +" + Hex(off) +
                               ", field +" + Hex(field);
                    }
                    return user;
                }
            }
        }
    }
    return 0;
}

std::uint64_t FindOcarinaModule(CoopPuppet::Guest& guest, std::uint64_t main_base,
                                std::uint64_t root, std::string* how) {
    if (main_base == 0 || root == 0) {
        return 0;
    }
    const std::uint64_t got = Pointer(guest, main_base + addr::kOcarinaModuleVtableGot);
    if (got == 0) {
        return 0;
    }
    const std::uint64_t vtable = got + 0x10;
    const auto is = [&](std::uint64_t p) { return p != 0 && Pointer(guest, p) == vtable; };
    // In `object`'s first kRootScan bytes: a field pointing at it, or an array holding it.
    const auto look = [&](std::uint64_t object, std::string& where) -> std::uint64_t {
        for (std::uint64_t off = 0; off < kRootScan; off += 8) {
            const std::uint64_t p = Pointer(guest, object + off);
            if (is(p)) {
                where = "+" + Hex(off);
                return p;
            }
            std::int32_t count = 0;
            std::int32_t capacity = 0;
            if (!Read(guest, object + off, count) || !Read(guest, object + off + 4, capacity) ||
                count <= 0 || count > capacity || capacity > 1024) {
                continue;
            }
            const std::uint64_t data = Pointer(guest, object + off + 8);
            for (std::int32_t i = 0; data != 0 && i < count; ++i) {
                const std::uint64_t m = Pointer(guest, data + 8 * static_cast<std::uint64_t>(i));
                if (is(m)) {
                    where = "array +" + Hex(off) + " [" + std::to_string(i) + "]";
                    return m;
                }
            }
        }
        return 0;
    };
    std::string where;
    if (const std::uint64_t m = look(root, where); m != 0) {
        if (how != nullptr) {
            *how = "root " + where;
        }
        return m;
    }
    for (std::uint64_t off = 0; off < kRootScan; off += 8) {
        const std::uint64_t child = Pointer(guest, root + off);
        if (child == 0 || child == root || (child & 7) != 0) {
            continue;
        }
        if (const std::uint64_t m = look(child, where); m != 0) {
            if (how != nullptr) {
                *how = "root +" + Hex(off) + " -> " + where;
            }
            return m;
        }
    }
    return 0;
}

static std::string Words(CoopPuppet::Guest& guest, std::uint64_t at, std::uint64_t size) {
    std::string out;
    for (std::uint64_t off = 0; off < size; off += 8) {
        out += (off == 0 ? "" : " ") + Hex(Pointer(guest, at + off));
    }
    return out;
}

// For each pointer field in `at`'s first 0x80 bytes: the text at [p + 0x10] or at p (a
// resource's or a string's name), as " +off 'text'".
static std::string Names(CoopPuppet::Guest& guest, std::uint64_t at) {
    std::string out;
    for (std::uint64_t off = 0; at != 0 && off < 0x80; off += 8) {
        const std::uint64_t p = Pointer(guest, at + off);
        if (p == 0 || (p & 7) != 0) {
            continue;
        }
        std::string t = Text(guest, Pointer(guest, p + 0x10));
        if (t.size() < 3) {
            t = Text(guest, p);
        }
        if (t.size() >= 3) {
            out += " +" + Hex(off) + " '" + t + "'";
        }
    }
    return out;
}

std::string DescribeModule(CoopPuppet::Guest& guest, std::uint64_t module) {
    const std::uint64_t user = Pointer(guest, module + addr::kModuleUser);
    std::uint32_t flags = 0;
    Read(guest, user + addr::kUserFlags, flags);
    const std::uint64_t back = Pointer(guest, user + addr::kUserModuleBack);
    return "module " + Hex(module) + ", user +0x40 " + Hex(user) + " '" + UserName(guest, user) +
           "' accessor " + Hex(user != 0 ? Accessor(guest, user) : 0) + " flags " + Hex(flags) +
           (user != 0 && LooksLikeUser(guest, user) ? " passes" : " fails") +
           " the user check, back link " + (back == module + 0x38 ? "ok" : Hex(back)) +
           (user != 0 ? ", user words " + Words(guest, user, 0x80) + Names(guest, user) +
                            ", +0x20 words " + Words(guest, Pointer(guest, user + 0x20), 0x80) +
                            Names(guest, Pointer(guest, user + 0x20))
                      : std::string());
}

std::uint64_t FindUserInSystem(CoopPuppet::Guest& guest, std::uint64_t system,
                               const std::string& name, std::string* how) {
    if (system == 0) {
        return 0;
    }
    const auto is = [&](std::uint64_t user) {
        return user != 0 && Accessor(guest, user) != 0 && UserName(guest, user) == name &&
               LooksLikeUser(guest, user);
    };
    // Pointer arrays.
    for (std::uint64_t off = 0; off + 16 <= kSystemScan; off += 8) {
        std::int32_t count = 0;
        std::int32_t capacity = 0;
        if (!Read(guest, system + off, count) || !Read(guest, system + off + 4, capacity) ||
            count <= 0 || count > capacity || capacity > 8192) {
            continue;
        }
        const std::uint64_t data = Pointer(guest, system + off + 8);
        for (std::int32_t i = 0; data != 0 && i < count; ++i) {
            const std::uint64_t user = Pointer(guest, data + 8 * static_cast<std::uint64_t>(i));
            if (is(user)) {
                if (how != nullptr) {
                    *how = "array at system +" + Hex(off) + " [" + std::to_string(i) + "]";
                }
                return user;
            }
        }
    }
    // Circular lists, the link anywhere in the user's first kUserScan bytes.
    for (std::uint64_t off = 0; off < kSystemScan; off += 8) {
        const std::uint64_t head = system + off;
        std::uint64_t node = Pointer(guest, head);
        for (int i = 0; node != 0 && node != head && i < kSystemListSteps; ++i) {
            for (std::uint64_t link = 0; link < kUserScan && link <= node; link += 8) {
                if (is(node - link)) {
                    if (how != nullptr) {
                        *how = "list at system +" + Hex(off) + ", link +" + Hex(link);
                    }
                    return node - link;
                }
            }
            node = Pointer(guest, node);
        }
    }
    return 0;
}

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
    state.logged_user = 0;
    state.user_said = false;
    state.missing_said = false;
    state.ocarina_user = 0;
    state.ocarina_looked = false;
    state.module_said.clear();
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
        if (user != state.logged_user || !state.user_said) {
            state.logged_user = user;
            state.user_said = true;
            const std::uint64_t resource = Pointer(guest, user + addr::kUserResource);
            std::uint32_t flags = 0;
            Read(guest, user + addr::kUserFlags, flags);
            say(who + "'s sound user " + Hex(user) + " is '" +
                Text(guest, Pointer(guest, resource + addr::kResourceName)) + "' (" +
                ((flags & addr::kUserFlagEffect) != 0 ? "ELink" : "SLink") + " flags " +
                Hex(flags) + ")");
        }
        // The notes are the game's "Ocarina" user's assets (issue 152): played on it
        // (not placed at the puppet: the game's own notes are not placed either).
        if (state.ocarina_looked && state.ocarina_user == 0) {
            // The module's user may get its resource later (first use on this game?): look
            // again on each note, saying the module line only when it changes.
            std::string where;
            const std::uint64_t module =
                FindOcarinaModule(guest, main_base, SoundRoot(guest, main_base), &where);
            const std::uint64_t mu = module != 0 ? Pointer(guest, module + addr::kModuleUser) : 0;
            if (module != 0) {
                const std::string line = who + ": Ocarina " + DescribeModule(guest, module) + " (" + where + ")";
                if (line != state.module_said) {
                    state.module_said = line;
                    say(line);
                }
            }
            if (mu != 0 && Accessor(guest, mu) != 0) {
                state.ocarina_user = mu;
                say(who + ": Ocarina notes go to sound user " + Hex(mu) + " '" + kOcarinaUser +
                    "' (Ocarina module at " + where + ", field +0x40, on a later note)");
            }
        }
        if (!state.ocarina_looked) {
            state.ocarina_looked = true;
            std::uint64_t off = 0;
            std::string how;
            const std::uint64_t system = SLinkSystem(guest, main_base);
            state.ocarina_user = FindUserInModules(guest, SoundRoot(guest, main_base), kOcarinaUser, &how);
            std::string no_module;
            if (state.ocarina_user == 0) {
                // The module by its vtable: its user even when the name checks fail (logged).
                std::string where;
                const std::uint64_t module =
                    FindOcarinaModule(guest, main_base, SoundRoot(guest, main_base), &where);
                if (module != 0) {
                    state.module_said = who + ": Ocarina " + DescribeModule(guest, module) + " (" + where + ")";
                    say(state.module_said);
                } else {
                    no_module = "; no Ocarina module (vtable [main+" +
                                Hex(addr::kOcarinaModuleVtableGot) + "]+0x10) near the sound root " +
                                Hex(SoundRoot(guest, main_base));
                }
                const std::uint64_t mu = module != 0 ? Pointer(guest, module + addr::kModuleUser) : 0;
                if (mu != 0 && Accessor(guest, mu) != 0) {
                    state.ocarina_user = mu;
                    how = "Ocarina module at " + where + ", field +0x40";
                }
            }
            if (state.ocarina_user == 0) {
                state.ocarina_user = FindUserInSystem(guest, system, kOcarinaUser, &how);
            }
            if (state.ocarina_user == 0) {
                state.ocarina_user = FindUser(guest, user, kOcarinaUser, &off);
                how = "list at +" + Hex(off) + " of its own user";
            }
            say(state.ocarina_user != 0
                    ? who + ": Ocarina notes go to sound user " + Hex(state.ocarina_user) + " '" +
                          kOcarinaUser + "' (" + how + ")"
                    : who + ": no '" + kOcarinaUser + "' sound user found (SLink system " +
                          Hex(system) + " vtable main+" +
                          Hex(Pointer(guest, system) - main_base) + ", own user " + Hex(user) + (LooksLikeUser(guest, user) ? " passes" : " fails") +
                          " the user check" + no_module + "): notes searched in its own user");
        }
        const std::uint64_t notes_user = state.ocarina_user != 0 ? state.ocarina_user : user;
        const std::uint64_t notes_accessor =
            state.ocarina_user != 0 ? Accessor(guest, state.ocarina_user) : accessor;
        if (notes_accessor == 0) {
            state.step = State::Step::none;
            return std::nullopt;
        }
        state.user = notes_user;
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
        c.x[0] = notes_accessor;
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
    std::string now = Playing(guest, SoundUser(guest, actor));
    if (state.ocarina_user != 0) {
        // The game's Ocarina user: the notes played there (issue 152).
        const std::string notes = Playing(guest, state.ocarina_user);
        if (!notes.empty()) {
            now += (now.empty() ? "" : ", ") + std::string("Ocarina user: ") + notes;
        }
    }
    if (now != state.playing) {
        state.playing = now;
        say(who + " sounds: " + (now.empty() ? std::string("none") : now));
    }
}

} // namespace CoopPuppetSound
