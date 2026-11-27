// SPDX-License-Identifier: GPL-3.0-or-later
// The other players' name tags on the game's own balloons (coop_native_tags.h).
#include "coop_native_tags.h"
#include "coop_native_ui.h"   // ScreenLive: only a live screen handed out (party-hud-crash-c126)

#include <algorithm>
#include <cmath>
#include <cstring>

namespace CoopNativeTags {

namespace {

// Bytes in the UTF-8 sequence `lead` starts; 0 for a byte that cannot start one.
std::size_t SequenceBytes(unsigned char lead) {
    if (lead < 0x80) return 1;
    if (lead >= 0xC2 && lead <= 0xDF) return 2;
    if (lead >= 0xE0 && lead <= 0xEF) return 3;
    if (lead >= 0xF0 && lead <= 0xF4) return 4;
    return 0;
}

// Whether s[i..i+n) is a whole, shortest-form, non-surrogate UTF-8 character.
bool WholeCharacter(std::string_view s, std::size_t i, std::size_t n) {
    if (i + n > s.size()) return false;
    for (std::size_t k = 1; k < n; ++k) {
        if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
    }
    const auto b0 = static_cast<unsigned char>(s[i]);
    const auto b1 = n > 1 ? static_cast<unsigned char>(s[i + 1]) : 0;
    if (n == 3 && ((b0 == 0xE0 && b1 < 0xA0) || (b0 == 0xED && b1 >= 0xA0))) return false;  // overlong, surrogate
    if (n == 4 && ((b0 == 0xF0 && b1 < 0x90) || (b0 == 0xF4 && b1 >= 0x90))) return false;  // overlong, past U+10FFFF
    return true;
}

template <typename T>
bool ReadValue(Game& game, std::uint64_t address, T& out) {
    return game.Read(address, &out, sizeof out);
}

// A message reference as the game's text setter reads it: {const char* utf8, u32 bytes, i64 -1}.
struct Ref {
    std::uint64_t text = 0;
    std::uint32_t bytes = 0;
    std::uint32_t pad = 0;
    std::int64_t end = -1;
};
static_assert(sizeof(Ref) == 24);

constexpr std::uint64_t kColours = Balloons::kSlotBytes * addr::kParts;   // block+: the colour slots
constexpr std::uint64_t kPaneName = kColours + 16 * addr::kParts;         // block+: "T_Text_00"
constexpr char kTextPane[] = "T_Text_00";
static_assert(sizeof kTextPane <= 16);

std::uint64_t PartRoot(Game& game, std::uint64_t part) {
    if (part == 0) return 0;
    const auto root = game.Call(game.MainBase() + addr::kPartRootPane, {part + 0x40, 0, 0, 0, 0});
    return root ? *root : 0;
}

bool ShowPane(Game& game, std::uint64_t pane, bool shown) {
    return pane != 0 && game.Call(game.MainBase() + addr::kPaneShow, {pane, shown ? 1u : 0u, 0, 0, 0}).has_value();
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
        if (out.size() + piece.size() > max_bytes) break;
        out.append(piece);
        i += n;
    }
    return out;
}

std::array<float, 2> LayoutPoint(float x, float y, float width, float height) {
    if (!(width > 0.0f) || !(height > 0.0f)) return {0.0f, 0.0f};
    return {(x / width * 2.0f - 1.0f) * addr::kHalfWidth, (1.0f - y / height * 2.0f) * addr::kHalfHeight};
}

std::vector<PartTag> PlanParts(const std::vector<Spot>& far_to_near, float width, float height) {
    std::vector<PartTag> out;
    if (!(width > 0.0f) || !(height > 0.0f)) return out;
    for (auto it = far_to_near.rbegin(); it != far_to_near.rend() && out.size() < kMaxTags; ++it) {
        if (!(it->alpha > 0.0f) || !std::isfinite(it->x) || !std::isfinite(it->y)) continue;
        PartTag t;
        t.name = Clean(it->name, kNameBytes);
        if (t.name.empty()) continue;
        t.at = LayoutPoint(it->x, it->y, width, height);
        if (std::fabs(t.at[0]) > addr::kHalfWidth || std::fabs(t.at[1]) > addr::kHalfHeight) continue;
        t.part = addr::kParts - 1 - out.size();
        t.rgb = it->rgb & 0xFFFFFFu;
        out.push_back(std::move(t));
    }
    return out;
}

std::uint64_t ScreenByVtable(Game& game, std::uint64_t vtable_offset) {
    if (CoopNativeUi::PlayUiDown()) return 0;   // (nothing is live: CoopNativeUi::ScreenLive)
    const std::uint64_t base = game.MainBase();
    std::uint64_t got = 0, manager = 0, screens = 0;
    std::uint32_t count = 0;
    if (!ReadValue(game, base + addr::kUiManagerGot, got) || got == 0 || !ReadValue(game, got, manager) ||
        manager == 0 || !ReadValue(game, manager + addr::kUiCount, count) ||
        !ReadValue(game, manager + addr::kUiScreens, screens) || screens == 0 || count > 512) {
        return 0;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint64_t screen = 0, vt = 0;
        if (ReadValue(game, screens + i * 8ull, screen) && screen != 0 && ReadValue(game, screen, vt) &&
            vt == base + vtable_offset && CoopNativeUi::ScreenLive(screen)) {
            return screen;
        }
    }
    return 0;
}

// As f_8022a3e0 (pane, s0 x, s1 y): the call takes two floats, so its steps are done here. Bit 8 of the flags: a
// pane the game places another way (not a balloon's root): left alone. Same place: nothing. Else x, y at +0x6c; with
// bit 20, the pane's dirty bit 6 is set as the game does (f_815457d0 with the newly set bits) and f_8154d540 tells the
// layout.
bool MovePane(Game& game, std::uint64_t pane, const std::array<float, 2>& at) {
    std::uint32_t flags = 0;
    if (pane == 0 || !ReadValue(game, pane + addr::kPaneFlags, flags) || (flags & (1u << 8)) != 0) return false;
    float now[2] = {};
    if (game.Read(pane + addr::kPaneXY, now, sizeof now) && now[0] == at[0] && now[1] == at[1]) return true;
    if (!game.Write(pane + addr::kPaneXY, at.data(), sizeof(float) * 2)) return false;
    if ((flags & (1u << 20)) == 0) return true;
    return MarkPaneMoved(game, pane);
}

bool MarkPaneMoved(Game& game, std::uint64_t pane) {
    std::uint32_t word[2] = {};
    if (!game.Read(pane, word, sizeof word)) return false;
    const std::uint32_t mark = word[1] & 0x40u;
    if ((mark & ~word[0]) != 0) {
        const std::uint32_t set = word[0] | mark;
        game.Write(pane, &set, sizeof set);
        game.Call(game.MainBase() + addr::kPaneMarkA, {pane, mark & ~word[0], 0, 0, 0});
    }
    return game.Call(game.MainBase() + addr::kPaneMarkB, {pane, 0, 0, 0, 0}).has_value();
}

std::vector<Held> LayoutsHeld(Game& game, std::uint64_t screen, std::size_t bytes, std::string_view contains) {
    std::vector<Held> out;
    std::vector<std::uint64_t> words(bytes / 8);
    if (screen == 0 || words.empty() || !game.Read(screen, words.data(), words.size() * 8)) return out;
    for (std::size_t i = 0; i < words.size(); ++i) {
        const std::uint64_t p = words[i];
        std::uint64_t res = 0, name_at = 0;
        if (p < 0x80000000ull || (p & 7) != 0 || !ReadValue(game, p + 0xf8, res) || res < 0x80000000ull ||
            !ReadValue(game, res + 0x18, name_at) || name_at < 0x80000000ull) {
            continue;
        }
        char text[48] = {};
        if (!game.Read(name_at, text, sizeof text - 1)) continue;
        const std::string name(text);   // (the last byte stays 0)
        bool readable = name.size() >= 4;
        for (const char c : name) readable = readable && c > 0x20 && c < 0x7f;
        if (!readable || name.find(contains) == std::string::npos) continue;
        out.push_back({i * 8, p, name});
    }
    return out;
}

bool Balloons::Block(Game& game) {
    if (block_ != 0) return true;
    if (block_failed_) return false;
    const auto got = game.Call(game.MainBase() + addr::kMalloc, {kBlockBytes, 0, 0, 0, 0});
    if (!got || *got == 0) {
        block_failed_ = true;
        return false;
    }
    std::vector<std::uint8_t> zero(kBlockBytes, 0);
    std::memcpy(zero.data() + kPaneName, kTextPane, sizeof kTextPane);
    if (!game.Write(*got, zero.data(), zero.size())) {
        block_failed_ = true;
        return false;
    }
    block_ = *got;
    return true;
}

// The part's T_Text_00 shows `name` (from the part's own slot: the text box keeps the pointer, so a slot is only
// written again for a new name on that part).
bool Balloons::Name(Game& game, std::size_t i, std::uint64_t part, const std::string& name) {
    if (named_on_[i] == part && named_[i] == name) return true;
    const std::uint64_t slot = block_ + i * kSlotBytes;
    std::string bytes = name.substr(0, kSlotBytes - sizeof(Ref) - 1);
    bytes.push_back('\0');
    Ref ref;
    ref.text = slot + sizeof(Ref);
    ref.bytes = static_cast<std::uint32_t>(bytes.size() - 1);
    if (!game.Write(ref.text, bytes.data(), bytes.size()) || !game.Write(slot, &ref, sizeof ref) ||
        !game.Call(game.MainBase() + addr::kSetPaneText, {part, block_ + kPaneName, slot, 1, 0})) {
        named_on_[i] = 0;
        return false;
    }
    named_on_[i] = part;
    named_[i] = name;
    return true;
}

// The player's colour on the part's text: both colours of the text box (the top and the bottom of its gradient) as
// the layout's own animation sets them. Only on a text pane (kind 2, bits 22 to 25 of the flags); left as the game
// has it otherwise.
void Balloons::Colour(Game& game, std::size_t i, std::uint64_t part, std::uint32_t rgb) {
    if (rgb_on_[i] == part && rgb_[i] == rgb) return;
    rgb_on_[i] = part;
    rgb_[i] = rgb;
    const auto pane = game.Call(game.MainBase() + addr::kPaneByName, {part, block_ + kPaneName, 0, 0, 0});
    std::uint32_t flags = 0;
    std::uint64_t inner = 0, textbox = 0;
    if (!pane || *pane == 0 || !ReadValue(game, *pane + addr::kPaneFlags, flags) || ((flags >> 22) & 0xF) != 2 ||
        !ReadValue(game, *pane + 8, inner) || inner == 0 || !ReadValue(game, inner + 8, textbox) || textbox == 0) {
        return;
    }
    const float rgba[4] = {static_cast<float>((rgb >> 16) & 0xFF) / 255.0f, static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                           static_cast<float>(rgb & 0xFF) / 255.0f, 1.0f};
    const std::uint64_t at = block_ + kColours + i * 16;
    if (!game.Write(at, rgba, sizeof rgba)) return;
    game.Call(game.MainBase() + addr::kTextColour0, {textbox, at, 0, 0, 0});
    game.Call(game.MainBase() + addr::kTextColour1, {textbox, at, 0, 0, 0});
}

bool Balloons::Step(Game& game, const std::vector<Spot>& far_to_near, float width, float height) {
    const std::uint64_t screen = ScreenByVtable(game, addr::kBalloonVtable);
    if (screen != screen_) {
        screen_ = screen;
        used_.clear();  // a new screen (a load): the old panes went with the old one
        named_on_ = {};
        rgb_on_ = {};
    }
    if (screen == 0) return false;
    const auto plan = PlanParts(far_to_near, width, height);
    std::vector<std::uint64_t> now;
    if (!plan.empty() && Block(game)) {
        for (const auto& tag : plan) {
            std::uint64_t part = 0;
            if (!ReadValue(game, screen + addr::kPartTable + tag.part * addr::kPartStride, part) || part == 0 ||
                !Name(game, tag.part, part, tag.name)) {
                continue;
            }
            Colour(game, tag.part, part, tag.rgb);
            const std::uint64_t root = PartRoot(game, part);
            if (root != 0 && MovePane(game, root, tag.at) && ShowPane(game, root, true)) now.push_back(root);
        }
    }
    for (const std::uint64_t old : used_) {
        if (std::find(now.begin(), now.end(), old) == now.end()) ShowPane(game, old, false);
    }
    used_ = std::move(now);
    return block_ != 0 || plan.empty();
}

void Balloons::HideAll(Game& game) {
    for (const std::uint64_t pane : used_) ShowPane(game, pane, false);
    used_.clear();
}

}  // namespace CoopNativeTags
