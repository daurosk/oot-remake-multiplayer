// SPDX-License-Identifier: GPL-3.0-or-later
//
// coop_mod_share.h: the network thread keeps what came; the present thread
// sends, compares and hands the host's values to the loader.
#include "coop_mod_share.h"

#include <map>
#include <mutex>
#include <utility>
#include <variant>

#include "coop/host/session.hpp"
#include "coop/net/client.hpp"

namespace CoopModShare {

namespace {

namespace proto = coop::protocol;

std::string Short(const proto::ModEntry& e) {
    return e.version.empty() ? e.id : e.id + " " + e.version;
}

struct Glue {
    std::mutex mutex;
    Provider provider;
    bool connected = false;
    bool hosting = false;
    std::uint8_t host_number = 0;
    std::map<std::uint8_t, proto::ModList> lists;  // the others', by number
    std::map<std::uint8_t, std::string> names;
    bool list_sent = false;  // on this connection
    std::optional<std::vector<proto::ModSetting>> shared_sent;
    bool host_values_due = false;  // the present thread hands host_values on
    std::optional<std::vector<proto::ModSetting>> host_values;
    std::vector<std::string> differences;     // as last told
    std::map<std::uint8_t, std::size_t> guests_told;  // host: differences last told per guest
    std::vector<std::string> lines;
    void Line(std::string text) {
        if (lines.size() < 32) {
            lines.push_back(std::move(text));
        }
    }
    void Reset() {
        connected = false;
        hosting = false;
        host_number = 0;
        lists.clear();
        names.clear();
        list_sent = false;
        shared_sent.reset();
        host_values.reset();
        host_values_due = true;
        differences.clear();
        guests_told.clear();
    }
};

Glue& TheGlue() {
    static Glue glue;
    return glue;
}

}  // namespace

std::vector<std::string> Compare(const proto::ModList& host, const proto::ModList& mine, std::size_t max) {
    std::vector<std::string> out;
    std::size_t more = 0;
    const auto add = [&](std::string line) {
        if (out.size() < max) {
            out.push_back(std::move(line));
        } else {
            ++more;
        }
    };
    const auto find = [](const proto::ModList& list, const std::string& id) -> const proto::ModEntry* {
        for (const auto& e : list.mods) {
            if (e.id == id) return &e;
        }
        return nullptr;
    };
    for (const auto& h : host.mods) {
        const proto::ModEntry* m = find(mine, h.id);
        if (m == nullptr) {
            add("missing " + Short(h) + " (the host has it)");
        } else if (m->version != h.version) {
            add(h.id + ": you have " + (m->version.empty() ? "no version" : m->version) + ", the host " +
                (h.version.empty() ? "no version" : h.version));
        } else if (m->hash != h.hash && m->hash != proto::ModHash{} && h.hash != proto::ModHash{}) {
            add(Short(h) + ": your files differ from the host's");
        }
    }
    for (const auto& m : mine.mods) {
        if (find(host, m.id) == nullptr) {
            add("extra " + Short(m) + " (the host does not have it)");
        }
    }
    if (more > 0) {
        out.push_back("and " + std::to_string(more) + " more");
    }
    return out;
}

void SetProvider(Provider provider) {
    Glue& g = TheGlue();
    std::lock_guard lock(g.mutex);
    g.provider = std::move(provider);
}

void OnEvent(const coop::net::Event& event, coop::net::Client& client) {
    using coop::net::EventKind;
    Glue& g = TheGlue();
    std::lock_guard lock(g.mutex);
    switch (event.kind) {
    case EventKind::welcomed:
    case EventKind::reconnected:
    case EventKind::disconnected:
        g.Reset();  // each connection starts with nothing; the server gives the rest
        g.connected = event.kind != EventKind::disconnected;
        g.hosting = g.connected && client.is_host();
        g.host_number = client.host_number();
        for (const auto& p : client.players()) {
            g.names[p.number] = p.nickname;
        }
        return;
    case EventKind::host_changed:
        g.hosting = client.is_host();
        g.host_number = client.host_number();
        g.shared_sent.reset();  // a new host sends its own values
        if (g.hosting) {
            g.host_values.reset();
            g.host_values_due = true;
        }
        g.differences.clear();
        g.guests_told.clear();
        return;
    case EventKind::player_joined:
        g.names[event.player] = event.nickname;
        return;
    case EventKind::player_left:
        g.lists.erase(event.player);
        g.names.erase(event.player);
        g.guests_told.erase(event.player);
        return;
    case EventKind::message:
        break;
    default:
        return;
    }
    if (const auto* list = std::get_if<proto::ModList>(&event.message)) {
        g.lists[list->player_number] = *list;
    } else if (const auto* settings = std::get_if<proto::ModSettings>(&event.message)) {
        if (!g.hosting) {
            g.host_values = settings->settings;
            g.host_values_due = true;
        }
    }
}

void Frame(coop::host::Session* session) {
    Glue& g = TheGlue();
    std::optional<proto::Message> send_list;
    std::optional<proto::Message> send_settings;
    std::optional<std::optional<std::vector<proto::ModSetting>>> hand_on;
    std::function<void(std::optional<std::vector<proto::ModSetting>>)> host_values;
    {
        std::lock_guard lock(g.mutex);
        if (g.host_values_due && g.provider.host_values) {
            g.host_values_due = false;
            hand_on = g.host_values;
            host_values = g.provider.host_values;
        }
        if (session != nullptr && g.connected) {
            const std::optional<proto::ModList> mine = g.provider.mine ? g.provider.mine() : std::nullopt;
            if (mine && !g.list_sent) {
                g.list_sent = true;
                send_list = *mine;
            }
            if (g.hosting && g.provider.shared) {
                std::vector<proto::ModSetting> shared = g.provider.shared();
                if (shared.size() > proto::kMaxModSettingEntries) {
                    shared.resize(proto::kMaxModSettingEntries);
                }
                if (g.shared_sent != shared) {
                    g.shared_sent = shared;
                    send_settings = proto::ModSettings{proto::kPlayerSelf, std::move(shared)};
                }
            }
            if (mine && !g.hosting) {
                const auto host = g.lists.find(g.host_number);
                if (host != g.lists.end()) {
                    std::vector<std::string> diff = Compare(host->second, *mine);
                    if (diff != g.differences) {
                        if (!diff.empty()) {
                            g.Line("Mods: yours differ from the host's (" + diff.front() +
                                   (diff.size() > 1 ? ", ..." : "") + "): see the Mods tab");
                        } else if (!g.differences.empty()) {
                            g.Line("Mods: yours now match the host's");
                        }
                        g.differences = std::move(diff);
                    }
                }
            }
            if (mine && g.hosting) {
                for (const auto& [number, list] : g.lists) {
                    const std::size_t n = Compare(*mine, list, 1000).size();
                    const auto told = g.guests_told.find(number);
                    if (told == g.guests_told.end() || told->second != n) {
                        g.guests_told[number] = n;
                        if (n > 0) {
                            const auto name = g.names.find(number);
                            g.Line("Mods: player " + std::to_string(number) +
                                   (name != g.names.end() ? " (" + name->second + ")" : "") + " has " +
                                   std::to_string(n) + (n == 1 ? " mod" : " mods") + " unlike yours");
                        }
                    }
                }
            }
        }
    }
    if (hand_on && host_values) {
        host_values(*hand_on);
    }
    for (auto* m : {&send_list, &send_settings}) {
        if (!*m) {
            continue;
        }
        const bool sent = session->with_client([&](coop::net::Client& client) {
            return client.state() == coop::net::State::connected && client.send(**m);
        });
        if (!sent) {
            std::lock_guard lock(g.mutex);
            if (m == &send_list) {
                g.list_sent = false;  // the next frame tries again
            } else {
                g.shared_sent.reset();
            }
        }
    }
}

std::vector<std::string> TakeLines() {
    Glue& g = TheGlue();
    std::lock_guard lock(g.mutex);
    return std::exchange(g.lines, {});
}

View Now() {
    Glue& g = TheGlue();
    std::lock_guard lock(g.mutex);
    View v;
    v.connected = g.connected;
    v.hosting = g.hosting;
    v.host_list_known = g.hosting || g.lists.count(g.host_number) != 0;
    v.differences = g.differences;
    for (const auto& [number, n] : g.guests_told) {
        if (n > 0) {
            const auto name = g.names.find(number);
            v.guests_differ.push_back("Player " + std::to_string(number) +
                                      (name != g.names.end() ? " (" + name->second + ")" : "") + ": " +
                                      std::to_string(n) + (n == 1 ? " mod differs" : " mods differ"));
        }
    }
    return v;
}

}  // namespace CoopModShare
