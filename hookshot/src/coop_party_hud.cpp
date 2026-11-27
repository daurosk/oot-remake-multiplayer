// SPDX-License-Identifier: GPL-3.0-or-later
// The other players' hearts on the game's own HUD (coop_party_hud.h).
#include "coop_party_hud.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace CoopPartyHud {

namespace {

constexpr const char* kSay = "coop: party hud: ";
constexpr std::size_t kNameBytes = 64;                    // block+0: a name for one call
constexpr std::size_t kTextBytes = 200;                   // a line's text at most
constexpr std::size_t kLineBytes = sizeof(CoopNativeUi::Ref) + kTextBytes + 8;
constexpr std::size_t kOutAt = kNameBytes + kLineBytes * (kSlots + 2);   // block+: the path lookup's layout out
constexpr std::size_t kNamesAt = kOutAt + 16;             // block+: the run-time gauges' names (32 bytes each)
constexpr std::size_t kLiveNamesAt = kNamesAt + 32 * kSlots;   // block+: their name lines' names (32 bytes each)
constexpr std::size_t kBlockBytes = kLiveNamesAt + 32 * kSlots;
constexpr std::size_t kMore = kSlots, kSelf = kSlots + 1;   // labels_ after the players' lines


template <typename T>
bool ReadValue(Game& game, std::uint64_t at, T& out) {
    return game.Read(at, &out, sizeof out);
}

std::string PaneName(const char* what, int slot, int index = -1) {
    char b[40];
    if (index < 0) std::snprintf(b, sizeof b, "Pa_Coop%s_%d", what, slot);
    else std::snprintf(b, sizeof b, "Pa_Coop%s_%d_%02d", what, slot, index);
    return b;
}

std::string LabelPart(std::size_t index) {
    return index < static_cast<std::size_t>(kSlots) ? PaneName("Name", static_cast<int>(index))
           : index == kMore                          ? std::string("Pa_CoopMore_0")
                                                     : std::string("Pa_CoopSelf_0");
}

// As the game's pane mover (f_8022a3e0, coop_native_tags.cpp MovePane): x, y at +0x6c, then the layout is told.
bool MovePane(Game& game, std::uint64_t pane, const std::array<float, 2>& at) {
    std::uint32_t flags = 0;
    if (pane == 0 || !ReadValue(game, pane + addr::kPaneFlags, flags)) return false;
    if (!game.Write(pane + addr::kPaneXY, at.data(), sizeof(float) * 2)) return false;
    if ((flags & (1u << 20)) == 0) return true;
    std::uint32_t word[2] = {};
    if (!game.Read(pane, word, sizeof word)) return false;
    const std::uint32_t mark = word[1] & 0x40u;
    if ((mark & ~word[0]) != 0) {
        const std::uint32_t set = word[0] | mark;
        game.Write(pane, &set, sizeof set);
        game.Call(game.At(addr::kPaneMarkA), {pane, mark & ~word[0], 0, 0, 0});
    }
    return game.Call(game.At(addr::kPaneMarkB), {pane, 0, 0, 0, 0}).has_value();
}

}  // namespace

Plan PlanHearts(const std::vector<int>& quarters) {
    Plan p;
    p.hearts = static_cast<int>(std::min<std::size_t>(quarters.size(), kHearts));
    for (int i = 0; i < p.hearts; ++i) {
        const int q = std::clamp(quarters[static_cast<std::size_t>(i)], 0, 4);
        p.whole[static_cast<std::size_t>(i)] = q == 4 ? 2 : 1;   // (an empty heart under the part heart)
        if (q > 0 && q < 4 && p.part_at < 0) {
            p.part_at = i;
            p.part_quarters = q;
        }
    }
    return p;
}

float GaugeFrame(int quarters, std::int32_t last_frame) {
    const int q = std::clamp(quarters, 1, 4);
    if (last_frame <= 3) return static_cast<float>(std::min<std::int32_t>(q - 1, std::max<std::int32_t>(last_frame, 0)));
    return static_cast<float>(last_frame) * static_cast<float>(q) / 4.0f;
}

bool LowLife(int life, int max) {
    if (life <= 0 || max <= 0) return false;
    const int at = max <= 20 ? 4 : max <= 40 ? 6 : max <= 60 ? 8 : 11;
    return life <= at;
}

int GlowHeart(int was, int now, int hearts) {
    if (was < 0 || was == now || hearts <= 0) return -1;
    const int heart = now < was ? now / 4 : (now - 1) / 4;
    return std::clamp(heart, 0, std::min(hearts, kHearts) - 1);
}

float PlayedFrame(std::uint64_t ms, std::int32_t last, bool loop) {
    if (last <= 0) return 0.0f;
    const std::uint64_t f = ms * 30 / 1000;
    if (loop) return static_cast<float>(f % (static_cast<std::uint64_t>(last) + 1));
    return static_cast<float>(std::min<std::uint64_t>(f, static_cast<std::uint64_t>(last)));
}

std::array<float, 2> HeartAt(float top, int index) {
    const int i = std::clamp(index, 0, kHearts - 1);
    return {kFirstX + kStepX * static_cast<float>(i % kPerRow),
            top - kLabelHeight - kStepY * (static_cast<float>(i / kPerRow) + 0.5f)};
}

float BlockHeight(int hearts) {
    const int rows = std::clamp((hearts + kPerRow - 1) / kPerRow, 0, kHearts / kPerRow);
    return kLabelHeight + kStepY * static_cast<float>(rows) + kBlockGap;
}

float TopFrom(std::optional<double> game_hud_bottom, bool own) {
    const float own_line = own ? kLabelHeight + kBlockGap : 0.0f;
    if (!game_hud_bottom || !(*game_hud_bottom > 0.0) || !(*game_hud_bottom < 1.0)) return kDefaultTop;
    // (never so low that our own line, one block of two rows and the "+N" line leave the picture; more blocks are
    // shown as they fit, BlocksThatFit)
    const float lowest = -kLayoutHeight + kBlockGap + own_line + BlockHeight(kHearts) + kLabelHeight + kBlockGap;
    return std::max(-static_cast<float>(*game_hud_bottom) * kLayoutHeight - kBlockGap, lowest);
}

