// A Character Select pick per age (player request, 2026-11-22): one character for child Link, another for adult Link,
// switched by itself when Link changes age (the Master Sword, the Door of Time, a save loaded at the other age).
//
// Snail's mod keeps one pick for both ages (CharacterMenu_SelectedCharacter, an Int in the save) and builds the player
// model's path from it when the game makes the player. The player is a resident actor: the game makes it at a save
// load, at an age change (the area load after Sequence_IsChildhood flips) and in his page's swap as his page closes,
// NOT at a plain scene or area load of the same age (lane run 2026-11-23: the pick was 2 through two same-age loads
// after a title load and Link stayed Link until the first age change). So this keeps two picks of its own and:
// - at a save load (patch 0024's wrap 6, AfterLoadSaveData, on the game's thread, before the area load makes the
//   player) writes the pick for the save's age straight into the Int's live value (GameFlags::set_int_now), then holds
//   it through the load: after each pass of the game's flag applier (wrap 1, phase 1) a value the game put back is
//   written again (LoadHold). The queued path alone came too late: the save's own pick was in when the player was made.
//   The hold ends as soon as the pick wanted for Link's age now is another (an age change soon after the load);
// - each frame (Picks::Frame) writes the pick for the age Link is (Sequence_IsChildhood) through the queued setter, so
//   an age change and anything else that changes the value is put right before the next time the player is made.
// The page stays the one picker: what the player picks there becomes the pick for the age they are now.
//
// Rules, each frame with the values read (Picks::Frame):
// - Off, no mod, the age unknown, or the title: nothing.
// - Snail's page open or its swap in flight: a changed pick is the player's choice for the current age (stored, not
//   written: his page writes it).
// - This age has no pick yet: the current pick becomes it (turning the feature on changes nothing on screen).
// - Else the stored pick for this age is the one wanted (Link when its model is not installed; the stored pick is
//   kept for when it is); when the mod's value differs, it is written, again at most every kRewriteFrames. The log
//   line is said for the first write of a run that the game took (Wrote), not for one that failed.
// Any other change of the mod's value (an age change, a save load, a host's save) is written over, never stored.
// Header only, standard C++ only; tests/lichii/char_ages_test.cpp.
#pragma once

#include <charconv>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "coop_char_slots.h"

namespace CoopCharAges {

struct Stored {
    bool on = false;
    std::optional<std::int32_t> child, adult;
    bool operator==(const Stored&) const = default;
};

// The settings file's one line: "on child adult" (1 or 0, then each pick or -1 for none).
inline std::string Format(const Stored& s) {
    return std::to_string(s.on ? 1 : 0) + " " + std::to_string(s.child.value_or(-1)) + " " +
           std::to_string(s.adult.value_or(-1)) + "\n";
}

inline std::optional<Stored> Parse(std::string_view text) {
    std::int32_t v[3] = {0, -1, -1};
    std::size_t at = 0;
    for (int i = 0; i < 3; ++i) {
        while (at < text.size() && (text[at] == ' ' || text[at] == '\t')) ++at;
        const char* first = text.data() + at;
        const char* last = text.data() + text.size();
        const auto r = std::from_chars(first, last, v[i]);
        if (r.ec != std::errc{}) return std::nullopt;
        at = static_cast<std::size_t>(r.ptr - text.data());
    }
    if ((v[0] != 0 && v[0] != 1) || v[1] < -1 || v[2] < -1) return std::nullopt;
    Stored s;
    s.on = v[0] == 1;
    if (v[1] >= 0) s.child = v[1];
    if (v[2] >= 0) s.adult = v[2];
    return s;
}

struct Input {
    std::optional<bool> childhood;     // Sequence_IsChildhood (nothing: not read)
    std::optional<std::int32_t> pick;  // CharacterMenu_SelectedCharacter (nothing: no mod)
    bool page_open = false;            // his page is up (CharacterMenu_IsPageOpen)
    bool swap_busy = false;            // his page's camera, slide or field swap still running
    bool title = false;                // the title screen (the age read there is a stale save's)
};

struct Step {
    std::optional<std::int32_t> write;  // store this in the mod's value now
    bool stored_changed = false;        // the picks changed: save them
    bool say = false;                   // log this write (the first of a run: a fight over the value is said once)
};

// A model is here (`files` nothing: unknown, so every pick counts as installed, as Pickable does).
inline bool IsInstalled(std::int32_t pick, const std::optional<std::map<std::uint16_t, std::uint16_t>>& files) {
    return !files || CoopCharSlots::Installed(pick, *files);
}

// The value the mod's Int should hold for Link at this age: nothing when off, the age unknown, or no pick for it yet.
inline std::optional<std::int32_t> Wanted(const Stored& s, std::optional<bool> childhood,
                                          const std::optional<std::map<std::uint16_t, std::uint16_t>>& files) {
    if (!s.on || !childhood) return std::nullopt;
    const std::optional<std::int32_t>& mine = *childhood ? s.child : s.adult;
    if (!mine) return std::nullopt;
    return IsInstalled(*mine, files) ? *mine : 0;
}

class Picks {
public:
    static constexpr int kRewriteFrames = 30;

