// SPDX-License-Identifier: GPL-3.0-or-later
// Co-op's chat and name tags as the game's own UI (coop_native_ui.h).
#include "coop_native_ui.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace CoopNativeUi {

namespace {

// Bytes in the UTF-8 sequence `lead` starts; 0 for a byte that cannot start one.
std::size_t SequenceBytes(unsigned char lead) {
    if (lead < 0x80) {
        return 1;
    }
    if (lead >= 0xC2 && lead <= 0xDF) {
        return 2;
    }
    if (lead >= 0xE0 && lead <= 0xEF) {
        return 3;
    }
    if (lead >= 0xF0 && lead <= 0xF4) {
        return 4;
    }
    return 0;
}

// Whether s[i..i+n) is a whole, shortest-form, non-surrogate UTF-8 character.
bool WholeCharacter(std::string_view s, std::size_t i, std::size_t n) {
    if (i + n > s.size()) {
        return false;
    }
    for (std::size_t k = 1; k < n; ++k) {
        if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) {
            return false;
        }
    }
    const auto b0 = static_cast<unsigned char>(s[i]);
    const auto b1 = n > 1 ? static_cast<unsigned char>(s[i + 1]) : 0;
    if (n == 3 && ((b0 == 0xE0 && b1 < 0xA0) || (b0 == 0xED && b1 >= 0xA0))) {
        return false;  // overlong, or a surrogate
    }
    if (n == 4 && ((b0 == 0xF0 && b1 < 0x90) || (b0 == 0xF4 && b1 >= 0x90))) {
        return false;  // overlong, or past U+10FFFF
    }
    return true;
}

template <typename T>
bool ReadValue(Game& game, std::uint64_t address, T& out) {
    return game.Read(address, &out, sizeof out);
}

template <typename T>
bool WriteValue(Game& game, std::uint64_t address, const T& in) {
    return game.Write(address, &in, sizeof in);
}

// The screen's open call (vtable slot kSlotOpen) with `mode`.
bool Open(Game& game, std::uint64_t screen, std::uint64_t mode) {
    std::uint64_t vtable = 0;
    std::uint64_t open = 0;
    if (!ReadValue(game, screen, vtable) || vtable == 0 ||
        !ReadValue(game, vtable + addr::kSlotOpen * 8, open) || open == 0) {
        return false;
    }
    return game.Call(open, {screen, mode, 0, 0, 0}).has_value();
}

// Sets the pane named by the game string at `pane_name` in the screen's layout.
bool SetPane(Game& game, std::uint64_t screen, std::uint64_t pane_name, std::uint64_t ref) {
    std::uint64_t layout = 0;
    if (!ReadValue(game, screen + addr::kScreenLayout, layout) || layout == 0) {
        return false;
    }
    return game.Call(addr::kSetPaneText, {layout, pane_name, ref, 1, 0}).has_value();
}

}  // namespace

std::string Clean(std::string_view utf8, std::size_t max_bytes) {
    std::string out;
    out.reserve(std::min(utf8.size(), max_bytes));
    std::size_t i = 0;
    while (i < utf8.size()) {
        const auto lead = static_cast<unsigned char>(utf8[i]);
        std::size_t n = SequenceBytes(lead);
        std::string_view piece;
        if (n == 0 || !WholeCharacter(utf8, i, n)) {
            piece = "?";
            n = 1;  // and start again at the next byte
        } else if (n == 1 && (lead < 0x20 || lead == 0x7F)) {
            piece = lead == '\t' || lead == '\n' || lead == '\r' ? " " : "";  // tags (0x0E) and controls go
        } else {
            piece = utf8.substr(i, n);
        }
        if (out.size() + piece.size() > max_bytes) {
            break;
        }
        out.append(piece);
        i += n;
    }
    return out;
}

Said SplitSpeaker(std::string_view line) {
    const auto at = line.find(": ");
    if (at == std::string_view::npos || at == 0) {
        return {"", std::string(line)};
    }
    return {std::string(line.substr(0, at)), std::string(line.substr(at + 2))};
}

