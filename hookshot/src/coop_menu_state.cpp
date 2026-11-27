// SPDX-License-Identifier: GPL-3.0-or-later
// coop_menu_state.h: co-op's menus as the play-test bridge's `menu` state member.
#include "coop_menu_state.h"

#include <cstddef>

#include "coop_playtest_core.h"

namespace CoopMenuState {
namespace {

using CoopPlaytest::Quote;

const char* KindWord(CoopPauseMenu::RowKind kind) {
    switch (kind) {
    case CoopPauseMenu::RowKind::kAction: return "action";
    case CoopPauseMenu::RowKind::kChoice: return "choice";
    case CoopPauseMenu::RowKind::kToggle: return "toggle";
    case CoopPauseMenu::RowKind::kHeading: return "heading";
    case CoopPauseMenu::RowKind::kNote: return "note";
    }
    return "other";
}

const char* Bool(bool b) { return b ? "true" : "false"; }

std::string TabJson(const Tab& tab) {
    if (!tab.open) return "{\"open\":false}";
    const auto& rows = tab.view.rows;
    std::string out = "{\"open\":true,\"tab\":\"multiplayer\",\"page\":" + Quote(tab.page) + ",\"rows\":[";
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        out += std::string(i ? "," : "") + "{\"name\":" + Quote(r.name) + ",\"kind\":\"" + KindWord(r.kind) + "\"";
        if (r.kind == CoopPauseMenu::RowKind::kToggle) out += std::string(",\"on\":") + Bool(r.on);
        if (r.fixed) out += ",\"fixed\":true";
        out += "}";
    }
    const int sel = tab.view.selected;
    const bool has = sel >= 0 && static_cast<std::size_t>(sel) < rows.size();
    out += "],\"focused\":" + (has ? std::to_string(sel) : std::string("null"));
    out += ",\"focused_name\":" + (has ? Quote(rows[static_cast<std::size_t>(sel)].name) : std::string("null"));
    out += std::string(",\"typing\":") + Bool(tab.view.typing) + "}";
    return out;
}

std::string ScreenJson(const Screen& s) {
    if (!s.open) return "{\"open\":false}";
    std::string out = std::string("{\"open\":true,\"driven\":") + Bool(s.driven);
    if (!s.driven) return out + "}";
    out += ",\"title\":" + Quote(s.texts.title) + ",\"options\":[";
    bool first = true;
    for (int f = 0; f < CoopSnailCustom::kOptions; ++f) {
        const auto i = static_cast<std::size_t>(f);
        if (s.texts.labels[i].empty()) continue;   // (past the page's last option: his pane shows a space)
        out += std::string(first ? "" : ",") + "{\"option\":" + std::to_string(f) + ",\"label\":" +
               Quote(s.texts.labels[i]) + ",\"value\":" + Quote(s.texts.values[i]) + "}";
        first = false;
    }
    const bool has = s.focus >= 0 && s.focus < CoopSnailCustom::kOptions &&
                     !s.texts.labels[static_cast<std::size_t>(s.focus)].empty();
    out += "],\"focused\":" + (has ? std::to_string(s.focus) : std::string("null"));
    out += ",\"focused_label\":" + (has ? Quote(s.texts.labels[static_cast<std::size_t>(s.focus)]) : std::string("null"));
    out += std::string(",\"mixer\":") + Bool(s.mixer) + ",\"page_no\":" + std::to_string(s.page_no) +
           ",\"pages\":" + std::to_string(s.pages);
    out += std::string(",\"keys\":{\"guide\":") + Bool(s.guide_keys) + ",\"page\":" + Bool(s.keys.page) +
           ",\"mix\":" + Bool(s.keys.mix) + "}}";
    return out;
}

}  // namespace

Screen FromDriver(const CoopSnailCustom::Driver& d, bool open, bool driven) {
    Screen s;
    s.open = open;
    s.driven = open && driven && d.Active();
    if (!s.driven) return s;
    s.texts = d.Shown();
    s.focus = d.Focus();
    s.mixer = d.Editing();
    s.page_no = d.PageNo();
    s.pages = d.Pages();
    s.guide_keys = d.KeysShown();
    s.keys = d.KeysNow();
    return s;
}

std::string Json(const Tab& tab, const Screen& screen) {
    return "{\"coop\":" + TabJson(tab) + ",\"customize_screen\":" + ScreenJson(screen) + "}";
}

std::string MinusJson(const Minus& m) {
    if (!m.open) return "{\"open\":false}";
    std::string out = "{\"open\":true,\"tab\":" + Quote(m.tab) + ",\"view\":" + Quote(m.view) +
                      ",\"details_page\":" + std::to_string(m.details_page) +
                      ",\"details_pages\":" + std::to_string(m.details_pages) + ",\"keys\":" + Bool(m.keys) +
                      ",\"typed_chars\":" + std::to_string(m.typed_chars) + ",\"row\":" + std::to_string(m.row) +
                      ",\"list\":" + Bool(m.list) + ",\"key_items\":";
    if (m.details_page != 1) return out + "null}";
    out += "[";
    for (std::size_t i = 0; i < m.key_items.size(); ++i) {
        out += std::string(i ? "," : "") + "{\"name\":" + Quote(m.key_items[i].name) + ",\"value\":" +
               Quote(m.key_items[i].value) + "}";
    }
    return out + "]}";
}

std::string Json(const Tab& tab, const Screen& screen, const Minus& minus) {
    return "{\"coop\":" + TabJson(tab) + ",\"customize_screen\":" + ScreenJson(screen) + ",\"minus\":" +
           MinusJson(minus) + "}";
}

std::string MapPageJson(const MapPage& map) {
    return std::string("{\"up\":") + Bool(map.up) + ",\"panes\":" + std::to_string(map.panes) +
           ",\"shown\":" + std::to_string(map.shown) + "}";
}

std::string Json(const Tab& tab, const Screen& screen, const Minus& minus, const MapPage& map) {
    std::string out = Json(tab, screen, minus);
    out.pop_back();   // (the closing brace: map_page goes in last)
    return out + ",\"map_page\":" + MapPageJson(map) + "}";
}

}  // namespace CoopMenuState