    explicit Picks(Stored s = {}) : s_(s) {}

    const Stored& stored() const { return s_; }
    void SetOn(bool on) {
        s_.on = on;
        wait_ = 0;
    }
    // A pick chosen outside his page (the settings rows): 0 is Link.
    void SetPick(bool child, std::int32_t n) {
        (child ? s_.child : s_.adult) = n < 0 ? 0 : n;
        wait_ = 0;
    }

    // `files` nothing: the models are unknown (NPC models there, no pack to name them): every pick counts as installed.
    Step Frame(const Input& in, const std::optional<std::map<std::uint16_t, std::uint16_t>>& files) {
        Step out;
        if (!s_.on || !in.pick || !in.childhood || in.title) {
            last_seen_.reset();
            return out;
        }
        std::optional<std::int32_t>& mine = *in.childhood ? s_.child : s_.adult;
        if (in.page_open || in.swap_busy) {
            if (last_seen_ && *in.pick != *last_seen_ && IsInstalled(*in.pick, files) && mine != *in.pick) {
                mine = *in.pick;   // the player picked on his page: this age's character
                out.stored_changed = true;
            }
            last_seen_ = *in.pick;
            wait_ = 0;
            return out;
        }
        last_seen_ = *in.pick;
        if (!mine) {
            mine = *in.pick;   // the first frame at this age: what is shown now becomes its pick
            out.stored_changed = true;
            return out;
        }
        const std::int32_t want = IsInstalled(*mine, files) ? *mine : 0;
        if (*in.pick == want) {
            wait_ = 0;
            said_ = -1;
            return out;
        }
        if (wait_ > 0) {
            --wait_;
            return out;
        }
        out.write = want;
        out.say = said_ != want;
        wait_ = kRewriteFrames;
        return out;
    }
    // The caller's write of Frame's value was taken by the game (its setter or queue had it): a fight over the value
    // is said once from now. A write that failed (no room, a load) is not counted, so the next one is said.
    void Wrote(std::int32_t value) { said_ = value; }

private:
    Stored s_;
    std::optional<std::int32_t> last_seen_;
    int wait_ = 0;
    std::int32_t said_ = -1;   // the value last written (taken) and logged, until the mod's value is the wanted one
};

// The pick held in place through a save load (the game's thread; the caller locks). Loaded at wrap 6 (the save is
// in, the player not made yet), Applied after each pass of the game's flag applier, until it ends: the next load, his
// page or swap (Follow, on the game's thread too) or a pick learnt (Stop, the frame), the pick wanted for Link's age now is not the held one any more
// (Follow: the Master Sword or the Door of Time within kHoldMs of the load, a pick set on the settings rows, the
// feature turned off), or kHoldMs. Its summary is said once, by the frame.
class LoadHold {
public:
    static constexpr std::int64_t kHoldMs = 20000;   // a save load's area load, with room (a slow PC)

