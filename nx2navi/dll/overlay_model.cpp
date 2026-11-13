// SPDX-License-Identifier: GPL-3.0-or-later
#include "overlay_model.hpp"

#include <atomic>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <cstdio>

namespace CoopNx2navi::Ui {

std::string Compass(double dx, double dz) {
    if (!std::isfinite(dx) || !std::isfinite(dz) || std::hypot(dx, dz) < 0.5) {
        return {};
    }
    // Clockwise from north (-Z) towards east (+X).
    double degrees = std::atan2(dx, -dz) * 180.0 / 3.14159265358979323846;
    if (degrees < 0) {
        degrees += 360.0;
    }
    static constexpr const char* kNames[8] = {"north",  "north-east", "east",  "south-east",
                                              "south",  "south-west", "west",  "north-west"};
    static constexpr const char* kNoms[8] = {"nord", "nord-est", "est", "sud-est", "sud", "sud-ouest", "ouest", "nord-ouest"};
    return (French() ? kNoms : kNames)[static_cast<int>(std::floor(degrees / 45.0 + 0.5)) % 8];
}

namespace {
std::atomic<bool> g_french{false};
// The panel's own texts in the game's language (English, or French in a French game).
const char* T(const char* english, const char* french) { return g_french.load(std::memory_order_relaxed) ? french : english; }
}  // namespace

void SetFrench(bool on) { g_french.store(on, std::memory_order_relaxed); }
bool French() { return g_french.load(std::memory_order_relaxed); }

std::string LinkLine(const Snapshot& s) {
    switch (s.link) {
    case Link::connected:
        return std::string(T("Co-op: connected as P", "Coop : connect\u00e9 en tant que J")) + std::to_string(s.self) +
               " \"" + s.self_name + "\"";
    case Link::lost:
        return T("Co-op: server lost, retrying ", "Coop : serveur perdu, nouvel essai ") + s.server;
    case Link::refused:
        return T("Co-op: refused by the server (", "Coop : refus\u00e9 par le serveur (") + s.refused + ")";
    case Link::connecting:
    default:
        return T("Co-op: connecting to ", "Coop : connexion \u00e0 ") + s.server + "...";
    }
}

std::string LocalLine(const Local& local) {
    if (!local.game_found) {
        return T("You: game not found yet", "Vous : jeu pas encore trouv\u00e9");
    }
    if (!local.in_game) {
        return T("You: not in game (title screen, load or menu)", "Vous : hors du jeu (\u00e9cran titre, chargement ou menu)");
    }
    return T("You: ", "Vous : ") + (local.scene ? *local.scene : std::string(T("unknown scene", "lieu inconnu")));
}

std::string PeerLine(const Peer& peer, const Local& local) {
    std::string line = T("P", "J") + std::to_string(peer.number) + " " +
                       (peer.name.empty() ? std::string("?") : peer.name) + T(": ", " : ");
    if (!peer.known) {
        return line + T("joining", "arrive");
    }
    if (!peer.fresh) {
        return line + T("not in game", "hors du jeu");
    }
    if (!peer.scene || peer.scene->empty()) {
        return line + T("in game, scene unknown", "en jeu, lieu inconnu");
    }
    line += *peer.scene;
    const bool same = local.in_game && local.scene && *local.scene == *peer.scene;
    if (same && peer.position) {
        const double dx = static_cast<double>((*peer.position)[0] - local.position[0]);
        const double dy = static_cast<double>((*peer.position)[1] - local.position[1]);
        const double dz = static_cast<double>((*peer.position)[2] - local.position[2]);
        const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (std::isfinite(distance)) {
            char buffer[48];
            std::snprintf(buffer, sizeof(buffer), T(", %.1f units", ", %.1f m"), distance);
            line += buffer;
            const std::string way = Compass(dx, dz);
            line += way.empty() ? std::string(T(", right here", ", juste ici")) : ", " + way;
        }
    }
    return line;
}

float NoticeAlpha(const Notice& notice, std::uint64_t now_ms) {
    const std::uint64_t age = now_ms >= notice.at_ms ? now_ms - notice.at_ms : 0;
    if (age >= kNoticeMs) {
        return 0.0f;
    }
    const std::uint64_t fade_from = kNoticeMs - kNoticeFadeMs;
    if (age <= fade_from) {
        return 1.0f;
    }
    return static_cast<float>(kNoticeMs - age) / static_cast<float>(kNoticeFadeMs);
}

void AddNotice(std::deque<Notice>& notices, std::string text, std::uint64_t now_ms) {
    notices.push_back({std::move(text), now_ms});
    while (notices.size() > kNoticesKept) {
        notices.pop_front();
    }
}

void PruneNotices(std::deque<Notice>& notices, std::uint64_t now_ms) {
    while (!notices.empty() && NoticeAlpha(notices.front(), now_ms) <= 0.0f) {
        notices.pop_front();
    }
}

void AddHistory(std::deque<std::string>& history, std::string line) {
    history.push_back(std::move(line));
    while (history.size() > kHistoryKept) {
        history.pop_front();
    }
}

std::size_t Utf8Count(std::string_view text) {
    std::size_t count = 0;
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) & 0xc0) != 0x80) {
            ++count;
        }
    }
    return count;
}