std::string FitLabel(const std::string& text) {
    const std::size_t room = static_cast<std::size_t>(kLabelRoom / kLabelEm * 100.0f);   // (CoopNativeUi::Width units)
    if (CoopNativeUi::Width(text) <= room) return text;
    const std::size_t dots = CoopNativeUi::Width("...");
    std::string out;
    std::size_t used = 0;
    for (std::size_t i = 0; i < text.size();) {
        std::size_t n = 1;   // (one whole UTF-8 letter)
        while (i + n < text.size() && (static_cast<unsigned char>(text[i + n]) & 0xc0) == 0x80) ++n;
        const std::size_t w = CoopNativeUi::Width(std::string_view(text).substr(i, n));
        if (used + w + dots > room) break;
        out.append(text, i, n);
        used += w;
        i += n;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out + "...";
}

float LabelX(const std::string& fitted) {
    return kLabelLeft + static_cast<float>(CoopNativeUi::Width(fitted)) * kLabelEm / 200.0f;
}

std::string MoreText(std::size_t hidden) {
    return hidden > 0 ? "+" + std::to_string(hidden) : std::string();
}

std::size_t BlocksThatFit(float top, const std::vector<int>& hearts, std::size_t slots) {
    const float floor = -kLayoutHeight + kBlockGap;
    for (std::size_t n = std::min(hearts.size(), slots);; --n) {
        float need = n < hearts.size() ? kLabelHeight + kBlockGap : 0.0f;   // (the "+N" line)
        for (std::size_t i = 0; i < n; ++i) need += BlockHeight(hearts[i]);
        if (top - need >= floor || n == 0) return n;
    }
}

std::uint64_t Hud::Name(Game& game, const std::string& text) {
    if (block_ == 0 || text.size() + 1 > kNameBytes || !game.Write(block_, text.c_str(), text.size() + 1)) return 0;
    return block_;
}

// The text pane at `path` ("Pa_CoopName_0/T_Text_00") of `layout`, found as the game's path text call finds each pane
// it sets: the path lookup (the last part's layout and the pane's name there), its pane table, the pane by that name.
// 0 when there is none (`why` says why).
std::uint64_t Hud::TextPane(Game& game, std::uint64_t layout, const std::string& path, std::string& why) {
    const std::uint64_t n = Name(game, path);
    const std::uint64_t out = block_ + kOutAt, none = 0;
    if (n == 0 || !game.Write(out, &none, sizeof none)) return why = "path not written", 0;
    const auto name = game.Call(game.At(addr::kPathFind), {n, layout, out, 0, 0});
    std::uint64_t part = 0;
    if (!name || *name == 0 || !ReadValue(game, out, part) || part == 0) return why = "path not in the layout", 0;
    const auto table = game.Call(game.At(addr::kPaneTable), {part + 0x40, 0, 0, 0, 0});
    if (!table || *table == 0) return why = "no pane table", 0;
    const auto pane = game.Call(game.At(addr::kTablePane), {*table, *name, 0, 0, 0});
    if (!pane || *pane == 0) return why = "pane not found", 0;
    std::uint32_t flags = 0;
    if (!ReadValue(game, *pane + addr::kPaneFlags, flags) || ((flags >> 22) & 0xfu) != addr::kTextPaneKind) {
        return why = "not a text pane", 0;
    }
    return *pane;
}

// The game's own line of text for the run-time name lines without the pack (addr::kBarePart): among the UI manager's
// running screens (as CoopNativeUi::ScreenByVtable walks them) the layout of the HUD's own layout type that has both
// kBarePart and kBareCheck (= MenuOverlay_00), and that part pane's part layout. 0 when there is none (`why` says).
std::uint64_t Hud::BarePart(Game& game, std::uint64_t hud_layout, std::string& why) {
    namespace ui = CoopNativeUi::addr;
    std::uint64_t type = 0, got = 0, manager = 0, screens = 0;
    std::uint32_t count = 0;
    if (CoopNativeUi::PlayUiDown()) return why = "a scene load runs or the title is up", 0;   // (nothing is live: ScreenLive)
    if (!ReadValue(game, hud_layout, type) || type == 0) return why = "the HUD's layout type not read", 0;
    if (!ReadValue(game, game.At(ui::kUiManagerGot), got) || got == 0 || !ReadValue(game, got, manager) || manager == 0 ||
        !ReadValue(game, manager + ui::kUiCount, count) || !ReadValue(game, manager + ui::kUiScreens, screens) ||
        screens == 0 || count > 512) {
        return why = "no screen list", 0;
    }
    const auto find = [&](std::uint64_t layout, const char* pane) -> std::uint64_t {
        const std::uint64_t n = Name(game, pane);
        const auto p = n == 0 ? std::nullopt : game.Call(game.At(ui::kPaneByName), {layout, n, 0, 0, 0});
        return p ? *p : 0;
    };
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint64_t screen = 0, layout = 0, t = 0, part = 0;
        if (!ReadValue(game, screens + i * 8ull, screen) || screen == 0 ||
            (layout = CoopNativeUi::ScreenLayout(game, screen)) == 0 || layout == hud_layout ||
            !ReadValue(game, layout, t) || t != type) {
            continue;   // (never hands the lookup a non-layout)
        }
        const std::uint64_t p = find(layout, addr::kBarePart);
        if (p == 0 || find(layout, addr::kBareCheck) == 0) continue;
        if (ReadValue(game, p + addr::kPartLayout, part) && part != 0) return part;
    }
    return why = "no " + std::string(addr::kBareScreen) + " " + addr::kBarePart + " among " + std::to_string(count) +
                 " running screens", 0;
}