    enum class End : std::uint8_t {
        kStopped,      // his page or swap, a pick learnt
        kNextLoad,     // another save load
        kWantChanged,  // the age or the picks changed: the held value is not this age's pick now
        kTime,         // kHoldMs
    };
    struct Summary {
        std::int32_t want = 0;
        std::uint32_t passes = 0;                 // applier passes seen while held
        std::uint32_t put_back = 0;               // of them, the ones after which the value was not ours any more
        std::optional<std::int32_t> game_value;   // the last value found there instead of ours
        End end = End::kStopped;
    };

    // `want` the pick for the save's age (Wanted; nothing: not held), `live` the Int's live value now. Returns the
    // value to write in place now (nothing: it is there already, or not held).
    std::optional<std::int32_t> Loaded(std::optional<std::int32_t> want, std::optional<std::int32_t> live,
                                       std::int64_t now_ms) {
        Stop(End::kNextLoad);   // (a hold of an earlier load: its summary is said)
        if (!want || !live) return std::nullopt;
        held_ = want;
        until_ = now_ms + kHoldMs;
        return *live != *want ? want : std::nullopt;
    }
    // Before each Applied and each frame: the picks, Link's age now (`childhood` nothing: not read, kept) and whether
    // his page is up or its swap runs (`picking`). When the value wanted now (Wanted) is not the held one, the hold
    // ends: holding it would put the other age's pick in place while the game makes this age's player. While the
    // player picks, it ends too: the mod's value is his choice, and an applier pass must not write the held pick over
    // it before the frame has seen the page.
    void Follow(const Stored& s, std::optional<bool> childhood,
                const std::optional<std::map<std::uint16_t, std::uint16_t>>& files, bool picking) {
        if (!held_) return;
        if (picking) {
            Stop(End::kStopped);
            return;
        }
        if (!s.on) {
            Stop(End::kWantChanged);
            return;
        }
        if (!childhood) return;
        if (Wanted(s, childhood, files) != held_) Stop(End::kWantChanged);
    }
    // After an applier pass: the value to write in place again (the game put another back), or nothing.
    std::optional<std::int32_t> Applied(std::optional<std::int32_t> live, std::int64_t now_ms) {
        if (!held_) return std::nullopt;
        if (now_ms >= until_) {
            Stop(End::kTime);
            return std::nullopt;
        }
        ++passes_;
        if (live && *live != *held_) {
            ++put_back_;
            game_value_ = *live;
            return held_;
        }
        return std::nullopt;
    }
    bool held() const { return held_.has_value(); }
    // Of this hold, the times the game put another value back (1: the write Applied just returned is the first).
    std::uint32_t put_backs() const { return put_back_; }
    // The frame each frame: past kHoldMs it stops (no applier pass may come after the load).
    void Expire(std::int64_t now_ms) {
        if (held_ && now_ms >= until_) Stop(End::kTime);
    }
    void Stop(End why = End::kStopped) {
        if (!held_) return;
        summary_ = Summary{*held_, passes_, put_back_, game_value_, why};
        held_.reset();
        passes_ = put_back_ = 0;
        game_value_.reset();
    }
    std::optional<Summary> TakeSummary() { return std::exchange(summary_, std::nullopt); }

private:
    std::optional<std::int32_t> held_;
    std::int64_t until_ = 0;
    std::uint32_t passes_ = 0, put_back_ = 0;
    std::optional<std::int32_t> game_value_;
    std::optional<Summary> summary_;
};

inline const char* EndName(LoadHold::End e) {
    switch (e) {
    case LoadHold::End::kStopped: return "his page, swap or a pick learnt";
    case LoadHold::End::kNextLoad: return "the next load";
    case LoadHold::End::kWantChanged: return "the age or the picks changed";
    case LoadHold::End::kTime: return "time";
    }
    return "?";
}

// --- The pickable characters of the mod's characters.json ---
struct Character {
    std::int32_t number = 0;
    std::string name;
    bool operator==(const Character&) const = default;
};

namespace detail {

inline bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

struct Reader {
    std::string_view t;
    std::size_t at = 0;
    void Space() {
        while (at < t.size() && IsSpace(t[at])) ++at;
    }
    bool Eat(char c) {
        Space();
        if (at < t.size() && t[at] == c) {
            ++at;
            return true;
        }
        return false;
    }
    std::optional<std::string> String() {
        Space();
        if (at >= t.size() || t[at] != '"') return std::nullopt;
        std::string out;
        for (++at; at < t.size(); ++at) {
            if (t[at] == '"') {
                ++at;
                return out;
            }
            if (t[at] == '\\' && at + 1 < t.size()) ++at;   // (an escaped character is kept as it is)
            out += t[at];
        }
        return std::nullopt;
    }
    // Skips one value of any kind (an object or array by its brackets, a string, or a bare word up to , } ]).
    bool Skip() {
        Space();
        if (at >= t.size()) return false;
        if (t[at] == '"') return String().has_value();
        if (t[at] == '{' || t[at] == '[') {
            int depth = 0;
            while (at < t.size()) {
                if (t[at] == '"') {
                    if (!String()) return false;
                    continue;
                }
                if (t[at] == '{' || t[at] == '[') ++depth;
                if (t[at] == '}' || t[at] == ']') --depth;
                ++at;
                if (depth == 0) return true;
            }
            return false;
        }
        while (at < t.size() && t[at] != ',' && t[at] != '}' && t[at] != ']') ++at;
        return true;
    }
};

}  // namespace detail

// The characters a player can pick on the mod's page: the "characters" rows with "picker": true, in file order, by the
// mod's stored number and name (a row without a number or a name is left out; a list that cannot be read is empty).
// Which of them have a model on this PC is CoopCharSlots::Installed's question (Pickable).
inline std::vector<Character> ParseCharacters(std::string_view text) {
    std::vector<Character> out;
    const auto key = text.find("\"characters\"");
    if (key == std::string_view::npos) return out;
    detail::Reader r{text, key + 12};
    if (!r.Eat(':') || !r.Eat('[')) return out;
    while (r.Eat('{')) {
        Character c;
        bool has_number = false, picker = false;
        std::string name;
        while (!r.Eat('}')) {
            const auto k = r.String();
            if (!k || !r.Eat(':')) return out;
            r.Space();
            if (*k == "number" || *k == "picker") {
                const std::size_t from = r.at;
                if (!r.Skip()) return out;
                std::string_view v = text.substr(from, r.at - from);
                while (!v.empty() && detail::IsSpace(v.back())) v.remove_suffix(1);
                if (*k == "picker") {
                    picker = v == "true";
                } else {
                    std::int32_t n = 0;
                    const auto res = std::from_chars(v.data(), v.data() + v.size(), n);
                    has_number = res.ec == std::errc{} && n >= 0;
                    c.number = n;
                }
            } else if (*k == "name") {
                const auto v = r.String();
                if (!v) return out;
                name = *v;
            } else if (!r.Skip()) {
                return out;
            }
            r.Eat(',');
        }
        if (has_number && picker && !name.empty()) {
            c.name = std::move(name);
            out.push_back(std::move(c));
        }
        if (!r.Eat(',')) break;
    }
    return out;
}

// The rows the settings page can offer: the pickable characters whose model is installed (`files` nothing: unknown,
// all of them). Link (0) is always there, first.
inline std::vector<Character> Pickable(const std::vector<Character>& list,
                                       const std::optional<std::map<std::uint16_t, std::uint16_t>>& files) {
    std::vector<Character> out{{0, "Link"}};
    for (const auto& c : list) {
        if (c.number == 0) continue;
        if (IsInstalled(c.number, files)) out.push_back(c);
    }
    return out;
}

// --- The module's glue (coop_hook.cpp; any thread): the picks kept in coop-character-ages.txt beside coop.ini ---
Stored Current();
void SetOn(bool on);
void SetPick(bool child, std::int32_t n);

} // namespace CoopCharAges

namespace coop::host {
class WritableGameMemory;
}

namespace CoopCharAges {
// Hookshot's wraps (patch 0024's wrap 6 and patch 0005's wrap 1, phase 1), on the game's thread: the save is in, and
// the game's flag applier has run. Quick: no file, and no lock the frame holds over a file.
void AtSaveLoaded(coop::host::WritableGameMemory& memory);
void AtFlagsApplied(coop::host::WritableGameMemory& memory);
} // namespace CoopCharAges
