// SPDX-License-Identifier: GPL-3.0-or-later
// The other players on the game's own maps as copies of its map icon part (coop_map_icons.h).
#include "coop_map_icons.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <utility>

namespace CoopMapIcons {

namespace addr = CoopNativeTags::addr;

namespace {

std::mutex g_frame_mutex;
Frame g_frame;

constexpr float kHalfPi = 1.57079632679f;

// The copy's own panes (coop_map_icons.h): the icon types other than the player's arrow and the label are hidden.
constexpr const char* kIconHidden[] = {"N_Door_00",    "N_TreasureRoot_00", "N_Boss_00", "N_Facility_00",
                                       "N_Collect_00", "N_WaterLevel_00",   "N_Ruto_00"};
constexpr const char* kEntranceHidden[] = {"N_Mini_00", "N_Loacal_00", "T_Location_01", "T_Location_02"};
constexpr const char* kIconLayout = "PaMapIcon_00";
constexpr const char* kPlayerLayout = "PaMapIconPlayer_00";
constexpr const char* kGamePart = "Pa_MapIcon_00";   // the layout's own icon part (the game copies it)

// A message reference as the game's text setter reads it (as coop_native_tags.cpp's).
struct Ref {
    std::uint64_t text = 0;
    std::uint32_t bytes = 0;
    std::uint32_t pad = 0;
    std::int64_t end = -1;
};
static_assert(sizeof(Ref) == 24);

template <typename T>
bool ReadValue(CoopNativeTags::Game& game, std::uint64_t address, T& out) {
    return game.Read(address, &out, sizeof out);
}

std::string CopyName(std::size_t i) {
    char text[32];
    std::snprintf(text, sizeof text, kCopyName, static_cast<int>(i));
    return text;
}

// The probe's line pieces (printf: this file has no fmt, its test links none).
unsigned long long U(std::uint64_t v) { return static_cast<unsigned long long>(v); }
template <typename... A>
std::string Text(const char* format, A... args) {
    char text[256];
    std::snprintf(text, sizeof text, format, args...);
    return text;
}

// A part object keeps its layout inside it at +0x40 (the balloons' kPartRootPane(part + 0x40)).
constexpr std::uint64_t kPartLayoutAt = 0x40;

// The resource name of the layout at `at` (layout+0xf8 -> resource, +0x18 -> name, as LayoutsHeld reads it); "" when
// it is not one.
std::string LayoutName(CoopNativeTags::Game& game, std::uint64_t at) {
    std::uint64_t res = 0, name_at = 0;
    if (!ReadValue(game, at + 0xf8, res) || res < 0x80000000ull || !ReadValue(game, res + 0x18, name_at) ||
        name_at < 0x80000000ull) {
        return {};
    }
    char text[48] = {};
    if (!game.Read(name_at, text, sizeof text - 1)) return {};
    std::string name(text);
    bool readable = name.size() >= 4;
    for (const char c : name) readable = readable && c > 0x20 && c < 0x7f;
    return readable ? name : std::string{};
}

// The layout of the part object named `part_layout` held by `owner` (a part object: in its first 0x800 bytes or its
// layout's); 0: none.
std::uint64_t PartObjectLayout(CoopNativeTags::Game& game, std::uint64_t owner, const char* part_layout) {
    for (const std::uint64_t from : {owner, owner + kPartLayoutAt}) {
        std::vector<std::uint64_t> words(0x800 / 8);
        if (owner == 0 || !game.Read(from, words.data(), words.size() * 8)) continue;
        for (const std::uint64_t q : words) {
            if (q < 0x80000000ull || (q & 7) != 0) continue;
            if (LayoutName(game, q + kPartLayoutAt).rfind(part_layout, 0) == 0) return q + kPartLayoutAt;
        }
    }
    return 0;
}

// The pause tab's proven route to a part's own panes (coop_pause_tab_module.cpp FindOwnPane / PartLayout / PaneIn;
// pause tab run 2026-11-18, docs/game-ui.md): main+0x43c040(layout + 0x40) -> the layout's pane table; a part's own
// layout is a node of the holding table's parts list (sentinel table + 0x128, first [table + 0x130], next [node + 8],
// the part pane's name at [node + 0x148]); main+0x43e300(table, "name") -> the pane by name, a part's own pane too.
constexpr std::uint64_t kPaneTable = 0x43c040;    // (coop_pause_tab.h kPaneTable)
constexpr std::uint64_t kOwnPaneByName = 0x43e300;   // (coop_pause_tab.h kPaneByName)
constexpr std::uint64_t kPathText = 0xa67d30;        // (coop_pause_tab.h kPaneText, coop_title.cpp kPaneText)
constexpr std::size_t kPathAt = 160;                 // a copy's slot+: the label's path (the name before it)
static_assert(kPathAt + 64 <= Icons::kSlotBytes);

std::uint64_t PartIn(CoopNativeTags::Game& game, std::uint64_t table, const char* name) {
    if (table == 0) return 0;
    const std::uint64_t end = table + 0x128;
    std::uint64_t node = 0;
    if (!ReadValue(game, table + 0x130, node)) return 0;
    for (int guard = 0; node != 0 && node != end && guard < 256; ++guard) {
        std::uint64_t at = 0;
        char got[64] = {};
        if (ReadValue(game, node + 0x148, at) && at != 0 && game.Read(at, got, sizeof got - 1) &&
            std::strcmp(got, name) == 0) {
            return node;
        }
        if (!ReadValue(game, node + 8, node)) return 0;
    }
    return 0;
}

}  // namespace

void Publish(Frame frame) {
    std::lock_guard<std::mutex> lock(g_frame_mutex);
    g_frame = std::move(frame);
}

Frame Taken() {
    std::lock_guard<std::mutex> lock(g_frame_mutex);
    return g_frame;
}

std::optional<std::array<float, 6>> Inverse(const float m[12]) {
    const float det = m[0] * m[4] - m[3] * m[1];
    for (const int i : {0, 1, 3, 4, 9, 10}) {
        if (!std::isfinite(m[i])) return std::nullopt;
    }
    if (!(std::fabs(det) > 1e-9f)) return std::nullopt;
    const float a = m[4] / det, c = -m[3] / det, b = -m[1] / det, d = m[0] / det;
    return std::array<float, 6>{a, b, c, d, -(a * m[9] + c * m[10]), -(b * m[9] + d * m[10])};
}

std::array<float, 2> ToLocal(const std::array<float, 6>& v, float x, float y) {
    const float gx = x - addr::kHalfWidth, gy = addr::kHalfHeight - y;
    return {v[0] * gx + v[2] * gy + v[4], v[1] * gx + v[3] * gy + v[5]};
}

std::vector<Slot> Plan(const Frame& frame, const std::array<float, 6>& inverse) {
    std::vector<Slot> out;
    std::vector<const Mark*> used;
    // (the clip's own turn, as the minimap turns with the camera: the inverse turns the other way)
    const float clip_turn = std::atan2(inverse[1], inverse[0]);
    for (const auto& m : frame.marks) {
        if (out.size() >= kCopies) break;
        if (m.clamped || !std::isfinite(m.x) || !std::isfinite(m.y)) continue;
        Slot s;
        s.copy = out.size();
        s.at = ToLocal(inverse, m.x, m.y);
        if (m.angle && std::isfinite(*m.angle)) s.turn = -*m.angle - kHalfPi + clip_turn;
        s.arrow = m.floor_delta == 0;
        s.name = m.name.empty() ? "P" + std::to_string(m.number) : m.name;
        s.rgb = m.rgb & 0xFFFFFFu;
        out.push_back(std::move(s));
        used.push_back(&m);
    }
    // Overlapping arrows: only the name of the one nearer to us (no place of ours: the lower number) shows.
    auto far = [&](const Mark& m) {
        if (!frame.self) return static_cast<float>(m.number);
        return std::hypot(m.x - (*frame.self)[0], m.y - (*frame.self)[1]);
    };
    for (std::size_t i = 0; i < out.size(); ++i) {
        for (std::size_t j = i + 1; j < out.size(); ++j) {
            if (std::hypot(used[i]->x - used[j]->x, used[i]->y - used[j]->y) >= kOverlap) continue;
            const float fi = far(*used[i]), fj = far(*used[j]);
            const bool i_loses = fi > fj || (fi == fj && used[i]->number > used[j]->number);
            (i_loses ? out[i] : out[j]).name_shown = false;
        }
    }
    return out;
}

bool Icons::Block(CoopNativeTags::Game& game) {
    if (block_ != 0) return true;
    if (block_failed_) return false;
    names_.clear();
    names_.emplace_back(kClipPane);
    for (std::size_t i = 0; i < kCopies; ++i) names_.push_back(CopyName(i));
    for (const char* n : kIconHidden) names_.emplace_back(n);
    for (const char* n : kEntranceHidden) names_.emplace_back(n);
    for (const char* n : {"N_Player_00", "Pa_Player_00", "N_Entrance_00", "Pa_Entrance_00", "N_Location_00",
                          "T_Location_00", "N_Type_00", "N_Type_01", "N_PlayerRotate_00", kGamePart}) {
        names_.emplace_back(n);
    }
    const auto got = game.Call(game.MainBase() + addr::kMalloc, {kBlockBytes, 0, 0, 0, 0});
    if (names_.size() > kNames || !got || *got == 0) {
        block_failed_ = true;
        return false;
    }
    std::vector<std::uint8_t> bytes(kBlockBytes, 0);
    for (std::size_t i = 0; i < names_.size(); ++i) {
        std::memcpy(bytes.data() + i * kNameBytes, names_[i].c_str(), std::min(names_[i].size(), kNameBytes - 1));
    }
    if (!game.Write(*got, bytes.data(), bytes.size())) {
        block_failed_ = true;
        return false;
    }
    block_ = *got;
    return true;
}

std::uint64_t Icons::Name(const char* name) {
    for (std::size_t i = 0; i < names_.size(); ++i) {
        if (names_[i] == name) return block_ + i * kNameBytes;
    }
    return 0;
}

std::uint64_t Icons::Find(CoopNativeTags::Game& game, std::uint64_t layout, const char* name) {
    const std::uint64_t at = Name(name);
    if (layout == 0 || at == 0) return 0;
    const auto pane = game.Call(game.MainBase() + addr::kPaneByName, {layout, at, 0, 0, 0});
    return pane ? *pane : 0;
}

void Icons::Show(CoopNativeTags::Game& game, std::uint64_t pane, bool shown) {
    if (pane != 0) game.Call(game.MainBase() + addr::kPaneShow, {pane, shown ? 1u : 0u, 0, 0, 0});
}

// The copy's inner layouts and panes, once per screen; its other icon types hidden, the arrow and the label shown.
bool Icons::Ready(CoopNativeTags::Game& game, std::size_t i) {
    Copy& c = copies_[i];
    if (c.ready) return true;
    if (table_ == 0) {
        const auto t = game.Call(game.MainBase() + kPaneTable, {layout_ + 0x40, 0, 0, 0, 0});
        table_ = t ? *t : 0;
    }
    if (table_ == 0) return why_ = "no pane table of MapMenu_00", false;
    // (the copy's own pane: Find's f_8078e2b0 hands a part's inner root instead, and the copies are stored hidden on
    // their own pane, as the pause tab's)
    if (c.pane == 0) c.pane = Own(game, table_, CopyName(i).c_str());
    if (c.pane == 0) return why_ = "no " + CopyName(i), false;
    if (c.icon == 0) c.icon = PartIn(game, table_, CopyName(i).c_str());
    if (c.icon == 0) return why_ = CopyName(i) + ": no part layout", false;
    if (c.player == 0) c.player = PartIn(game, c.icon, "Pa_Player_00");
    if (c.entrance == 0) c.entrance = PartIn(game, c.icon, "Pa_Entrance_00");
    if (c.player == 0 || c.entrance == 0) return why_ = CopyName(i) + ": no arrow or label layout", false;
    c.turn = Own(game, c.player, "N_PlayerRotate_00");
    c.label = Own(game, c.entrance, "T_Location_00");
    if (c.label == 0) return why_ = CopyName(i) + ": no T_Location_00", false;
    for (const char* n : kIconHidden) Show(game, Own(game, c.icon, n), false);
    for (const char* n : kEntranceHidden) Show(game, Own(game, c.entrance, n), false);
    Show(game, Own(game, c.player, "N_Type_01"), false);   // N_Type_00: Link's look
    Show(game, Own(game, c.player, "N_Type_00"), true);
    Show(game, Own(game, c.entrance, "N_Location_00"), true);
    Show(game, c.label, true);
    c.arrow = Own(game, c.icon, "N_Player_00");
    Show(game, Own(game, c.icon, "N_Entrance_00"), true);
    Show(game, c.arrow, true);
    c.arrow_shown = true;
    c.ready = true;
    return true;
}

std::uint64_t Icons::Own(CoopNativeTags::Game& game, std::uint64_t table, const char* name) {
    const std::uint64_t at = Name(name);
    if (table == 0 || at == 0) return 0;
    const auto pane = game.Call(game.MainBase() + kOwnPaneByName, {table, at, 0, 0, 0});
    return pane ? *pane : 0;
}

bool Icons::Step(CoopNativeTags::Game& game, const Frame& frame) {
    const std::uint64_t screen = CoopNativeTags::ScreenByVtable(game, CoopNativeTags::kMapMenuVtable);
    // A new screen, or the same address holding another layout (a load whose new screen reuses the old one's memory):
    // its panes are new, the old ones may be freed. Forget them without a game call.
    std::uint64_t layout_now = 0;
    if (screen != 0 && !ReadValue(game, screen + addr::kScreenLayout, layout_now)) layout_now = 0;
    if (screen != screen_ || layout_now != layout_) {
        Forget();
        screen_ = screen;
        layout_ = layout_now;
    }
    if (screen == 0) return why_ = "no map screen", up_ = false;
    if (layout_ == 0 || !Block(game)) return why_ = layout_ == 0 ? "no map layout" : "no memory", up_ = false;
    if (clip_ == 0) clip_ = Find(game, layout_, kClipPane);
    float m[12] = {};
    const auto inverse = clip_ != 0 && game.Read(clip_ + kPaneMatrix, m, sizeof m) ? Inverse(m) : std::nullopt;
    if (!inverse) {   // (the clip not placed this frame: the copies are not left at old spots)
        HideAll(game);
        return why_ = clip_ == 0 ? "no S_MapIcon_00" : "S_MapIcon_00 not placed", up_ = up_ && clip_ != 0;
    }
    if (!Ready(game, 0)) return up_ = false;   // (copy 0 missing: no pack; the rest are made with it)
    up_ = true;
    std::vector<std::size_t> now;
    for (const Slot& s : Plan(frame, *inverse)) {
        if (!Ready(game, s.copy)) continue;
        Copy& c = copies_[s.copy];
        const std::string text = s.name_shown ? CoopNativeTags::Clean(s.name, kPathAt - sizeof(Ref) - 1) : "";
        if (c.named != text) {
            // The game's path setter on the map layout (the pause tab's and the title's kPaneText, main+0xa67d30:
            // (layout, "part/pane", &ref, 1, 0)), the path through the copy's label part; slot: {ref, text, path}.
            const std::uint64_t slot = block_ + kNames * kNameBytes + s.copy * kSlotBytes;
            const std::string path = CopyName(s.copy) + "/Pa_Entrance_00/T_Location_00";
            Ref ref;
            ref.text = slot + sizeof(Ref);
            ref.bytes = static_cast<std::uint32_t>(text.size());
            c.name_set = game.Write(ref.text, text.c_str(), text.size() + 1) && game.Write(slot, &ref, sizeof ref) &&
                         game.Write(slot + kPathAt, path.c_str(), path.size() + 1) &&
                         game.Call(game.MainBase() + kPathText, {layout_, slot + kPathAt, slot, 1, 0}).has_value();
            if (c.name_set) c.named = text;
        }
        if (!c.rgb_set || c.rgb != s.rgb) {   // (as Balloons::Colour: both colours of the text box)
            std::uint64_t inner = 0, box = 0;
            if (ReadValue(game, c.label + 8, inner) && inner != 0 && ReadValue(game, inner + 8, box) && box != 0) {
                const float rgba[4] = {((s.rgb >> 16) & 0xFF) / 255.0f, ((s.rgb >> 8) & 0xFF) / 255.0f,
                                       (s.rgb & 0xFF) / 255.0f, 1.0f};
                const std::uint64_t at = block_ + kNames * kNameBytes + kCopies * kSlotBytes + s.copy * 16;
                if (game.Write(at, rgba, sizeof rgba)) {
                    game.Call(game.MainBase() + addr::kTextColour0, {box, at, 0, 0, 0});
                    game.Call(game.MainBase() + addr::kTextColour1, {box, at, 0, 0, 0});
                    c.rgb = s.rgb, c.rgb_set = true;
                }
            }
        }
        if (c.arrow_shown != s.arrow) {   // (the arrow and the label panes found once in Ready, shown on a change)
            Show(game, c.arrow, s.arrow);
            c.arrow_shown = s.arrow;
        }
        c.turned.reset();
        if (turn_at_ != 0 && c.turn != 0 && s.turn) {
            const float turn = *s.turn;
            if (game.Write(c.turn + turn_at_, &turn, sizeof turn)) {
                CoopNativeTags::MarkPaneMoved(game, c.turn);
                c.turned = turn;
            }
        }
        if (CoopNativeTags::MovePane(game, c.pane, s.at)) {
            Show(game, c.pane, true);
            now.push_back(s.copy);
        }
    }
    for (const std::size_t old : shown_) {
        if (std::find(now.begin(), now.end(), old) == now.end()) Show(game, copies_[old].pane, false);
    }
    shown_ = std::move(now);
    why_.clear();
    return true;
}

std::uint64_t Icons::ArrowTurnPane(CoopNativeTags::Game& game, std::uint64_t table) {
    if (!Block(game)) return 0;
    return Own(game, PartIn(game, PartIn(game, table, kGamePart), "Pa_Player_00"), "N_PlayerRotate_00");
}

State Icons::Snapshot(CoopNativeTags::Game& game) {
    State st;
    st.up = up_;
    st.why = why_;
    st.turn_at = turn_at_;
    st.table = table_ != 0;
    for (std::size_t i = 0; i < kCopies; ++i) {
        const Copy& c = copies_[i];
        if (std::find(shown_.begin(), shown_.end(), i) == shown_.end()) continue;
        State::Shown one;
        one.copy = i;
        float xy[2] = {};
        if (game.Read(c.pane + addr::kPaneXY, xy, sizeof xy)) one.at = {xy[0], xy[1]};
        float t = 0;
        if (c.turn != 0 && turn_at_ != 0 && game.Read(c.turn + turn_at_, &t, sizeof t)) one.turn = t;
        one.wrote_turn = c.turned;
        one.name = c.named;
        one.name_set = c.name_set;
        st.shown.push_back(std::move(one));
    }
    // The game's own icon part (Pa_MapIcon_00, the one it shows Link with): its arrow's words +0x60..+0x7c, for the
    // turn's place (kPaneTurn) to be read against our own facing.
    if (table_ != 0) {
        const std::uint64_t turn = ArrowTurnPane(game, table_);
        st.game_arrow = turn != 0;
        if (turn != 0) game.Read(turn + 0x60, st.game_arrow_words.data(), sizeof(float) * st.game_arrow_words.size());
    }
    return st;
}

State Icons::ProbeArrow(CoopNativeTags::Game& game) {
    State st;
    st.probe = true;
    st.turn_at = turn_at_;
    const std::uint64_t screen = CoopNativeTags::ScreenByVtable(game, CoopNativeTags::kMapMenuVtable);
    std::uint64_t layout = 0;
    if (screen == 0 || !ReadValue(game, screen + addr::kScreenLayout, layout) || layout == 0 || !Block(game)) {
        st.why = screen == 0 ? "probe: no map screen" : "probe: no map layout";
        return st;
    }
    const auto t = game.Call(game.MainBase() + kPaneTable, {layout + 0x40, 0, 0, 0, 0});
    const std::uint64_t table = t ? *t : 0;
    st.table = table != 0;
    if (table != 0) {
        const std::uint64_t turn = ArrowTurnPane(game, table);
        st.game_arrow = turn != 0;
        if (turn != 0) game.Read(turn + 0x60, st.game_arrow_words.data(), sizeof(float) * st.game_arrow_words.size());
    }
    // The run-time copies, found as Probe finds them: part objects held in the screen's first 0x3000 bytes whose layout
    // is a PaMapIcon_00, their PaMapIconPlayer_00 layout, its N_PlayerRotate_00.
    std::vector<std::uint64_t> words(0x3000 / 8), seen;
    if (!game.Read(screen, words.data(), words.size() * 8)) return st;
    for (const std::uint64_t p : words) {
        if (st.run_arrow_words.size() >= 4) break;
        if (p < 0x80000000ull || (p & 7) != 0 || std::find(seen.begin(), seen.end(), p) != seen.end()) continue;
        seen.push_back(p);
        if (LayoutName(game, p + kPartLayoutAt).rfind(kIconLayout, 0) != 0) continue;
        const std::uint64_t turn = Find(game, PartObjectLayout(game, p, kPlayerLayout), "N_PlayerRotate_00");
        std::array<float, 8> w{};
        if (turn != 0 && game.Read(turn + 0x60, w.data(), sizeof(float) * w.size())) st.run_arrow_words.push_back(w);
    }
    return st;
}

std::string Json(const State& st) {
    auto num = [](double v) {
        char t[32];
        std::snprintf(t, sizeof t, "%.4f", std::isfinite(v) ? v : 0.0);
        return std::string(t);
    };
    auto quote = [](const std::string& v) {
        std::string o = "\"";
        for (const char ch : v) {
            if (ch == '"' || ch == '\\') o += '\\';
            if (static_cast<unsigned char>(ch) >= 0x20) o += ch;
        }
        return o + "\"";
    };
    std::string o = "{\"up\":" + std::string(st.up ? "true" : "false") + ",\"why\":" + quote(st.why) +
                    ",\"table\":" + (st.table ? "true" : "false") + ",\"turn_at\":" + std::to_string(st.turn_at) +
                    ",\"game_arrow\":" + (st.game_arrow ? "true" : "false") + ",\"game_arrow_words\":[";
    for (std::size_t i = 0; i < st.game_arrow_words.size(); ++i) o += (i ? "," : "") + num(st.game_arrow_words[i]);
    o += "],\"run_arrow_words\":[";
    for (std::size_t k = 0; k < st.run_arrow_words.size(); ++k) {
        o += k ? ",[" : "[";
        for (std::size_t i = 0; i < st.run_arrow_words[k].size(); ++i) o += (i ? "," : "") + num(st.run_arrow_words[k][i]);
        o += "]";
    }
    o += std::string("],\"probe\":") + (st.probe ? "true" : "false") + ",\"shown\":[";
    for (std::size_t i = 0; i < st.shown.size(); ++i) {
        const auto& s = st.shown[i];
        o += std::string(i ? "," : "") + "{\"copy\":" + std::to_string(s.copy) + ",\"x\":" + num(s.at[0]) +
             ",\"y\":" + num(s.at[1]) + ",\"turn\":" + (s.turn ? num(*s.turn) : "null") +
             ",\"wrote_turn\":" + (s.wrote_turn ? num(*s.wrote_turn) : "null") + ",\"name\":" + quote(s.name) +
             ",\"name_set\":" + (s.name_set ? "true" : "false") + "}";
    }
    return o + "]}";
}

namespace {
std::mutex g_state_mutex;
std::string g_state_json;
}  // namespace

void PublishState(std::string json) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_state_json = std::move(json);
}