bool Hud::Find(Game& game, std::uint64_t layout, std::vector<std::string>* log) {
    layout_ = layout;
    native_ = false;
    hearts_ = {};
    gauges_ = {};
    glows_ = {};
    labels_ = {};
    if (block_ == 0) {
        const auto block = game.Call(game.At(CoopNativeUi::addr::kMalloc), {kBlockBytes, 0, 0, 0, 0});
        if (!block || *block == 0) return false;
        block_ = *block;
    }
    const auto pane = [&](const std::string& name) -> std::uint64_t {
        if (ignore_pack_ && name.rfind("Pa_Coop", 0) == 0) return 0;   // (IgnorePack: a test's "no pack")
        const std::uint64_t n = Name(game, name);
        const auto p = n == 0 ? std::nullopt : game.Call(game.At(CoopNativeUi::addr::kPaneByName), {layout, n, 0, 0, 0});
        return p ? *p : 0;
    };
    const auto anim = [&](const std::string& path, std::int32_t* last = nullptr) -> std::uint64_t {
        const std::uint64_t n = Name(game, path);
        const auto a = n == 0 ? std::nullopt : game.Call(game.At(addr::kAnimFind), {layout, n, 0, 0, 0});
        if (!a || *a == 0) return 0;
        if (last != nullptr) {
            std::uint64_t player = 0;
            const auto l = ReadValue(game, *a + addr::kAnimPlayer, player) && player != 0
                               ? game.Call(game.At(addr::kAnimLastFrame), {player, 0, 0, 0, 0})
                               : std::nullopt;
            *last = l ? static_cast<std::int32_t>(static_cast<std::uint32_t>(*l)) : -1;
        }
        return *a;
    };
    // The run-time gauges' source: the game's own heart part and its parent (R2: no pack, or players past its slots).
    // The status screen gone a frame and back with the SAME layout: our clones still hang in it (each found by its own
    // name there), so they are kept; only a new layout drops them, else they would be made and attached again while the
    // old ones still show, and 0x820 bytes leak each time (review-b118 01:17 (1)).
    bool keep = layout == live_layout_;
    int kept = 0;
    for (int s = 0; s < kSlots && keep; ++s) {
        const Live& v = live_[static_cast<std::size_t>(s)];
        for (int k = 0; k < 2 && keep; ++k) {
            const std::uint64_t root = k == 0 ? v.root : v.name_root;
            if (root == 0) continue;
            char own[32];
            if (k == 0) std::snprintf(own, sizeof own, "Pa_CoopLive_%d", s);
            else std::snprintf(own, sizeof own, "Pa_CoopLiveName_%d", s);
            const std::uint64_t n = Name(game, own);
            const auto p = n == 0 ? std::nullopt : game.Call(game.At(CoopNativeUi::addr::kPaneByName), {layout, n, 0, 0, 0});
            keep = p && *p == root;
            kept += keep ? 1 : 0;
        }
    }
    if (log && layout == live_layout_ && kept > 0) {
        log->push_back(std::string(kSay) + (keep ? "the same layout again: " + std::to_string(kept) + " run-time part(s) kept"
                                                 : "the same layout address but our run-time parts are not in it: made again"));
    }
    if (!keep) Drop(game);
    gauge_part_ = parent_ = 0;
    source_at_ = {};
    if (const std::uint64_t src = pane("Pa_HeartGauge_00"); src != 0) {
        ReadValue(game, src + addr::kPartLayout, gauge_part_);
        game.Read(src + addr::kPaneXY, source_at_.data(), sizeof(float) * 2);
        parent_ = pane("N_ScaleL_00");
    }
    if (parent_ == 0) gauge_part_ = 0;
    const bool could_clone = gauge_part_ != 0;
    if (!clones_) gauge_part_ = parent_ = 0;   // (run-time clones off: AllowRunTimeClones)
    live_layout_ = layout;
    if (log && ignore_pack_) log->push_back(std::string(kSay) + "test: the pack's panes are not looked for (no pack)");
    if (log) {
        log->push_back(std::string(kSay) +
                       (gauge_part_ != 0 ? "run-time gauges can be made (Pa_HeartGauge_00 under N_ScaleL_00)"
                        : !clones_ && could_clone
                            ? "run-time gauges are off (the game's part clone crashes outside a screen's own set-up): "
                              "players past the pack's slots go to \"+N\""
                            : "no run-time gauges (Pa_HeartGauge_00 or N_ScaleL_00 not found)"));
    }
    slots_ = 0;
    bool all = true;
    if (pane(PaneName("Heart", 0, 0)) == 0) {
        if (log) {
            log->push_back(std::string(kSay) + "the HUD has no hearts of ours (an older pack): " +
                           (gauge_part_ != 0 ? "every other player gets a run-time gauge"
                                             : "the other players' hearts are hidden (nothing drawn instead)"));
        }
        if (gauge_part_ == 0) return false;
        all = false;
    }
    // The slots the pack has, each complete (an older pack has fewer than kSlots: those are used, the rest go to "+N").
    for (int s = 0; s < kSlots && all; ++s) {
        if (s > 0 && pane(PaneName("Heart", s, 0)) == 0) break;   // (no more slots in this pack)
        for (int i = 0; i <= kHearts; ++i) {   // (the last: the slot's part heart)
            const bool part = i == kHearts;
            Pane& p = part ? gauges_[static_cast<std::size_t>(s)]
                           : hearts_[static_cast<std::size_t>(s)][static_cast<std::size_t>(i)];
            const std::string name = part ? PaneName("Gauge", s) : PaneName("Heart", s, i);
            p.pane = pane(name);
            p.base = anim(name + "/BaseState");
            p.heart = anim(name + "/HeartState");
            if (part) p.gauge = anim(name + "/HeartGauge", &p.gauge_last);
            // (glow and pinch: played when there; the hearts show without them)
            p.glow_state = anim(name + "/GlowState");
            p.glow = anim(name + "/Glow", &p.glow_last);
            p.pinch = anim(name + "/Pinch", &p.pinch_last);
            p.defence[0] = anim(name + "/IsDoubleDefense", &p.defence_last[0]);
            p.defence[1] = anim(name + "/DefenceBorn", &p.defence_last[1]);
            all = all && p.pane != 0 && p.base != 0 && p.heart != 0 && (!part || p.gauge != 0);
        }
        labels_[static_cast<std::size_t>(s)].pane = pane(PaneName("Name", s));
        all = all && labels_[static_cast<std::size_t>(s)].pane != 0;
        if (all) slots_ = s + 1;
    }
    all = slots_ > 0;
    labels_[kMore].pane = pane(LabelPart(kMore));
    labels_[kSelf].pane = pane(LabelPart(kSelf));   // (an older pack without it: no own line, the hearts still show)
    // The run-time slots' name lines, made as their hearts are: the pack's line of the game's text (PaEnemyLocation_00);
    // without the pack the game's own line of the same two text panes, the pause menu's category title (MenuOverlay_00's
    // Pa_Category_00 = PaMenuCategory_00, a Resident screen; its backing panes hidden on each clone). PlayerStatus_00
    // itself has no such part (its parts: hearts, magic, buttons, the stone). Neither: hearts only, the log says so.
    name_part_ = 0;
    name_bare_ = false;
    for (const Label& l : labels_) {
        if (name_part_ == 0 && l.pane != 0) ReadValue(game, l.pane + addr::kPartLayout, name_part_);
    }
    std::string name_from = "the pack's text part";
    if (name_part_ == 0 && parent_ != 0) {
        std::string why;
        name_part_ = BarePart(game, layout, why);
        name_bare_ = name_part_ != 0;
        name_from = name_bare_ ? "the game's " + std::string(addr::kBareScreen) + " " + addr::kBarePart : why;
    }
    if (parent_ == 0) name_part_ = 0;
    if (log && gauge_part_ != 0) {
        log->push_back(std::string(kSay) + (name_part_ != 0 ? "run-time name lines can be made (" + name_from + ")"
                                                            : "no run-time name lines (" + name_from + ")"));
    }
    // A kept layout: its run-time lines back in their slots (labels_ was just cleared).
    for (int s = slots_; s < kSlots && keep; ++s) {
        const Live& v = live_[static_cast<std::size_t>(s)];
        if (v.name_root == 0) continue;
        labels_[static_cast<std::size_t>(s)].pane = v.name_root;
        labels_[static_cast<std::size_t>(s)].boxes = v.name_boxes;
    }
    // Each line's two text panes, found once per layout; the run's log says for each one whether it was found.
    for (std::size_t i = 0; i < labels_.size(); ++i) {
        Label& l = labels_[i];
        if (i >= static_cast<std::size_t>(slots_) && i < static_cast<std::size_t>(kSlots)) continue;   // (not in this pack)
        for (std::size_t k = 0; k < l.boxes.size(); ++k) {
            const std::string path = LabelPart(i) + (k == 0 ? "/T_Text_00" : "/T_Text_01");
            std::string why = "no part";
            l.boxes[k] = l.pane == 0 ? 0 : TextPane(game, layout, path, why);
            if (log) log->push_back(std::string(kSay) + "text " + path + (l.boxes[k] != 0 ? ": found" : ": not found (" + why + ")"));
        }
    }
    if (log) {
        // What the screenshots are read against: every animation of the two heart layouts, and its last frame.
        for (const char* part : {"Pa_CoopHeart_0_00", "Pa_CoopGauge_0"}) {
            for (const char* a : {"BaseState", "HeartState", "GlowState", "Glow", "Pinch", "HeartGauge", "GlowGauge",
                                  "HeartNoticeLoop", "DefenceBorn", "IsDoubleDefense"}) {
                std::int32_t last = -1;
                const bool found = anim(std::string(part) + "/" + a, &last) != 0;
                log->push_back(std::string(kSay) + "anim " + part + "/" + a +
                               (found ? ": last frame " + std::to_string(last) : ": not found"));
            }
        }
        log->push_back(std::string(kSay) + (all ? "the game's HUD shows the other players' hearts (its own heart parts, " +
                                                      std::to_string(slots_) + " slots)"
                                                : "no complete slot of ours in the pack"));
    }
    native_ = all || gauge_part_ != 0;
    return native_;
}