std::uint32_t Colour(std::uint8_t number) {
    // 1-4: coop::protocol::slot_color. 5-16: between them on the wheel, then
    // lighter and darker ones, so neighbours in number stay far apart.
    static constexpr std::array<std::uint32_t, kMaxPlayers> kColours = {
        0x3070FF,  // 1 blue
        0xFF3030,  // 2 red
        0x30D040,  // 3 green
        0xFFC020,  // 4 yellow
        0xB050FF,  // 5 purple
        0x20D0D0,  // 6 cyan
        0xFF8020,  // 7 orange
        0xFF60C0,  // 8 pink
        0x90E030,  // 9 lime
        0x7090FF,  // 10 periwinkle
        0xE0E0E0,  // 11 silver
        0xC08040,  // 12 brown
        0x30A090,  // 13 teal
        0xFF9090,  // 14 salmon
        0xA0A0FF,  // 15 lavender
        0xF0F070,  // 16 cream
    };
    if (number < 1 || number > kMaxPlayers) {
        return 0xFFFFFF;
    }
    return kColours[number - 1];
}

float ChatFeed::Seconds(std::size_t chars) {
    return std::min(8.0f, 3.0f + 0.06f * static_cast<float>(chars));
}

void ChatFeed::Update(const std::vector<ChatIn>& lines, std::uint64_t now_ms) {
    for (const auto& line : lines) {
        const std::uint64_t came = now_ms >= line.age_ms ? now_ms - line.age_ms : 0;
        // Two looks at one line can differ by a frame in age: the step absorbs it,
        // and the step either side is checked too.
        const std::uint64_t step = came / kSameMs;
        if (seen_.count({line.text, step}) != 0 || seen_.count({line.text, step + 1}) != 0 ||
            (step > 0 && seen_.count({line.text, step - 1}) != 0)) {
            continue;
        }
        seen_.insert({line.text, step});
        if (!primed_) {
            continue;  // there before co-op's UI was: already read in the old look
        }
        Shown shown;
        shown.notice = line.notice;
        if (line.notice) {
            shown.text = Clean(line.text, kTextBytes);
        } else {
            const Said said = SplitSpeaker(line.text);
            shown.speaker = Clean(said.speaker, kSpeakerBytes);
            shown.text = Clean(said.text, kTextBytes);
        }
        if (shown.text.empty()) {
            continue;
        }
        shown.seconds = Seconds(shown.text.size());
        shown.came_ms = came;
        waiting_.push_back(std::move(shown));
        while (waiting_.size() > kWaiting) {
            waiting_.pop_front();
        }
    }
    primed_ = true;
    // Forget what can no longer come back (the hook keeps a few lines only).
    const std::uint64_t keep_from = now_ms > 10 * kStaleMs ? (now_ms - 10 * kStaleMs) / kSameMs : 0;
    for (auto it = seen_.begin(); it != seen_.end();) {
        it = it->second < keep_from ? seen_.erase(it) : std::next(it);
    }
}

void ChatFeed::PutBack(Shown line) {
    waiting_.push_front(std::move(line));
    busy_until_ = 0;
}

std::optional<Shown> ChatFeed::Next(std::uint64_t now_ms) {
    if (Busy(now_ms)) {
        return std::nullopt;
    }
    while (!waiting_.empty()) {
        Shown w = std::move(waiting_.front());
        waiting_.pop_front();
        if (now_ms > w.came_ms + kStaleMs) {
            continue;
        }
        busy_until_ = now_ms + static_cast<std::uint64_t>(w.seconds * 1000.0f);
        return w;
    }
    return std::nullopt;
}

std::vector<Tag> PlanTags(const std::vector<TagIn>& players, const std::array<float, 3>& self) {
    struct Near {
        float distance;
        const TagIn* in;
    };
    std::vector<Near> near;
    for (const auto& p : players) {
        if (p.hidden || p.number == 0) {
            continue;
        }
        const float dx = p.head[0] - self[0];
        const float dy = p.head[1] - self[1];
        const float dz = p.head[2] - self[2];
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (!std::isfinite(d) || d > kTagRange) {
            continue;
        }
        near.push_back({d, &p});
    }
    std::stable_sort(near.begin(), near.end(), [](const Near& a, const Near& b) {
        return a.distance < b.distance || (a.distance == b.distance && a.in->number < b.in->number);
    });
    if (near.size() > kMaxTags) {
        near.resize(kMaxTags);
    }
    std::vector<Tag> tags;
    tags.reserve(near.size());
    for (const auto& n : near) {
        Tag t;
        t.number = n.in->number;
        t.name = Clean(n.in->name, kSpeakerBytes);
        t.colour = Colour(n.in->number);
        t.head = n.in->head;
        t.alpha = n.distance <= kTagFadeFrom
                      ? 1.0f
                      : std::max(0.0f, (kTagRange - n.distance) / (kTagRange - kTagFadeFrom));
        tags.push_back(std::move(t));
    }
    return tags;
}