std::string StateJson() {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    return g_state_json.empty() ? std::string("null") : g_state_json;
}

// The probe (LICHII_COOP_MAP_PROBE=1) on the game's own icons, the way Step finds the copies: the screen's layout, its
// S_MapIcon_00 and Pa_MapIcon_00 panes, the layouts that part pane holds (all names: which one is the icon's), and the
// icon layouts held one level under the screen (the game's run-time copies, one per icon shown: Link's arrow among
// them), each with its arrow N_PlayerRotate_00's words +0x60..+0x9c.
std::vector<std::string> Icons::Probe(CoopNativeTags::Game& game) {
    std::vector<std::string> out;
    const std::uint64_t screen = CoopNativeTags::ScreenByVtable(game, CoopNativeTags::kMapMenuVtable);
    std::uint64_t layout = 0;
    if (screen != 0) ReadValue(game, screen + addr::kScreenLayout, layout);
    if (screen == 0 || layout == 0 || !Block(game)) {
        out.push_back(Text("screen %#llx layout %#llx: nothing to read", U(screen), U(layout)));
        return out;
    }
    const std::uint64_t clip = Find(game, layout, kClipPane), part = Find(game, layout, kGamePart);
    out.push_back(Text("screen %#llx layout %#llx S_MapIcon_00 %#llx Pa_MapIcon_00 %#llx", U(screen), U(layout),
                       U(clip), U(part)));
    // Probe 2: a part pane (0xb0 bytes) holds no pointer to its part layout. The game keeps a part as its own object
    // with the layout inside it at +0x40 (the balloons: kPartRootPane(part + 0x40)); look for those under the screen
    // and its layout, every name once, and walk the icon ones down to the arrow.
    std::vector<std::uint64_t> seen;
    std::vector<std::string> names;
    int lines = 0;
    for (const auto& [from, bytes] : {std::pair{screen, std::size_t{0x3000}}, std::pair{layout, std::size_t{0x800}}}) {
        std::vector<std::uint64_t> words(bytes / 8);
        if (!game.Read(from, words.data(), bytes)) continue;
        for (std::size_t i = 0; i < words.size() && lines < 40; ++i) {
            const std::uint64_t p = words[i];
            if (p < 0x80000000ull || (p & 7) != 0 || std::find(seen.begin(), seen.end(), p) != seen.end()) continue;
            seen.push_back(p);
            const std::string name = LayoutName(game, p + kPartLayoutAt);
            if (name.empty() || name.rfind("MapMenu_00", 0) == 0) continue;
            const bool icon = name.rfind(kIconLayout, 0) == 0;
            if (!icon && std::find(names.begin(), names.end(), name) != names.end()) continue;
            names.push_back(name);
            const auto root = game.Call(game.MainBase() + addr::kPartRootPane, {p + kPartLayoutAt, 0, 0, 0, 0});
            const std::uint64_t r = root ? *root : 0;
            ++lines;
            out.push_back(Text("part %#llx (%s) at %s+%#llx root pane %#llx%s", U(p), name.c_str(),
                               from == screen ? "screen" : "layout", U(i * 8), U(r), r != 0 && r == part ? " = Pa_MapIcon_00" : ""));
            if (!icon) continue;
            const std::uint64_t player = PartObjectLayout(game, p, kPlayerLayout);
            const std::uint64_t turn = Find(game, player, "N_PlayerRotate_00");
            float w[16] = {};
            if (turn != 0) game.Read(turn + 0x60, w, sizeof w);
            std::string text;
            for (const float f : w) text += Text(" %.3f", static_cast<double>(f));
            out.push_back(Text("%#llx arrow layout %#llx turn pane %#llx +0x60:", U(p), U(player), U(turn)) + text);
        }
    }
    return out;
}

void Icons::Forget() {
    screen_ = layout_ = clip_ = table_ = 0;
    copies_ = {};
    shown_.clear();
}

void Icons::HideAll(CoopNativeTags::Game& game) {
    for (const std::size_t i : shown_) Show(game, copies_[i].pane, false);
    shown_.clear();
}

}  // namespace CoopMapIcons