void Hud::Drop(Game&) {
    live_ = {};   // (the old layout's: forgotten, see Live)
}

// One run-time gauge, made as the status screen makes its own, taken out of the animation list (it would show Link's
// life), its hearts placed in the game's two rows of ten and hidden until set. The log says what was made or why not.
bool Hud::MakeLive(Game& game, int slot, std::vector<std::string>* log) {
    Live& v = live_[static_cast<std::size_t>(slot)];
    if (v.tried) return v.gauge != 0;
    v.tried = true;
    const std::string who = "run-time gauge " + std::to_string(slot) + ": ";
    const auto say = [&](const std::string& what) {
        if (log) log->push_back(std::string(kSay) + who + what);
        return false;
    };
    const auto call = [&](std::uint64_t f, std::array<std::uint64_t, 5> x) -> std::uint64_t {
        const auto r = game.Call(game.At(f), x);
        return r ? *r : 0;
    };
    if (gauge_part_ == 0 || block_ == 0) return say("no source");
    // (its name kept in our block for the clone's life: the clone may keep the pointer)
    char name[32];
    std::snprintf(name, sizeof name, "Pa_CoopLive_%d", slot);
    const std::uint64_t name_at = block_ + kNamesAt + 32 * static_cast<std::uint64_t>(slot);
    if (!game.Write(name_at, name, std::strlen(name) + 1)) return say("name not written");
    v.obj = call(addr::kNew, {8, addr::kCloneBytes, 0, 0, 0});
    if (v.obj == 0) return say("no memory");
    if (!game.Call(game.At(addr::kClonePart), {v.obj, gauge_part_, name_at, 0, 0})) return v.obj = 0, say("clone failed");
    v.root = call(addr::kPaneTable, {v.obj + 0x40, 0, 0, 0, 0});
    std::uint64_t owner = 0;
    if (v.root == 0 || !ReadValue(game, parent_ + addr::kParentOwner, owner)) {
        return v.obj = v.root = 0, say("clone has no root pane");
    }
    // Each of the game's calls checked (review-b118 01:17 (2)): not attached -> not ours to find again or show; a gauge
    // not made or set up -> its children are garbage to the setters; not unlinked -> it would show Link's own life.
    if (!game.Call(game.At(addr::kAttach), {owner, v.root, parent_, 0, 0})) return v.obj = v.root = 0, say("attach failed");
    const auto hide = [&](const std::string& why) {
        game.Call(game.At(addr::kPaneShow), {v.root, 0, 0, 0, 0});
        return say(why + ": not shown");
    };
    const std::uint64_t gauge = call(addr::kNew, {8, addr::kGaugeBytes, 0, 0, 0});
    if (gauge == 0) return hide("no memory for the gauge");
    if (!game.Call(game.At(addr::kGaugeMake), {gauge, v.obj, 0, 0, 0})) return hide("gauge make failed");
    if (!game.Call(game.At(addr::kGaugeSetUp), {gauge, 0, 1, 0, 0})) return hide("gauge set-up failed");
    if (!game.Call(game.At(addr::kGaugeUnlink), {gauge, 0, 0, 0, 0})) return hide("gauge unlink failed");
    // Its children and their part panes in the clone (Pa_NoGauge_NN, Pa_Gauge_00/01), each checked before use: the
    // setters read their animations with no check of their own.
    int hearts = 0;
    for (int i = 0; i < kHearts; ++i) {
        std::uint64_t c = 0, base = 0;
        ReadValue(game, gauge + addr::kGaugeWhole + 8 * static_cast<std::uint64_t>(i), c);
        if (c != 0 && ReadValue(game, c + addr::kHeartBase, base) && base != 0) v.whole[static_cast<std::size_t>(i)] = c;
        std::snprintf(name, sizeof name, "Pa_NoGauge_%02d", i);
        const std::uint64_t n = Name(game, name);
        v.at[static_cast<std::size_t>(i)] = n == 0 ? 0 : call(addr::kTablePane, {v.root, n, 0, 0, 0});
        hearts += v.whole[static_cast<std::size_t>(i)] != 0 && v.at[static_cast<std::size_t>(i)] != 0;
    }
    std::uint64_t part = 0, base = 0, anim = 0, player = 0;
    ReadValue(game, gauge + addr::kGaugePart, part);
    if (part != 0 && ReadValue(game, part + addr::kHeartBase, base) && base != 0 &&
        ReadValue(game, part + addr::kHeartGauge, anim) && anim != 0) {
        v.part = part;
        if (ReadValue(game, anim + addr::kAnimPlayer, player) && player != 0) {
            const auto l = game.Call(game.At(addr::kAnimLastFrame), {player, 0, 0, 0, 0});
            v.gauge_last = l ? static_cast<std::int32_t>(static_cast<std::uint32_t>(*l)) : -1;
        }
    }
    for (const char* p : {"Pa_Gauge_00", "Pa_Gauge_01"}) {
        const std::uint64_t n = Name(game, p);
        (p[10] == '0' ? v.part_pane : v.part2_pane) = n == 0 ? 0 : call(addr::kTablePane, {v.root, n, 0, 0, 0});
    }
    if (hearts < kHearts || v.part == 0 || v.part_pane == 0) {
        // (kept out of sight: hearts half there would be worse than none)
        game.Call(game.At(addr::kPaneShow), {v.root, 0, 0, 0, 0});
        return say("made but incomplete (" + std::to_string(hearts) + " of 20 hearts, part heart " +
                   (v.part != 0 && v.part_pane != 0 ? "there" : "missing") + "): not shown");
    }
    v.gauge = gauge;
    const float scale[2] = {kScale, kScale};
    game.Write(v.root + addr::kPaneScale, scale, sizeof scale);
    // The game's own rows: one heart and one row apart (78, 61) from the first heart's place.
    std::array<float, 2> first{};
    game.Read(v.at[0] + addr::kPaneXY, first.data(), sizeof first);
    for (int i = 1; i < kHearts; ++i) {
        MovePane(game, v.at[static_cast<std::size_t>(i)],
                 {first[0] + 78.0f * static_cast<float>(i % kPerRow), first[1] - 61.0f * static_cast<float>(i / kPerRow)});
    }
    if (v.part2_pane != 0) game.Call(game.At(addr::kPaneShow), {v.part2_pane, 0, 0, 0, 0});
    HideLive(game, v);
    MakeLiveName(game, slot, log);
    if (log) {
        char b[160];
        std::snprintf(b, sizeof b, "made (clone %#llx, root %#llx, gauge %#llx, first heart at %.0f, %.0f, part heart last frame %d)",
                      static_cast<unsigned long long>(v.obj), static_cast<unsigned long long>(v.root),
                      static_cast<unsigned long long>(gauge), first[0], first[1], v.gauge_last);
        say(b);
    }
    return true;
}