bool Utf8Append(std::string& text, std::uint32_t cp, std::size_t max_codepoints) {
    if (cp < 0x20 || (cp >= 0x7f && cp < 0xa0) || (cp >= 0xd800 && cp <= 0xdfff) || cp > 0x10ffff) {
        return false;
    }
    if (Utf8Count(text) >= max_codepoints) {
        return false;
    }
    if (cp < 0x80) {
        text.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        text.push_back(static_cast<char>(0xc0 | (cp >> 6)));
        text.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else if (cp < 0x10000) {
        text.push_back(static_cast<char>(0xe0 | (cp >> 12)));
        text.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        text.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else {
        text.push_back(static_cast<char>(0xf0 | (cp >> 18)));
        text.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
        text.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        text.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
    return true;
}

void Utf8PopBack(std::string& text) {
    while (!text.empty()) {
        const auto c = static_cast<unsigned char>(text.back());
        text.pop_back();
        if ((c & 0xc0) != 0x80) {
            return;
        }
    }
}

std::vector<Line> Panel(const Snapshot& s, const std::deque<Notice>& notices, const ChatView& chat,
                        std::uint64_t now_ms) {
    std::vector<Line> lines;
    lines.push_back({LinkLine(s), Role::title});
    lines.push_back({LocalLine(s.local), Role::local});
    for (const Peer& peer : s.peers) {
        lines.push_back({PeerLine(peer, s.local), Role::peer, 1.0f, PeerColour(s.self, peer.number)});
    }
    // A chat line or notice that starts with a player's name gets that player's colour.
    const auto colour_of = [&s](const std::string& text) {
        for (const Peer& peer : s.peers) {
            if (!peer.name.empty() && text.size() > peer.name.size() + 1 &&
                text.compare(0, peer.name.size(), peer.name) == 0 &&
                (text[peer.name.size()] == ':' || text[peer.name.size()] == ' ')) {
                return PeerColour(s.self, peer.number);
            }
        }
        return -1;
    };
    if (s.link == Link::connected && s.peers.empty()) {
        lines.push_back({T("Nobody else is here yet", "Personne d'autre pour l'instant"), Role::peer});
    }
    if (chat.open) {
        for (const std::string& line : chat.history) {
            lines.push_back({line, Role::chat, 1.0f, colour_of(line)});
        }
        lines.push_back({"> " + chat.input + "_", Role::input});
        lines.push_back({T("Enter sends, Esc cancels", "Entr\u00e9e : envoyer, \u00c9chap : annuler"), Role::hint});
        return lines;
    }
    for (const Notice& notice : notices) {
        const float alpha = NoticeAlpha(notice, now_ms);
        if (alpha > 0.0f) {
            lines.push_back({notice.text, Role::notice, alpha, colour_of(notice.text)});
        }
    }
    lines.push_back({T("Enter: chat   F9: hide", "Entr\u00e9e : discussion   F9 : masquer"), Role::hint});
    return lines;
}

int PeerColour(std::uint8_t self, std::uint8_t number) {
    if (number < 1 || number > 4 || number == self) {
        return -1;
    }
    int index = 0;
    for (std::uint8_t n = 1; n < number; ++n) {
        index += n != self ? 1 : 0;
    }
    return index < 3 ? index : 2;
}

std::optional<std::array<float, 2>> MapPoint(const MapView& view, float world_x, float world_z,
                                             float window_w, float window_h) {
    for (const float v : {view.centre_x, view.centre_z, view.world_span, view.pane_x, view.pane_y,
                          view.pane_w, view.pane_h, world_x, world_z}) {
        if (!std::isfinite(v)) {
            return std::nullopt;
        }
    }
    if (view.world_span < 1.0f || view.pane_w < 1.0f || view.pane_h < 1.0f || window_w < 1.0f ||
        window_h < 1.0f) {
        return std::nullopt;
    }
    const float u = (world_x - view.centre_x) / view.world_span;  // -0.5 to 0.5 across the picture
    const float v = (world_z - view.centre_z) / view.world_span;
    if (u < -0.5f || u > 0.5f || v < -0.5f || v > 0.5f) {
        return std::nullopt;
    }
    const float canvas_x = view.pane_x + u * view.pane_w;
    const float canvas_y = view.pane_y - v * view.pane_h;
    return std::array<float, 2>{(canvas_x / kCanvasW + 0.5f) * window_w,
                                (0.5f - canvas_y / kCanvasH) * window_h};
}

std::optional<TagBox> NameTagBox(float point_x, float point_y, float text_w, float text_h, float pad,
                                 float picture_x, float picture_y, float picture_w, float picture_h) {
    const float all[] = {point_x, point_y, text_w, text_h, pad, picture_x, picture_y, picture_w, picture_h};
    for (const float v : all) {
        if (!std::isfinite(v)) {
            return std::nullopt;
        }
    }
    if (text_w <= 0.0f || text_h <= 0.0f || pad < 0.0f || picture_w <= 0.0f || picture_h <= 0.0f) {
        return std::nullopt;
    }
    if (point_x < picture_x || point_x > picture_x + picture_w || point_y < picture_y ||
        point_y > picture_y + picture_h) {
        return std::nullopt;
    }
    const float w = text_w + 2 * pad;
    const float h = text_h + 2 * pad;
    if (w > picture_w || h > picture_h) {
        return std::nullopt;
    }
    TagBox box;
    box.x0 = std::clamp(point_x - w / 2, picture_x, picture_x + picture_w - w);
    box.y0 = std::clamp(point_y - h, picture_y, picture_y + picture_h - h);
    box.x1 = box.x0 + w;
    box.y1 = box.y0 + h;
    box.x = box.x0 + pad;
    box.y = box.y0 + pad;
    return box;
}

float TagAlpha(float distance) {
    if (!std::isfinite(distance) || distance < 0.0f) {
        return 0.0f;
    }
    if (distance <= kTagNear) {
        return 1.0f;
    }
    if (distance >= kTagFar) {
        return kTagFarAlpha;
    }
    return 1.0f - (1.0f - kTagFarAlpha) * (distance - kTagNear) / (kTagFar - kTagNear);
}

}  // namespace CoopNx2navi::Ui
