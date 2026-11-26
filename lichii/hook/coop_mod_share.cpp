// SPDX-License-Identifier: GPL-3.0-or-later
//
// coop_mod_share.h: the network thread keeps what came; the present thread
// sends, compares and hands the host's values to the loader.
#include "coop_mod_share.h"

#include "coop_gold_mode.h"
#include "coop_race_mode.h"
#include "coop_switch_stand_mode.h"

#include <array>
#include <map>
#include <mutex>
#include <utility>
#include <variant>

#include "coop/host/session.hpp"
#include "coop/net/client.hpp"
#include "coop/protocol/race.hpp"

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
    std::map<std::uint8_t, int> seedless;  // a player's seed against the host's, as last told: 0 same, 1 none, 2 other files (#59)
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
        seedless.clear();
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
    int kit_other = 0;  // > 0: the host's kit is older (its hashes made an older way), < 0: ours is
    for (const auto& h : host.mods) {
        const proto::ModEntry* m = find(mine, h.id);
        if (m == nullptr) {
            add("missing " + Short(h) + " (the host has it)");
        } else if (m->version != h.version) {
            add(h.id + ": you have " + (m->version.empty() ? "no version" : m->version) + ", the host " +
                (h.version.empty() ? "no version" : h.version));
        } else if (m->hash != h.hash && m->hash != proto::ModHash{} && h.hash != proto::ModHash{}) {
            if (HashWay(m->hash) == HashWay(h.hash)) {
                add(Short(h) + ": your files differ from the host's");
            } else if (kit_other == 0) {
                kit_other = HashWay(m->hash) > HashWay(h.hash) ? 1 : -1;
            }
        }
    }
    if (kit_other > 0) {
        add("the host's kit is older than yours: its mod files can't be checked against yours (the host updates)");
    } else if (kit_other < 0) {
        add("your kit is older than the host's: your mod files can't be checked against its (update to its kit)");
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

namespace {
constexpr std::size_t kWayAt = std::tuple_size_v<proto::ModHash> - 3;  // 'S', 'Q', the way
}  // namespace

void MarkHashWay(proto::ModHash& hash) {
    hash[kWayAt] = 'S';
    hash[kWayAt + 1] = 'Q';
    hash[kWayAt + 2] = kHashWay;
}

std::uint8_t HashWay(const proto::ModHash& hash) {
    if (hash[kWayAt] != 'S' || hash[kWayAt + 1] != 'Q' || hash[kWayAt + 2] < 2) return 1;
    return hash[kWayAt + 2];
}

bool IsSeedId(std::string_view id) {
    constexpr std::string_view prefix = "randomizer-";
    if (id.size() <= prefix.size() || id.substr(0, prefix.size()) != prefix) return false;
    for (const char c : id.substr(prefix.size())) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true;
}

namespace {
std::string SeedWordOf(const std::string& id) {
    std::string word = id.substr(std::string_view("randomizer-").size());
    for (char& c : word) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return word;
}
}  // namespace

std::string SeedMissing(const proto::ModList& host, const proto::ModList& theirs) {
    for (const auto& h : host.mods) {
        if (!IsSeedId(h.id)) continue;
        bool has = false;
        for (const auto& t : theirs.mods) has = has || t.id == h.id;
        if (has) continue;
        return SeedWordOf(h.id);
    }
    return {};
}

std::string SeedFilesOther(const proto::ModList& host, const proto::ModList& theirs) {
    for (const auto& h : host.mods) {
        if (!IsSeedId(h.id) || h.hash == proto::ModHash{}) continue;
        for (const auto& t : theirs.mods) {
            if (t.id == h.id && t.hash != proto::ModHash{} && t.hash != h.hash && HashWay(t.hash) == HashWay(h.hash)) {
                return SeedWordOf(h.id);
            }
        }
    }
    return {};
}

int SeedKitOther(const proto::ModList& host, const proto::ModList& theirs, std::string* seed) {
    for (const auto& h : host.mods) {
        if (!IsSeedId(h.id) || h.hash == proto::ModHash{}) continue;
        for (const auto& t : theirs.mods) {
            if (t.id != h.id || t.hash == proto::ModHash{} || HashWay(t.hash) == HashWay(h.hash)) continue;
            if (seed != nullptr) *seed = SeedWordOf(h.id);
            return HashWay(t.hash) > HashWay(h.hash) ? 1 : -1;
        }
    }
    return 0;
}

namespace {
std::string Who(std::uint8_t number, std::string_view name) {
    return "player " + std::to_string(number) + (name.empty() ? std::string() : " (" + std::string(name) + ")");
}
}  // namespace

std::string SeedlessLine(std::uint8_t number, std::string_view name, std::string_view seed) {
    return "Mods: " + Who(number, name) + " runs WITHOUT the randomizer seed " + std::string(seed) +
           ": that game is the base game, another world (other items in chests); its chat says why";
}

std::string SeedlessMineLine(std::string_view seed) {
    return "Mods: you run WITHOUT the host's randomizer seed " + std::string(seed) +
           ": your game is the base game, not the host's world, until the seed is built and the game started again "
           "(the Randomizer's lines say why)";
}

std::string SeedOtherLine(std::uint8_t number, std::string_view name, std::string_view seed) {
    return "Mods: " + Who(number, name) + " has the randomizer seed " + std::string(seed) +
           " built unlike the host's: on another kit (another OOTRR) it is another world (other items in chests); "
           "all need the same kit";
}

std::string SeedOtherMineLine(std::string_view seed) {
    return "Mods: your randomizer seed " + std::string(seed) +
           " is built unlike the host's: on another kit (another OOTRR) it is another world (other items in chests); "
           "update to the host's kit";
}

std::string SeedKitLine(std::uint8_t number, std::string_view name, std::string_view seed, bool newer) {
    return "Mods: " + Who(number, name) + " is on " + (newer ? "a newer" : "an older") +
           " kit than the host's, so its randomizer seed " + std::string(seed) +
           " can't be checked (it may be another world); " +
           (newer ? "the host updates to it" : "it updates to the host's kit");
}

std::string SeedKitMineLine(std::string_view seed, bool newer) {
    return newer ? "Mods: the host's kit is older than yours, so its randomizer seed " + std::string(seed) +
                       " can't be checked against yours (it may be another world): the host updates to your kit"
                 : "Mods: your kit is older than the host's, so your randomizer seed " + std::string(seed) +
                       " can't be checked against its (it may be another world): update to the host's kit";
}

std::string SeedBackLine(std::uint8_t number, std::string_view name) {
    return "Mods: " + Who(number, name) + " runs the host's randomizer seed now";
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
        if (event.kind == EventKind::welcomed) {
            // A new session: Shared until its host says (a rejoin keeps it). The first player in is the host
            // from here and gets no host change: its own choice is in force now.
            CoopGoldMode::OnWelcomed(g.hosting);  // (coop_hook.cpp logs the mode)
            CoopRaceMode::OnWelcomed(g.hosting);
            CoopSwitchStandMode::OnWelcomed(g.hosting);
        }
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
            CoopGoldMode::OnHosting();  // our choice, unless a host before us told the session's mode
            CoopRaceMode::OnHosting();
            CoopSwitchStandMode::OnHosting();
        }
        g.differences.clear();
        g.guests_told.clear();
        g.seedless.clear();
        return;
    case EventKind::player_joined:
        g.names[event.player] = event.nickname;
        return;
    case EventKind::player_left:
        g.lists.erase(event.player);
        g.names.erase(event.player);
        g.guests_told.erase(event.player);
        g.seedless.erase(event.player);
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
            CoopGoldMode::OnHostSettings(settings->settings);
            CoopRaceMode::OnHostSettings(settings->settings);
            CoopSwitchStandMode::OnHostSettings(settings->settings);
        }
    }
}