// A run-time slot's name line: the pack's text part cloned as the gauge is, under N_ScaleL_00, its two text panes
// found in the clone; then it is the slot's line (labels_), set, placed and hidden as the pack's own lines. No pack (or
// the clone fails): no line, the hearts still show; the log says which.
void Hud::MakeLiveName(Game& game, int slot, std::vector<std::string>* log) {
    if (name_part_ == 0 || block_ == 0) return;
    Label& l = labels_[static_cast<std::size_t>(slot)];
    const auto say = [&](const std::string& what) {
        if (log) log->push_back(std::string(kSay) + "run-time name line " + std::to_string(slot) + ": " + what);
    };
    const auto call = [&](std::uint64_t f, std::array<std::uint64_t, 5> x) -> std::uint64_t {
        const auto r = game.Call(game.At(f), x);
        return r ? *r : 0;
    };
    char name[32];
    std::snprintf(name, sizeof name, "Pa_CoopLiveName_%d", slot);
    const std::uint64_t name_at = block_ + kLiveNamesAt + 32 * static_cast<std::uint64_t>(slot);
    if (!game.Write(name_at, name, std::strlen(name) + 1)) return say("name not written");
    const std::uint64_t obj = call(addr::kNew, {8, addr::kCloneBytes, 0, 0, 0});
    if (obj == 0) return say("no memory");
    if (!game.Call(game.At(addr::kClonePart), {obj, name_part_, name_at, 0, 0})) return say("clone failed");
    const std::uint64_t root = call(addr::kPaneTable, {obj + 0x40, 0, 0, 0, 0});
    std::uint64_t owner = 0;
    if (root == 0 || !ReadValue(game, parent_ + addr::kParentOwner, owner)) return say("clone has no root pane");
    if (!game.Call(game.At(addr::kAttach), {owner, root, parent_, 0, 0})) return say("attach failed");
    game.Call(game.At(addr::kPaneShow), {root, 0, 0, 0, 0});
    live_[static_cast<std::size_t>(slot)].name_root = root;   // (attached: found again on a kept layout)
    if (name_bare_) {
        // The game's category title without its backing (addr::kBareHide), as the pack's line: text only.
        int hidden = 0;
        for (const char* h : addr::kBareHide) {
            const std::uint64_t n = Name(game, h);
            const std::uint64_t p = n == 0 ? 0 : call(addr::kTablePane, {root, n, 0, 0, 0});
            hidden += p != 0 && game.Call(game.At(addr::kPaneShow), {p, 0, 0, 0, 0}).has_value();
        }
        if (log) say(std::string("game's ") + addr::kBarePart + ": " + std::to_string(hidden) + " of 4 backing panes hidden");
    }
    int found = 0;
    for (std::size_t k = 0; k < l.boxes.size(); ++k) {
        const std::uint64_t n = Name(game, k == 0 ? "T_Text_00" : "T_Text_01");
        const std::uint64_t box = n == 0 ? 0 : call(addr::kTablePane, {root, n, 0, 0, 0});
        std::uint32_t flags = 0;
        const bool text = box != 0 && ReadValue(game, box + addr::kPaneFlags, flags) &&
                          ((flags >> 22) & 0xfu) == addr::kTextPaneKind;
        l.boxes[k] = text ? box : 0;
        found += text;
    }
    if (found == 0) return say("made but no text pane in it: not shown");
    live_[static_cast<std::size_t>(slot)].name_boxes = l.boxes;
    const float scale[2] = {kScale, kScale};   // (party_hud.py LABEL_SCALE, as the pack's lines)
    game.Write(root + addr::kPaneScale, scale, sizeof scale);
    l.pane = root;
    l.shown = 0;
    char b[96];
    std::snprintf(b, sizeof b, "made (clone %#llx, root %#llx, %d text pane(s))", static_cast<unsigned long long>(obj),
                  static_cast<unsigned long long>(root), found);
    say(b);
}