std::uint64_t TextRing::Put(Game& game, std::string_view text) {
    if (base_ == 0) {
        return 0;
    }
    const std::string cut = Clean(text, kSlotText);  // already clean text stays as it is
    const std::uint64_t slot = base_ + (next_ % kSlots) * kSlotBytes;
    next_ = (next_ + 1) % kSlots;
    const std::uint64_t at = slot + sizeof(Ref);
    std::string bytes = cut;
    bytes.push_back('\0');
    Ref ref;
    ref.text = at;
    ref.bytes = static_cast<std::uint32_t>(cut.size());
    if (!game.Write(at, bytes.data(), bytes.size()) || !WriteValue(game, slot, ref)) {
        return 0;
    }
    return slot;
}

std::uint64_t ScreenByVtable(Game& game, std::uint64_t vtable) {
    std::uint64_t got = 0;
    std::uint64_t manager = 0;
    std::uint32_t count = 0;
    std::uint64_t screens = 0;
    if (!ReadValue(game, addr::kUiManagerGot, got) || got == 0 || !ReadValue(game, got, manager) ||
        manager == 0 || !ReadValue(game, manager + addr::kUiCount, count) ||
        !ReadValue(game, manager + addr::kUiScreens, screens) || screens == 0 || count > 512) {
        return 0;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint64_t screen = 0;
        std::uint64_t vt = 0;
        if (ReadValue(game, screens + i * 8ull, screen) && screen != 0 && ReadValue(game, screen, vt) &&
            vt == vtable) {
            return screen;
        }
    }
    return 0;
}

std::uint64_t InGameScreen(Game& game) {
    std::uint64_t manager = 0;
    if (!ReadValue(game, addr::kInGameScreenGlobal, manager) || manager == 0) {
        return 0;
    }
    const auto screen = game.Call(addr::kInGameScreenGet, {manager, 0, 0, 0, 0});
    if (!screen || *screen == 0) {
        return 0;
    }
    std::uint64_t vt = 0;
    if (!ReadValue(game, *screen, vt) || vt != addr::kInGameVtable) {
        return 0;  // not the window this build has: change nothing
    }
    return *screen;
}

bool InGameFree(Game& game, std::uint64_t screen, const TextRing& ring) {
    float time = 0.0f;
    std::uint64_t text = 0;
    if (!ReadValue(game, screen + addr::kInGameTime, time) || !ReadValue(game, screen + addr::kInGameText, text)) {
        return false;
    }
    return !(time > 0.0f) || text == 0 || ring.Owns(text);
}

bool ShowLine(Game& game, TextRing& ring, const Shown& line, bool refresh) {
    const std::uint64_t screen = InGameScreen(game);
    if (screen == 0 || !InGameFree(game, screen, ring)) {
        return false;
    }
    const std::uint64_t name_ref = ring.Put(game, line.speaker);
    const std::uint64_t text_ref = ring.Put(game, line.text);
    Ref text{};
    if (name_ref == 0 || text_ref == 0 || !ReadValue(game, text_ref, text)) {
        return false;
    }
    if (!SetPane(game, screen, addr::kNameT_Name_00, name_ref) ||
        !WriteValue(game, screen + addr::kInGameText, text) ||
        !WriteValue(game, screen + addr::kInGameTime, line.seconds * addr::kFramesPerSecond)) {
        return false;
    }
    // As f_833d25c0: the text is set on the window's next step when it is
    // already up; when it is not, opening it sets it.
    std::int8_t a = 0;
    std::uint8_t b = 0;
    if (ReadValue(game, screen + addr::kScreenOpenA, a) && ReadValue(game, screen + addr::kScreenOpenB, b) &&
        b == 0 && a >= 1) {
        const std::uint8_t one = 1;
        WriteValue(game, screen + addr::kInGameShow, one);
        if (refresh) {
            return true;  // up already: the window's next step shows the new text, no opening again
        }
    }
    return Open(game, screen, 1);
}