void Frame(coop::host::Session* session) {
    Glue& g = TheGlue();
    std::optional<proto::Message> send_list;
    std::optional<proto::Message> send_settings;
    std::string race_in_settings;   // the race id send_settings carries (CoopGoldMode::MarkRaceSent once sent)
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
            if (g.hosting) {
                // Co-op's own host settings first (CoopGoldMode: the mode in force and, Not shared, the race id;
                // never cut off below), then the mods'. A mod that calls itself "coop" cannot give the same keys.
                std::vector<proto::ModSetting> shared{CoopGoldMode::AsSetting(CoopGoldMode::InForce()),
                                                      CoopRaceMode::AsSetting(), CoopSwitchStandMode::AsSetting()};
                if (auto race = CoopGoldMode::RaceSetting()) {
                    race_in_settings = race->value;
                    shared.push_back(std::move(*race));
                }
                if (g.provider.shared) {
                    for (proto::ModSetting& s : g.provider.shared()) {
                        if (s.mod_id != CoopGoldMode::kModId ||
                            (s.key != CoopGoldMode::kKey && s.key != CoopGoldMode::kRaceKey &&
                             s.key != proto::kRaceKey && s.key != CoopSwitchStandMode::kKey)) {
                            shared.push_back(std::move(s));
                        }
                    }
                }
                if (shared.size() > proto::kMaxModSettingEntries) {
                    shared.resize(proto::kMaxModSettingEntries);
                }
                if (g.shared_sent != shared) {
                    g.shared_sent = shared;
                    send_settings = proto::ModSettings{proto::kPlayerSelf, std::move(shared)};
                } else if (!race_in_settings.empty()) {
                    CoopGoldMode::MarkRaceSent(race_in_settings);   // (sent before)
                }
            }
            if (mine && !g.hosting) {
                const auto host = g.lists.find(g.host_number);
                if (host != g.lists.end()) {
                    std::vector<std::string> diff = Compare(host->second, *mine);
                    if (diff != g.differences) {
                        if (const std::string seed = SeedMissing(host->second, *mine); !seed.empty()) {
                            g.Line(SeedlessMineLine(seed));  // (the other differences are in the Mods tab)
                        } else if (const std::string other = SeedFilesOther(host->second, *mine); !other.empty()) {
                            g.Line(SeedOtherMineLine(other));
                        } else if (std::string kit_seed; const int kit = SeedKitOther(host->second, *mine, &kit_seed)) {
                            g.Line(SeedKitMineLine(kit_seed, kit > 0));
                        } else if (!diff.empty()) {
                            g.Line("Mods: yours differ from the host's (" + diff.front() +
                                   (diff.size() > 1 ? ", ..." : "") + "): see the Mods tab");
                        } else if (!g.differences.empty()) {
                            g.Line("Mods: yours now match the host's");
                        }
                        g.differences = std::move(diff);
                    }
                }
            }
            // A player without the host's seed plays another world (#59): the host and every other player are told,
            // once per change (a guest compares the others' lists with the host's).
            const proto::ModList* host_list = nullptr;
            if (g.hosting && mine) {
                host_list = &*mine;
            } else if (!g.hosting) {
                const auto host = g.lists.find(g.host_number);
                if (host != g.lists.end()) host_list = &host->second;
            }
            if (host_list != nullptr) {
                for (const auto& [number, list] : g.lists) {
                    if (!g.hosting && number == g.host_number) continue;
                    // 0: the host's seed, 1: none (SeedMissing), 2: other files (SeedFilesOther: another kit),
                    // 3 / 4: hashed another way (SeedKitOther: the player's kit is older / newer than the host's)
                    std::string seed = SeedMissing(*host_list, list);
                    int state = seed.empty() ? 0 : 1;
                    if (state == 0) {
                        seed = SeedFilesOther(*host_list, list);
                        if (!seed.empty()) state = 2;
                    }
                    if (state == 0) {
                        if (const int kit = SeedKitOther(*host_list, list, &seed)) state = kit > 0 ? 4 : 3;
                    }
                    const auto told = g.seedless.try_emplace(number, 0).first;
                    if (told->second == state) continue;
                    told->second = state;
                    const auto name = g.names.find(number);
                    const std::string who = name != g.names.end() ? name->second : std::string();
                    g.Line(state == 1   ? SeedlessLine(number, who, seed)
                           : state == 2 ? SeedOtherLine(number, who, seed)
                           : state >= 3 ? SeedKitLine(number, who, seed, state == 4)
                                        : SeedBackLine(number, who));
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
        if (sent && m == &send_settings && !race_in_settings.empty()) {
            CoopGoldMode::MarkRaceSent(race_in_settings);
        }
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
    std::function<std::vector<std::string>()> notes;
    std::vector<std::string> out;
    {
        std::lock_guard lock(g.mutex);
        out = std::exchange(g.lines, {});
        notes = g.provider.notes;
    }
    if (notes) {
        for (std::string& line : notes()) out.push_back(std::move(line));  // outside our lock: the mods lock their own
    }
    return out;
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
