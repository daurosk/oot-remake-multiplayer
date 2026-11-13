// coop_mods_tab.h, the module's part: the one Tab, handed to Hookshot as tab 2 (hos_coop_menu.h) through
// coop_pause.cpp's exports. Only in hookshot_coop.dll (the tests drive Tab itself).
#include "coop_mods_tab.h"

#include <memory>
#include <mutex>

#include "coop_mod_share.h"
#include "coop_mods_runtime.h"

namespace CoopModsTab {

namespace {

std::mutex g_mutex;
std::unique_ptr<Tab> g_tab;

Tab* TheTab() {  // under g_mutex
    if (!g_tab && CoopModsRuntime::Loader() != nullptr) {
        Tab::Sources s;
        s.loader = CoopModsRuntime::Loader();
        s.coop = [] {
            const CoopModShare::View v = CoopModShare::Now();
            std::vector<std::string> out;
            if (!v.connected) return out;
            if (!v.hosting && !v.host_list_known) out.push_back("Co-op: waiting for the host's list of mods.");
            for (const std::string& d : v.differences) out.push_back("Co-op: " + d);
            for (const std::string& g : v.guests_differ) out.push_back("Co-op: " + g);
            if (!v.hosting && v.host_list_known && v.differences.empty()) {
                out.push_back("Co-op: your mods match the host's.");
            }
            return out;
        };
        s.rando = [] {
            CoopRando::Rando* r = CoopModsRuntime::Rando();
            return r != nullptr ? r->Status() : std::string();
        };
        g_tab = std::make_unique<Tab>(std::move(s));
    }
    return g_tab.get();
}

struct Drawn {
    CoopPauseMenu::View view;
    std::vector<HosCoopMenuRow> rows;
};

}  // namespace

bool Available() {
    return CoopModsRuntime::Loader() != nullptr;
}

const char* TabName() {
    return "Mods";
}

bool Page(HosCoopMenuPage* out) {
    if (out == nullptr || out->size < sizeof(HosCoopMenuPage)) return false;
    thread_local Drawn drawn;
    {
        std::lock_guard lock(g_mutex);
        Tab* tab = TheTab();
        if (tab == nullptr) return false;
        drawn.view = tab->view();
    }
    const CoopPauseMenu::View& v = drawn.view;
    drawn.rows.resize(v.rows.size());
    for (std::size_t i = 0; i < v.rows.size(); ++i) {
        const CoopPauseMenu::Row& r = v.rows[i];
        HosCoopMenuRow& h = drawn.rows[i];
        h = HosCoopMenuRow{};
        h.name = r.name.c_str();
        h.value = r.value.c_str();
        h.kind = static_cast<std::uint8_t>(r.kind);
        h.on = r.on ? 1 : 0;
        h.left = r.left ? 1 : 0;
        h.right = r.right ? 1 : 0;
        h.fixed = r.fixed ? 1 : 0;
    }
    out->rows_n = static_cast<std::int32_t>(drawn.rows.size());
    out->rows = drawn.rows.data();
    out->selected = v.selected;
    out->typing = v.typing ? 1 : 0;
    out->help_title = v.help_title.c_str();
    out->help = v.help.c_str();
    out->hint = v.hint.c_str();
    out->gold = v.gold.c_str();
    out->change = v.change.c_str();
    out->confirm = v.confirm.c_str();
    out->back = v.back.c_str();
    return true;
}

bool MenuInput(int event, int arg, int arg2) {
    std::lock_guard lock(g_mutex);
    Tab* tab = TheTab();
    if (tab == nullptr) return false;
    switch (event) {
    case HOS_COOP_MENU_UP: return tab->Press(Input::kUp);
    case HOS_COOP_MENU_DOWN: return tab->Press(Input::kDown);
    case HOS_COOP_MENU_LEFT: return tab->Press(Input::kLeft);
    case HOS_COOP_MENU_RIGHT: return tab->Press(Input::kRight);
    case HOS_COOP_MENU_CONFIRM: return tab->Press(Input::kDecide);
    case HOS_COOP_MENU_BACK: return tab->Press(Input::kBack);
    case HOS_COOP_MENU_SHOW: tab->Show(); return true;
    case HOS_COOP_MENU_HIDE: tab->Hide(); return true;
    case HOS_COOP_MENU_SELECT:
        if (arg >= 0) tab->Select(static_cast<std::size_t>(arg));
        return true;
    case HOS_COOP_MENU_CLICK:
        if (arg >= 0) tab->Click(static_cast<std::size_t>(arg), arg2 < 0 ? -1 : arg2 > 0 ? 1 : 0);
        return true;
    default: return false;
    }
}

bool Key(std::uint32_t vk, bool repeat) {
    (void)repeat;
    std::lock_guard lock(g_mutex);
    Tab* tab = g_tab.get();
    if (tab == nullptr || !tab->typing()) return false;
    if (vk == 0x70 || vk == 0x2D) return false;  // F1, Insert: Hookshot's (the menu closes)
    switch (vk) {
    case 0x0D: tab->Press(Input::kDecide); break;
    case 0x1B: tab->Press(Input::kBack); break;
    case 0x08: tab->Press(Input::kErase); break;
    default: break;
    }
    return true;  // every other key while typing: its character comes as Char
}

bool Char(std::uint32_t cp) {
    std::lock_guard lock(g_mutex);
    return g_tab != nullptr && g_tab->Char(cp);
}

bool KeysTaken() {
    std::lock_guard lock(g_mutex);
    return g_tab != nullptr && g_tab->typing();
}

}  // namespace CoopModsTab
