// SPDX-License-Identifier: GPL-3.0-or-later
// coop_lobby_tab.h, the module's part: the one Tab, the session it shows
// (CoopLobby::Now, CoopHook::PlayerList, the voices, the hearts) and what its
// requests do. Only in hookshot_coop.dll (the tests drive Tab itself).
#include "coop_lobby_tab.h"

#include <chrono>
#include <mutex>
#include <utility>

#include "common/logging.h"
#include "coop_colors.h"
#include "coop_hook.h"
#include "coop_hookshot.h"
#include "coop_hud.h"
#include "coop_lobby_game.h"
#include "coop_objects.h"
#include "coop_pause_files.h"
#include "coop_voice.h"
#include "hos_coop_host.h"

namespace CoopLobbyTab {

namespace {

namespace proto = coop::protocol;

std::mutex g_mutex;   // everything below
Tab g_tab;
int g_max_players = 0;            // the host's coop-options.ini max_players, read when the tab shows
std::uint64_t g_refreshed_ms = 0;

std::uint64_t NowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

std::string ColourWords(std::uint32_t rgb) {
    const std::string_view tunic = CoopColors::TunicOf(rgb);
    if (tunic.empty()) {
        return "green";
    }
    std::string s(tunic);
    for (char& ch : s) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return s;
}

// The session as the tab shows it. Takes the network lock (PlayerList), so never under g_mutex.
Lobby Read(int max_players) {
    const CoopLobby::Seen seen = CoopLobby::Now();
    Lobby l;
    l.connected = seen.connected;
    if (!seen.connected) {
        return l;
    }
    l.hosting = seen.host != 0 && seen.host == seen.self;
    l.max_players = l.hosting ? max_players : 0;
    if (l.hosting) {
        HosCoopStatus st{};
        st.size = sizeof st;
        CoopHookshot::HostMenu().Status(&st);
        if (st.state == HOS_COOP_HOSTING) {
            l.invite = st.invite;
        }
    }
    const auto rows = CoopHook::PlayerList();
    for (const auto& s : seen.players) {
        Player p;
        p.number = s.number;
        p.name = s.name;
        p.self = s.self;
        p.host = s.host;
        p.colour = ColourWords(s.color);
        p.host_mutes = s.host_mutes;
        p.own_mutes = s.own_mutes;
        p.talking = s.self ? CoopVoice::Talking() : CoopVoice::Speaking(s.number);
        if (s.dbno == proto::DbnoState::downed) {
            p.life = Player::Life::downed;
        } else if (s.dbno == proto::DbnoState::out) {
            p.life = Player::Life::out;
        } else if ((s.status & proto::status::dead) != 0) {
            p.life = Player::Life::dead;
        }
        for (const auto& r : rows) {
            if (r.number == s.number) {
                p.ms = r.ms;
                if (r.scene && !r.scene->empty()) {
                    p.place = CoopHud::PlaceName(*r.scene);
                }
            }
        }
        if (s.host) {
            l.host_place = p.place;
            if (s.self) {
                if (const auto ours = CoopObjects::OurLife()) {
                    l.host_life = static_cast<std::uint16_t>(ours->life);
                    l.host_max = static_cast<std::uint16_t>(ours->max);
                }
            } else if (const auto theirs = CoopHud::PlayerLife(s.number)) {
                l.host_life = theirs->life;
                l.host_max = theirs->max;
            }
        }
        l.players.push_back(std::move(p));
    }
    return l;
}

void Refresh(bool now) {
    int max_players = 0;
    {
        std::lock_guard lock(g_mutex);
        const std::uint64_t ms = NowMs();
        if (!now && ms - g_refreshed_ms < 250) {
            return;
        }
        g_refreshed_ms = ms;
        max_players = g_max_players;
    }
    Lobby lobby = Read(max_players);
    std::lock_guard lock(g_mutex);
    g_tab.Set(std::move(lobby));
}

// Carries out the tab's request (never under g_mutex: the teleport takes the network lock).
void Carry(std::optional<Request> r) {
    if (!r) {
        return;
    }
    switch (r->kind) {
    case Request::Kind::kServer:
        LOG_INFO(Core, "coop: lobby tab: host action {} on player {}", static_cast<int>(r->action.action),
                 r->action.target);
        CoopLobby::Ask(r->action);
        break;
    case Request::Kind::kOwnMute: CoopLobby::SetOwnMute(r->player, r->what, r->on); break;
    case Request::Kind::kTeleport: CoopHook::SendChat("/teleport " + r->name); break;
    case Request::Kind::kCopyInvite: {
        HosCoopStatus st{};
        st.size = sizeof st;
        CoopHookshot::HostMenu().Status(&st);
        if (st.invite[0] != '\0') {
            CoopFiles::SetClipboardText(st.invite);
        }
        break;
    }
    }
    Refresh(true);
}

}  // namespace

const char* TabName() {
    return "Lobby";
}

bool Page(HosCoopMenuPage* out) {
    if (out == nullptr || out->size < sizeof(HosCoopMenuPage)) {
        return false;
    }
    Refresh(false);
    thread_local CoopPauseMenu::View view;
    thread_local std::vector<HosCoopMenuRow> rows;
    {
        std::lock_guard lock(g_mutex);
        view = g_tab.view();
    }
    rows.resize(view.rows.size());
    for (std::size_t i = 0; i < view.rows.size(); ++i) {
        const CoopPauseMenu::Row& r = view.rows[i];
        HosCoopMenuRow& h = rows[i];
        h = HosCoopMenuRow{};
        h.name = r.name.c_str();
        h.value = r.value.c_str();
        h.kind = static_cast<std::uint8_t>(r.kind);
        h.on = r.on ? 1 : 0;
        h.left = r.left ? 1 : 0;
        h.right = r.right ? 1 : 0;
        h.fixed = r.fixed ? 1 : 0;
    }
    out->rows_n = static_cast<std::int32_t>(rows.size());
    out->rows = rows.data();
    out->selected = view.selected;
    out->typing = view.typing ? 1 : 0;
    out->help_title = view.help_title.c_str();
    out->help = view.help.c_str();
    out->hint = view.hint.c_str();
    out->gold = view.gold.c_str();
    out->change = view.change.c_str();
    out->confirm = view.confirm.c_str();
    out->back = view.back.c_str();
    return true;
}

bool MenuInput(int event, int arg, int arg2) {
    if (event == HOS_COOP_MENU_SHOW) {
        const int max_players = CoopFiles::ReadOptions().max_players;   // the menu tick: files are fine here
        {
            std::lock_guard lock(g_mutex);
            g_max_players = max_players;
            g_tab.Show();
        }
        Refresh(true);
        return true;
    }
    Refresh(false);
    bool taken = true;
    std::optional<Request> request;
    {
        std::lock_guard lock(g_mutex);
        switch (event) {
        case HOS_COOP_MENU_UP: g_tab.Press(Input::kUp); break;
        case HOS_COOP_MENU_DOWN: g_tab.Press(Input::kDown); break;
        case HOS_COOP_MENU_LEFT: g_tab.Press(Input::kLeft); break;
        case HOS_COOP_MENU_RIGHT: g_tab.Press(Input::kRight); break;
        case HOS_COOP_MENU_CONFIRM: g_tab.Press(Input::kDecide); break;
        case HOS_COOP_MENU_BACK: taken = g_tab.Press(Input::kBack); break;
        case HOS_COOP_MENU_HIDE: g_tab.Hide(); break;
        case HOS_COOP_MENU_SELECT:
            if (arg >= 0) {
                g_tab.Select(static_cast<std::size_t>(arg));
            }
            break;
        case HOS_COOP_MENU_CLICK:
            if (arg >= 0) {
                g_tab.Click(static_cast<std::size_t>(arg), arg2 < 0 ? -1 : arg2 > 0 ? 1 : 0);
            }
            break;
        default: return false;
        }
        request = g_tab.TakeRequest();
    }
    Carry(std::move(request));
    return taken;
}

}  // namespace CoopLobbyTab