bool ShowNotice(Game& game, TextRing& ring, const Shown& notice) {
    const std::uint64_t screen = ScreenByVtable(game, addr::kInfoVtable);
    if (screen == 0) {
        return false;
    }
    const std::uint64_t ref = ring.Put(game, notice.text);
    return ref != 0 && SetPane(game, screen, addr::kNameT_Text_00, ref) && Open(game, screen, 2);
}

std::optional<std::array<float, 2>> Project(Game& game, std::uint64_t scratch, const std::array<float, 3>& world) {
    // scratch: 0 out xy, 8 depth, 16 world xyz.
    if (scratch == 0 || !game.Write(scratch + 16, world.data(), sizeof(float) * 3)) {
        return std::nullopt;
    }
    const float zero[3] = {0, 0, 0};
    game.Write(scratch, zero, sizeof zero);
    const auto on = game.Call(addr::kProject, {scratch, scratch + 16, scratch + 8, 0, 0});
    std::array<float, 2> xy{};
    float depth = 0.0f;
    if (!on || (*on & 1) == 0 || !game.Read(scratch, xy.data(), sizeof xy) || !ReadValue(game, scratch + 8, depth) ||
        depth < 0.0f || !std::isfinite(xy[0]) || !std::isfinite(xy[1])) {
        return std::nullopt;
    }
    return xy;
}

}  // namespace CoopNativeUi

namespace CoopNativeUi {

Shown TypingLine(std::string_view speaker, std::string_view typed, bool failed) {
    Shown s;
    s.speaker = Clean(speaker, kSpeakerBytes);
    // Clean keeps the text as the game shows it; the caret is the game font's own underscore.
    s.text = Clean(typed, kTextBytes) + "_";
    if (failed) {
        s.text += "  (not sent)";
    }
    s.seconds = kTypingSeconds;
    return s;
}

}  // namespace CoopNativeUi

namespace CoopNativeUi {

bool EndLine(Game& game, const TextRing& ring) {
    const std::uint64_t screen = InGameScreen(game);
    std::uint64_t text = 0;
    if (screen == 0 || !game.Read(screen + addr::kInGameText, &text, sizeof text) || !ring.Owns(text)) {
        return false;
    }
    const float zero = 0.0f;
    return game.Write(screen + addr::kInGameTime, &zero, sizeof zero);
}

}  // namespace CoopNativeUi