void Hud::HideLive(Game& game, Live& v) {
    if (v.root == 0 || v.shown == 0) return;
    if (game.Call(game.At(addr::kPaneShow), {v.root, 0, 0, 0, 0})) v.shown = 0;
}

// A run-time gauge set as the game sets its own: each whole heart (none past the player's), the part heart moved onto
// the heart partly filled; the whole gauge placed as the pack's copies are (HeartAt), dimmed on its root.
void Hud::StepLive(Game& game, int slot, const Plan& plan, float top, float alpha) {
    Live& v = live_[static_cast<std::size_t>(slot)];
    if (v.gauge == 0) return;
    std::array<float, 2> first{};
    game.Read(v.at[0] + addr::kPaneXY, first.data(), sizeof first);
    const std::array<float, 2> want = HeartAt(top, 0);
    const std::array<float, 2> root = {want[0] - kScale * first[0], want[1] - kScale * first[1]};
    if (!v.placed || v.root_at != root) {
        if (MovePane(game, v.root, root)) v.placed = true, v.root_at = root;
    }
    for (int i = 0; i < kHearts; ++i) {
        const int state = i < plan.hearts ? plan.whole[static_cast<std::size_t>(i)] : 0;
        int& had = v.state[static_cast<std::size_t>(i)];
        if (had != state + 1 && game.Call(game.At(addr::kHeartSet), {v.whole[static_cast<std::size_t>(i)],
                                                                     static_cast<std::uint64_t>(state), 0, 0, 0})) {
            had = state + 1;   // (0: not set yet)
        }
    }
    const bool part = plan.part_at >= 0;
    if (part) {
        if (v.part_on != plan.part_at) {
            std::array<float, 2> at{};
            game.Read(v.at[static_cast<std::size_t>(plan.part_at)] + addr::kPaneXY, at.data(), sizeof at);
            if (MovePane(game, v.part_pane, at)) v.part_on = plan.part_at;
        }
        const int frame = static_cast<int>(GaugeFrame(plan.part_quarters, v.gauge_last));
        if (v.part_frame != frame) {
            const bool a = game.Call(game.At(addr::kHeartSet), {v.part, 2, 0, 0, 0}).has_value();
            const bool b = game.Call(game.At(addr::kGaugeFrame), {v.part, static_cast<std::uint64_t>(frame), 0, 0, 0}).has_value();
            if (a && b) v.part_frame = frame;
        }
    }
    if (v.part_shown != (part ? 1 : 0) && game.Call(game.At(addr::kPaneShow), {v.part_pane, part ? 1u : 0u, 0, 0, 0})) {
        v.part_shown = part ? 1 : 0;
    }
    Alpha(game, v.root, v.alpha, alpha);
    if (v.shown != 1 && game.Call(game.At(addr::kPaneShow), {v.root, 1, 0, 0, 0})) v.shown = 1;
}

void Hud::Show(Game& game, Pane& p, bool shown) {
    if (p.pane == 0 || p.shown == (shown ? 1 : 0)) return;   // (0: a slot an older pack does not have)
    if (game.Call(game.At(addr::kPaneShow), {p.pane, shown ? 1u : 0u, 0, 0, 0})) p.shown = shown ? 1 : 0;
}

void Hud::Show(Game& game, Label& l, bool shown) {
    if (l.pane == 0 || l.shown == (shown ? 1 : 0)) return;
    if (game.Call(game.At(addr::kPaneShow), {l.pane, shown ? 1u : 0u, 0, 0, 0})) l.shown = shown ? 1 : 0;
}

void Hud::StopAt(Game& game, std::uint64_t anim, float& had, float frame) {
    if (anim == 0 || had == frame) return;
    if (game.CallFloat(game.At(addr::kAnimStopAt), anim, frame)) had = frame;
}