namespace CoopNativeUi {

std::uint64_t TextRing::PutFixed(Game& game, std::size_t index, std::string_view text) {
    if (base_ == 0 || index >= kFixed) {
        return 0;
    }
    const std::uint64_t slot = base_ + kRingBytes + index * kSlotBytes;
    const std::string cut = Clean(text, kSlotText);
    if (fixed_set_[index] && fixed_[index] == cut) {
        return slot;
    }
    std::string bytes = cut;
    bytes.push_back('\0');
    Ref ref;
    ref.text = slot + sizeof(Ref);
    ref.bytes = static_cast<std::uint32_t>(cut.size());
    if (!game.Write(ref.text, bytes.data(), bytes.size()) || !game.Write(slot, &ref, sizeof ref)) {
        fixed_set_[index] = false;
        return 0;
    }
    fixed_[index] = cut;
    fixed_set_[index] = true;
    return slot;
}

std::array<float, 2> LayoutPoint(float x, float y, float width, float height) {
    if (!(width > 0.0f) || !(height > 0.0f)) {
        return {0.0f, 0.0f};
    }
    return {(x / width - 0.5f) * addr::kLayoutWidth, (0.5f - y / height) * addr::kLayoutHeight};
}

std::vector<PartTag> PlanParts(const std::vector<TagSpot>& far_to_near, float width, float height) {
    std::vector<PartTag> out;
    for (auto it = far_to_near.rbegin(); it != far_to_near.rend() && out.size() < kMaxTags; ++it) {
        if (!(it->alpha > 0.0f) || !std::isfinite(it->x) || !std::isfinite(it->y)) {
            continue;
        }
        PartTag t;
        t.part = addr::kBalloonParts - 1 - out.size();
        t.name = Clean(it->name, kSpeakerBytes);
        t.at = LayoutPoint(it->x, it->y, width, height);
        out.push_back(std::move(t));
    }
    return out;
}

std::array<std::uint64_t, addr::kBalloonParts> BalloonParts(Game& game, std::uint64_t screen) {
    std::array<std::uint64_t, addr::kBalloonParts> parts{};
    for (std::size_t i = 0; i < parts.size(); ++i) {
        std::uint64_t p = 0;
        if (screen != 0 && game.Read(screen + addr::kBalloonPartTable + i * addr::kBalloonPartStride, &p, sizeof p)) {
            parts[i] = p;
        }
    }
    return parts;
}

std::uint64_t PartRoot(Game& game, std::uint64_t part) {
    if (part == 0) {
        return 0;
    }
    const auto root = game.Call(addr::kPartRootPane, {part + 0x40, 0, 0, 0, 0});
    return root ? *root : 0;
}

bool MovePane(Game& game, std::uint64_t pane, const std::array<float, 2>& at) {
    std::uint32_t flags = 0;
    if (pane == 0 || !game.Read(pane + addr::kPaneFlags, &flags, sizeof flags) || (flags & (1u << 8)) != 0) {
        return false;  // (bit 8: a pane kind that keeps its place elsewhere; not a balloon's)
    }
    float now[2] = {};
    if (game.Read(pane + addr::kPaneXY, now, sizeof now) && now[0] == at[0] && now[1] == at[1]) {
        return true;
    }
    if (!game.Write(pane + addr::kPaneXY, at.data(), sizeof(float) * 2)) {
        return false;
    }
    if ((flags & (1u << 20)) == 0) {
        return true;
    }
    std::uint32_t word[2] = {};
    if (!game.Read(pane, word, sizeof word)) {
        return false;
    }
    const std::uint32_t mark = word[1] & 0x40u;
    if ((mark & ~word[0]) != 0) {
        const std::uint32_t set = word[0] | mark;
        game.Write(pane, &set, sizeof set);
        game.Call(addr::kPaneMarkA, {pane, mark & ~word[0], 0, 0, 0});
    }
    return game.Call(addr::kPaneMarkB, {pane, 0, 0, 0, 0}).has_value();
}

bool ShowPane(Game& game, std::uint64_t pane, bool shown) {
    return pane != 0 && game.Call(addr::kPaneShow, {pane, shown ? 1u : 0u, 0, 0, 0}).has_value();
}

bool Balloons::Step(Game& game, TextRing& ring, const std::vector<TagSpot>& far_to_near, float width, float height) {
    const std::uint64_t screen = ScreenByVtable(game, addr::kBalloonVtable);
    if (screen != screen_) {
        screen_ = screen;
        used_.clear();  // a new screen (a load): the old panes are gone with it
        named_on_ = {};
    }
    if (screen == 0) {
        return false;
    }
    const auto parts = BalloonParts(game, screen);
    std::vector<std::uint64_t> now;
    for (const auto& tag : PlanParts(far_to_near, width, height)) {
        const std::uint64_t part = parts[tag.part];
        const std::uint64_t ref = ring.PutFixed(game, tag.part, tag.name);
        if (part == 0 || ref == 0) {
            continue;
        }
        if (named_on_[tag.part] != part || named_[tag.part] != tag.name) {
            game.Call(addr::kSetPaneText, {part, addr::kNameT_Text_00, ref, 1, 0});
            named_on_[tag.part] = part;
            named_[tag.part] = tag.name;
        }
        const std::uint64_t root = PartRoot(game, part);
        if (root != 0 && MovePane(game, root, tag.at) && ShowPane(game, root, true)) {
            now.push_back(root);
        }
    }
    for (const std::uint64_t old : used_) {
        if (std::find(now.begin(), now.end(), old) == now.end()) {
            ShowPane(game, old, false);
        }
    }
    used_ = std::move(now);
    return true;
}

void Balloons::HideAll(Game& game) {
    for (const std::uint64_t pane : used_) {
        ShowPane(game, pane, false);
    }
    used_.clear();
}

}  // namespace CoopNativeUi