// The glow (f_833dc830: GlowState 3, Glow played; ended as f_833dc8f0, GlowState 0) and the pinch (Pinch played round
// while life is low), set frame by frame: no game object plays our copies.
// Double defence: IsDoubleDefense and DefenceBorn (the heart's white rim coming) held at their end with it, at 0
// without; which of them the game's own hearts use was not read (the run's log gives their last frames).
void Hud::Effects(Game& game, Pane& p, bool glowing, std::uint64_t glow_ms, bool pinch, std::uint64_t now_ms,
                  bool defence) {
    for (std::size_t i = 0; i < p.defence.size(); ++i) {
        StopAt(game, p.defence[i], p.defence_at[i],
               defence ? static_cast<float>(std::max<std::int32_t>(p.defence_last[i], 0)) : 0.0f);
    }
    if (p.glow != 0 && p.glow_state != 0) {
        StopAt(game, p.glow_state, p.glow_state_at, glowing ? kGlowState : 0.0f);
        StopAt(game, p.glow, p.glow_at, glowing ? PlayedFrame(glow_ms, p.glow_last, false) : 0.0f);
    }
    StopAt(game, p.pinch, p.pinch_at, pinch ? PlayedFrame(now_ms, p.pinch_last, true) : 0.0f);
}

void Hud::Alpha(Game& game, std::uint64_t pane, float& had, float alpha) {
    if (pane == 0 || had == alpha) return;
    if (game.CallFloat(game.At(addr::kPaneAlpha), pane, alpha)) had = alpha;
}

void Hud::Set(Game& game, Pane& p, int state, float frame, const std::array<float, 2>& at, float alpha) {
    if (!p.placed || p.at != at) {
        if (MovePane(game, p.pane, at)) p.placed = true, p.at = at;
    }
    if (p.state != state) {   // as f_833dca10: BaseState, then HeartState
        const bool a = game.CallFloat(game.At(addr::kAnimStopAt), p.base, state == 0 ? 0.0f : 1.0f).has_value();
        const bool b = game.CallFloat(game.At(addr::kAnimStopAt), p.heart, state == 2 ? 1.0f : 0.0f).has_value();
        if (a && b) p.state = state;
    }
    if (p.gauge != 0 && p.frame != frame) {   // as f_833dcb60
        if (game.CallFloat(game.At(addr::kAnimStopAt), p.gauge, frame)) p.frame = frame;
    }
    Alpha(game, p.pane, p.alpha, alpha);
    Show(game, p, true);
}

void Hud::SetLabel(Game& game, std::size_t index, const std::string& line, float y, float alpha,
                   std::vector<std::string>* log) {
    Label& l = labels_[index];
    if (l.pane == 0) return;
    // Fitted each frame (cheap: one Width unless cut), so a font measured after the line was first set counts too.
    const std::string text = FitLabel(line);
    const std::array<float, 2> at{LabelX(text), y};
    if (!l.placed || l.at != at) {
        if (MovePane(game, l.pane, at)) l.placed = true, l.at = at;
    }
    if (!l.has_text || l.text != text) {
        // The line's text in its own place, kept while it shows (a text box keeps the pointer, not a copy), set on the
        // part's two text panes as the game's path text call sets each pane: {text, bytes, the pane's own attributes}.
        const std::string clean = CoopNativeUi::Clean(text, kTextBytes);
        const std::uint64_t ref = block_ + kNameBytes + kLineBytes * index;
        CoopNativeUi::Ref r;
        r.text = ref + sizeof r;
        r.bytes = static_cast<std::uint32_t>(clean.size());
        std::string why = l.boxes[0] == 0 && l.boxes[1] == 0 ? "no text pane found" : "";
        int set = 0;
        if (why.empty() && !game.Write(r.text, clean.c_str(), clean.size() + 1)) why = "text not written";
        for (const std::uint64_t box : l.boxes) {
            if (!why.empty() || box == 0) continue;
            std::uint64_t attrs = 0;
            ReadValue(game, box + addr::kPaneMsgAttrs, attrs);
            r.end = static_cast<std::int64_t>(attrs);
            if (!game.Write(ref, &r, sizeof r)) why = "reference not written";
            else if (!game.Call(game.At(addr::kPaneSetMsg), {box, ref, 0, 0, 0})) why = "the game's set failed";
            else ++set;
        }
        if (why.empty() && set > 0) l.text = text, l.has_text = true, l.drawn_frames = 0, l.said_width = false;
        if (log && !l.said_set) {
            l.said_set = true;
            log->push_back(std::string(kSay) + "text " + LabelPart(index) +
                           (why.empty() ? ": set on " + std::to_string(set) + " text pane(s): \"" + clean + "\""
                                        : ": not set (" + why + ")"));
        }
    }
    // Once per layout: the width the game drew the text (0 after a second: set but not drawn).
    if (log && l.has_text && !l.said_width && l.boxes[0] != 0 && ++l.drawn_frames >= 2) {
        float drawn = 0.0f;
        ReadValue(game, l.boxes[0] + addr::kPaneTextWidth, drawn);
        if (drawn > 0.0f || l.drawn_frames >= 60) {
            l.said_width = true;
            log->push_back(std::string(kSay) + "text " + LabelPart(index) + " drawn " + std::to_string(static_cast<int>(drawn)) +
                           " wide" + (drawn > 0.0f ? "" : " (not drawn)"));
        }
    }
    Alpha(game, l.pane, l.alpha, alpha);
    Show(game, l, true);
}

bool Hud::Step(Game& game, const std::vector<Block>& blocks, std::optional<double> game_hud_bottom,
               std::vector<std::string>* log, std::uint64_t now_ms, const std::string& own) {
    // The status screen is called into only while it is live (CoopNativeUi::ScreenLive, party-hud-crash-c126): not
    // during a scene load (SceneLoading, logged here) and not before the game's HUD screen has run after it (listed
    // from the title on, it first runs at the first play frame); its panes are found again after.
    const bool loading = CoopNativeUi::SceneLoading();
    if (loading != said_loading_) {
        said_loading_ = loading;
        if (log) {
            log->push_back(std::string(kSay) + (loading ? std::string("a scene load began: the HUD's layout left alone until it ends")
                                                        : std::string("the scene load ended: the HUD is looked for again")));
        }
    }
    const std::uint64_t screen = CoopNativeUi::ScreenByVtable(game, addr::kStatusVtable);
    const std::uint64_t layout = CoopNativeUi::ScreenLayout(game, screen);
    if (layout == 0) {
        layout_ = 0;   // (the HUD is gone: its panes with it; found again when it is back)
        native_ = false;
        return false;
    }
    if (layout != layout_ && !Find(game, layout, log)) return false;
    if (!native_) return false;
    const bool own_shown = !own.empty() && !blocks.empty();
    float top = TopFrom(game_hud_bottom, own_shown);
    if (own_shown) {
        SetLabel(game, kSelf, own, top - kLabelHeight / 2.0f, 1.0f, log);
        top -= kLabelHeight + kBlockGap;
    } else {
        Show(game, labels_[kSelf], false);
    }
    std::vector<int> counts;
    for (const Block& b : blocks) counts.push_back(PlanHearts(b.hearts).hearts);
    // The pack's slots first, then a run-time gauge for each player past them (R2/R3).
    const std::size_t shown = BlocksThatFit(top, counts, static_cast<std::size_t>(gauge_part_ != 0 ? kSlots : slots_));
    std::size_t shown_now = shown;
    for (int s = 0; s < kSlots; ++s) {
        auto& hearts = hearts_[static_cast<std::size_t>(s)];
        Pane& gauge = gauges_[static_cast<std::size_t>(s)];
        Label& label = labels_[static_cast<std::size_t>(s)];
        Glow& glow = glows_[static_cast<std::size_t>(s)];
        const bool live = s >= slots_;
        if (static_cast<std::size_t>(s) < shown && live && !MakeLive(game, s, log)) {
            // (no run-time gauge: this player and the rest are left out, "+N" counts them below)
            for (int r = s; r < kSlots; ++r) {
                HideLive(game, live_[static_cast<std::size_t>(r)]);
                if (r >= slots_) Show(game, labels_[static_cast<std::size_t>(r)], false);
            }
            shown_now = static_cast<std::size_t>(s);
            break;
        }
        if (static_cast<std::size_t>(s) >= shown) {   // nobody there (solo: every slot), no room, or not in the pack
            glow = {};
            HideLive(game, live_[static_cast<std::size_t>(s)]);
            for (Pane& p : hearts) Show(game, p, false);
            Show(game, gauge, false);
            Show(game, label, false);
            continue;
        }
        const Block& b = blocks[static_cast<std::size_t>(s)];
        const Plan plan = PlanHearts(b.hearts);
        const float alpha = b.dim ? kDimAlpha : 1.0f;
        int life = 0;
        for (int i = 0; i < plan.hearts; ++i) life += std::clamp(b.hearts[static_cast<std::size_t>(i)], 0, 4);
        const std::string who = b.text.substr(0, b.text.find("  "));   // (the name: the place may change)
        if (glow.who != who) glow = Glow{who};
        if (const int h = GlowHeart(glow.life, life, plan.hearts); h >= 0) glow.heart = h, glow.since = now_ms;
        glow.life = life;
        const std::uint64_t glow_ms = now_ms - glow.since;
        const std::int32_t glow_last = hearts[0].glow_last;
        const std::uint64_t glow_for = glow_last > 0 ? static_cast<std::uint64_t>(glow_last) * 1000 / 30 + 1 : kGlowMs;
        if (glow.heart >= 0 && glow_ms > glow_for) glow.heart = -1;
        const bool pinch = LowLife(life, plan.hearts * 4);
        if (live) {   // (its name line: the pack's text part cloned, none without the pack)
            SetLabel(game, static_cast<std::size_t>(s), b.text, top - kLabelHeight / 2.0f, alpha, log);
            StepLive(game, s, plan, top, alpha);
            top -= BlockHeight(plan.hearts);
            continue;
        }
        SetLabel(game, static_cast<std::size_t>(s), b.text, top - kLabelHeight / 2.0f, alpha, log);
        for (int i = 0; i < kHearts; ++i) {
            Pane& p = hearts[static_cast<std::size_t>(i)];
            if (i < plan.hearts) {
                const int state = plan.whole[static_cast<std::size_t>(i)];
                Set(game, p, state, 0.0f, HeartAt(top, i), alpha);
                Effects(game, p, glow.heart == i && plan.part_at != i, glow_ms, pinch && state == 2, now_ms,
                        b.double_defence);
            } else {
                Show(game, p, false);
            }
        }
        if (plan.part_at >= 0) {
            Set(game, gauge, 2, GaugeFrame(plan.part_quarters, gauge.gauge_last), HeartAt(top, plan.part_at), alpha);
            Effects(game, gauge, glow.heart == plan.part_at, glow_ms, pinch, now_ms, b.double_defence);
        } else {
            Show(game, gauge, false);
        }
        top -= BlockHeight(plan.hearts);
    }
    const std::string more = MoreText(blocks.size() - std::min(shown, shown_now));
    if (more.empty()) Show(game, labels_[kMore], false);
    else SetLabel(game, kMore, more, top - kLabelHeight / 2.0f, 1.0f, log);
    return true;
}

std::string TestOwn() { return "You (talking)"; }

std::vector<Block> TestBlocks(std::uint64_t now_ms) {
    const int step = static_cast<int>(now_ms / kTestStepMs);
    const auto block = [&](const char* name, int max_quarters, bool dim = false) {
        Block b;
        b.dim = dim;
        const int life = max_quarters - step % (max_quarters + 1);
        b.text = std::string(name) + " " + std::to_string(life) + "/" + std::to_string(max_quarters);
        for (int i = 0; i < (max_quarters + 3) / 4; ++i) b.hearts.push_back(std::clamp(life - 4 * i, 0, 4));
        return b;
    };
    // (the second: the longest line a player can have, 16 wide letters + voice words + the longest place name)
    const std::string longest = std::string(16, 'W') + " (talking)  Inside Ganondorf's Castle";
    return {block("Twenty", 80), block(longest.c_str(), 40), block("Three", 12, true), block("Four", 16)};
}

}  // namespace CoopPartyHud
