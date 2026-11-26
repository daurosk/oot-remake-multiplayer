// SPDX-License-Identifier: GPL-3.0-or-later
// Gold Skulltulas, Shared or Not shared (coop_gold_mode.h, sync-coverage row 63).
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "coop/host/flags.hpp"
#include "coop_gold_mode.h"

namespace proto = coop::protocol;
using CoopGoldMode::Mode;

TEST_CASE("gold mode: the values, and the coop entry a host's settings carry") {
    CHECK(CoopGoldMode::Value(Mode::shared) == "shared");
    CHECK(CoopGoldMode::Value(Mode::own) == "own");
    CHECK(CoopGoldMode::Parse("shared") == Mode::shared);
    CHECK(CoopGoldMode::Parse("own") == Mode::own);
    CHECK_FALSE(CoopGoldMode::Parse("Own").has_value());
    CHECK_FALSE(CoopGoldMode::Parse("").has_value());

    const proto::ModSetting s = CoopGoldMode::AsSetting(Mode::own);
    CHECK(s.mod_id == "coop");
    CHECK(s.key == "gold_skulltula");
    CHECK(s.value == "own");
    CHECK(CoopGoldMode::FromSettings({{"ootrr", "gold_skulltula", "own"}, s}) == Mode::own);
    CHECK_FALSE(CoopGoldMode::FromSettings({{"ootrr", "gold_skulltula", "own"}}).has_value());
    CHECK_FALSE(CoopGoldMode::FromSettings({{"coop", "gold_skulltula", "nonsense"}}).has_value());
}

TEST_CASE("gold mode: a host plays its own choice, a guest the host's, and with no word from the host Shared") {
    CoopGoldMode::ResetForTest();
    CHECK(CoopGoldMode::InForce() == Mode::shared);  // the default

    // Host: its choice from the menu.
    CoopGoldMode::SetChoice(Mode::own);
    CoopGoldMode::OnConnection();
    CHECK_FALSE(CoopGoldMode::NotShared());  // not before it hosts
    CoopGoldMode::OnHosting();
    CHECK(CoopGoldMode::NotShared());

    // Guest with its own choice Not shared, host says Shared: Shared.
    CoopGoldMode::OnConnection();
    CoopGoldMode::OnHostSettings({CoopGoldMode::AsSetting(Mode::shared)});
    CHECK(CoopGoldMode::InForce() == Mode::shared);

    // Guest of a host from before this setting (settings with no coop entry): Shared.
    CoopGoldMode::OnConnection();
    CoopGoldMode::OnHostSettings({{"ootrr", "seed", "1"}});
    CHECK(CoopGoldMode::InForce() == Mode::shared);
    CoopGoldMode::ResetForTest();
}

TEST_CASE("gold mode: a change in the menu during a session takes effect at the next Host") {
    CoopGoldMode::ResetForTest();
    CoopGoldMode::OnConnection();
    CoopGoldMode::OnHosting();
    CHECK(CoopGoldMode::InForce() == Mode::shared);
    CoopGoldMode::SetChoice(Mode::own);  // changed while hosting
    CoopGoldMode::OnHosting();           // (a host event again in the same session)
    CHECK(CoopGoldMode::InForce() == Mode::shared);
    CHECK(CoopGoldMode::Choice() == Mode::own);
    // Stop hosting, Host again: a new connection.
    CoopGoldMode::OnConnection();
    CoopGoldMode::OnHosting();
    CHECK(CoopGoldMode::InForce() == Mode::own);
    CoopGoldMode::ResetForTest();
}

TEST_CASE("gold mode: a guest that becomes host after a migration keeps the session's mode, not its own choice") {
    CoopGoldMode::ResetForTest();
    CoopGoldMode::SetChoice(Mode::shared);
    CoopGoldMode::OnConnection();
    CoopGoldMode::OnHostSettings({CoopGoldMode::AsSetting(Mode::own)});  // the first host's
    CHECK(CoopGoldMode::NotShared());
    CoopGoldMode::OnHosting();  // the host left; we host now
    CHECK(CoopGoldMode::NotShared());
    CoopGoldMode::ResetForTest();

    CoopGoldMode::SetChoice(Mode::own);
    CoopGoldMode::OnConnection();
    CoopGoldMode::OnHostSettings({CoopGoldMode::AsSetting(Mode::shared)});
    CoopGoldMode::OnHosting();
    CHECK(CoopGoldMode::InForce() == Mode::shared);
    CoopGoldMode::ResetForTest();
}

TEST_CASE("gold mode: after a Not shared session, a new session keeps nothing local before its mode is told") {
    // Review of f36a8852, bug 1: keep_local has no way back, so the frames of a new session before its
    // welcome must not see the last session's Not shared.
    CoopGoldMode::ResetForTest();
    CoopGoldMode::SetChoice(Mode::own);
    CoopGoldMode::OnConnection();
    CoopGoldMode::OnHosting();
    CHECK(CoopGoldMode::KeepLocalNow());  // hosting Not shared

    // Stop hosting; the player picks Shared and joins a friend (or hosts again). Begin resets the mode
    // before its client starts; no welcome or settings yet.
    CoopGoldMode::SetChoice(Mode::shared);
    CoopGoldMode::OnConnection();
    CHECK_FALSE(CoopGoldMode::Told());
    CHECK_FALSE(CoopGoldMode::NotShared());
    CHECK_FALSE(CoopGoldMode::KeepLocalNow());

    // A host that says Not shared: now it keeps them.
    CoopGoldMode::OnHostSettings({CoopGoldMode::AsSetting(Mode::own)});
    CHECK(CoopGoldMode::KeepLocalNow());
    // A host that says Shared: not.
    CoopGoldMode::OnConnection();
    CoopGoldMode::OnHostSettings({CoopGoldMode::AsSetting(Mode::shared)});
    CHECK_FALSE(CoopGoldMode::KeepLocalNow());
    CoopGoldMode::ResetForTest();
}

namespace {
constexpr std::uint32_t kCount = 0x04735978;  // PouchData.Item_Token_Gold.StockNum
constexpr std::uint32_t kGet = 0x6ba394fe;    // IsGet.Item_Token_Gold
constexpr std::uint32_t kOther = 0x11111111;  // another Int, not touched
constexpr std::uint64_t kSkull[] = {0x1000, 0x2000, 0x3000};
constexpr std::uint64_t kWeb = 0x1500;        // a burned web: not a Gold Skulltula's

void Put32(std::vector<std::uint8_t>& d, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) d.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void Put64(std::vector<std::uint8_t>& d, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) d.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
std::uint32_t Get32(const std::vector<std::uint8_t>& d, std::size_t at) {
    std::uint32_t v = 0;
    for (int i = 3; i >= 0; --i) v = v << 8 | d[at + static_cast<std::size_t>(i)];
    return v;
}

// A save in tools/autotest/save_flags.py's layout: Bool IsGet, Int count and another Int, the types in order,
// the end record and its pair, then (with `with_set`) the key set.
std::vector<std::uint8_t> MakeSave(std::uint32_t count, std::uint32_t get, std::vector<std::uint64_t> keys,
                                   bool with_set = true) {
    std::vector<std::uint8_t> d;
    Put32(d, 0x01020304);
    Put32(d, 1);
    Put32(d, 0);  // the pool's offset, below
    while (d.size() < 0x20) d.push_back(0);
    for (std::uint32_t t = 0; t < 0x20; ++t) {
        Put32(d, 0);
        Put32(d, t);
        if (t == 0) {
            Put32(d, kGet);
            Put32(d, get);
        } else if (t == 2) {
            Put32(d, kCount);
            Put32(d, count);
            Put32(d, kOther);
            Put32(d, 77);
        }
    }
    Put32(d, 0);
    Put32(d, 0x20);
    const std::size_t pair = d.size();
    Put32(d, 0xabcdef01);  // the file's hash
    Put32(d, 0);           // the set's offset
    const std::size_t pool = d.size();
    for (int i = 0; i < 4; ++i) d[8 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(pool >> (8 * i));
    if (with_set) {
        for (int i = 0; i < 4; ++i) d[pair + 4 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(pool >> (8 * i));
        std::sort(keys.begin(), keys.end());
        for (const std::uint64_t k : keys) Put64(d, k);
        Put64(d, 0);
    }
    return d;
}
// A race: our Skulltulas, count and IsGet.
CoopGoldMode::Race MakeRace(std::int32_t count, bool get, std::initializer_list<std::uint64_t> keys) {
    CoopGoldMode::Race r;
    r.count = count;
    r.get = get;
    r.keys = keys;
    return r;
}

// The game's flags over a map: a flag not in it does not read (not loaded).
class FakeGame final : public coop::host::FlagStore {
public:
    std::optional<proto::FlagEntry> read(proto::FlagKey key) override {
        const auto it = flags.find(key);
        if (it == flags.end()) return std::nullopt;
        return it->second;
    }
    bool write(const proto::FlagEntry& entry) override {
        const auto it = flags.find(entry.key);
        if (refuse || it == flags.end()) return false;
        it->second.value = entry.value;
        return true;
    }
    void Bool(std::uint64_t key, bool on) {
        const auto k = proto::FlagKey::key64_flag(key);
        flags[k] = proto::FlagEntry(k, proto::FlagKind::boolean, on ? 1 : 0);
    }
    bool On(std::uint64_t key) const { return flags.at(proto::FlagKey::key64_flag(key)).value != 0; }
    void Named(std::uint32_t hash, proto::FlagKind kind, std::uint32_t v) {
        const proto::FlagKey k(hash, std::uint16_t{0});
        flags[k] = proto::FlagEntry(k, kind, v);
    }
    std::uint32_t Value(std::uint32_t hash) const { return flags.at(proto::FlagKey(hash, std::uint16_t{0})).value; }
    std::map<proto::FlagKey, proto::FlagEntry> flags;
    bool refuse = false;
};

// A game with every Skulltula's two keys off, count `count`, IsGet `get`.
FakeGame MakeGame(std::int32_t count, bool get) {
    FakeGame g;
    for (const std::uint64_t k : kSkull) {
        g.Bool(k, false);
        g.Bool(k + 2, false);
    }
    g.Named(kCount, proto::FlagKind::int32, static_cast<std::uint32_t>(count));
    g.Named(kGet, proto::FlagKind::boolean, get ? 1 : 0);
    return g;
}

namespace fs = std::filesystem;
fs::path FreshDir(const char* name) {
    const fs::path dir = fs::temp_directory_path() / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    return dir;
}

const std::array<std::uint8_t, 16> kHostKey{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
const std::array<std::uint8_t, 16> kOtherHostKey{16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1};

// As a guest of the host with race id `id` in a Not shared session.
void JoinRace(const std::string& id) {
    CoopGoldMode::OnConnection();
    CoopGoldMode::OnWelcomed(false);
    CoopGoldMode::OnHostSettings({CoopGoldMode::AsSetting(Mode::own), {"coop", "gold_race", id}});
}
}  // namespace

TEST_CASE("gold mode: Not shared, the host's save keeps this game's race: tokens, count and IsGet") {
    // Review of f36a8852: a race guest got the host's tokens and count at join and at Resync from host.
    const auto race = MakeRace(1, true, {kSkull[1] + 2, kSkull[1], 0x9000});
    std::vector<std::uint8_t> host = MakeSave(2, 1, {kSkull[0], kSkull[0] + 2, kSkull[2], kSkull[2] + 2, kWeb});
    const auto kept = CoopGoldMode::KeepRaceIn(host, &race, kSkull, kCount, kGet);
    REQUIRE(kept.done);
    CHECK(kept.host_keys == 4);
    CHECK(kept.own_keys == 2);
    CHECK(kept.host_count == 2);
    CHECK(kept.own_count == 1);
    CHECK_FALSE(kept.keys_left);
    // Ours, and the host's own (the web) kept; not our other keys (0x9000 is not a Skulltula's).
    const std::vector<std::uint8_t> want = MakeSave(1, 1, {kSkull[1], kSkull[1] + 2, kWeb});
    CHECK(host == want);
    CHECK(Get32(host, 0x20 + 8 * 4 + 4 + 8) == 77);  // the other Int (after type 0's two records and type 1's)
}

TEST_CASE("gold mode: Not shared, no race with this host starts from nothing") {
    std::vector<std::uint8_t> host = MakeSave(3, 1, {kSkull[0], kSkull[0] + 2, kWeb});
    CHECK(CoopGoldMode::KeepRaceIn(host, nullptr, kSkull, kCount, kGet).done);
    CHECK(host == MakeSave(0, 0, {kWeb}));
    const CoopGoldMode::Race none;
    std::vector<std::uint8_t> host2 = MakeSave(3, 1, {kSkull[0], kWeb});
    CHECK(CoopGoldMode::KeepRaceIn(host2, &none, kSkull, kCount, kGet).done);
    CHECK(host2 == MakeSave(0, 0, {kWeb}));
}

TEST_CASE("gold mode: Not shared, a host's save with no key set or that does not read") {
    // No key set: count and IsGet are ours, our keys cannot be added (said in the log).
    const auto race = MakeRace(1, true, {kSkull[1]});
    std::vector<std::uint8_t> host = MakeSave(0, 0, {}, false);
    const auto kept = CoopGoldMode::KeepRaceIn(host, &race, kSkull, kCount, kGet);
    REQUIRE(kept.done);
    CHECK(kept.keys_left);
    CHECK(host == MakeSave(1, 1, {}, false));
    // Not a save: left as it is.
    std::vector<std::uint8_t> junk(64, 0xee);
    const std::vector<std::uint8_t> was = junk;
    const auto bad = CoopGoldMode::KeepRaceIn(junk, &race, kSkull, kCount, kGet);
    CHECK_FALSE(bad.done);
    CHECK(bad.why.find("not a saved game") != std::string::npos);
    CHECK(junk == was);
}

TEST_CASE("gold mode: a race is saved and read back as text, and its file is per host") {
    const auto race = MakeRace(3, true, {kSkull[0], kSkull[0] + 2, 0xfedcba9876543210ULL});
    const std::string text = CoopGoldMode::RaceText(race);
    CHECK(CoopGoldMode::ParseRace(text) == race);
    CHECK(CoopGoldMode::ParseRace(CoopGoldMode::RaceText({})) == CoopGoldMode::Race{});
    CHECK_FALSE(CoopGoldMode::ParseRace("").has_value());
    CHECK_FALSE(CoopGoldMode::ParseRace("something else\ncount 1\n").has_value());
    CHECK_FALSE(CoopGoldMode::ParseRace(text + "count -1\n").has_value());
    CHECK_FALSE(CoopGoldMode::ParseRace(text + "key 12zz\n").has_value());
    CHECK_FALSE(CoopGoldMode::ParseRace(text + "colour red\n").has_value());

    const fs::path dir = FreshDir("coop_tests_race_file");
    const std::string a = CoopGoldMode::RaceIdOf(kHostKey);
    const std::string b = CoopGoldMode::RaceIdOf(kOtherHostKey);
    CHECK(a.size() == 16);
    CHECK(a != b);
    CHECK(a == CoopGoldMode::RaceIdOf(kHostKey));
    CHECK(CoopGoldMode::RaceFile(dir, a) == dir / (a + ".txt"));
    CHECK(CoopGoldMode::RaceFile(dir, "../escape").empty());  // a told id is never a path
    CHECK_FALSE(CoopGoldMode::SaveRace(dir, "ABC", race));
    CHECK_FALSE(CoopGoldMode::LoadRace(dir, a).has_value());
    CHECK(CoopGoldMode::SaveRace(dir, a, race));
    CHECK(CoopGoldMode::LoadRace(dir, a) == race);
    CHECK_FALSE(CoopGoldMode::LoadRace(dir, b).has_value());  // another host's: none
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("gold mode: Not shared, the race is read from the game and put back, never lowered") {
    FakeGame game = MakeGame(2, true);
    game.Bool(kSkull[0], true);
    game.Bool(kSkull[0] + 2, true);
    game.Bool(kSkull[1], true);
    const auto seen = CoopGoldMode::ReadRace(game, kSkull, kCount, kGet);
    REQUIRE(seen.has_value());
    CHECK(*seen == MakeRace(2, true, {kSkull[0], kSkull[0] + 2, kSkull[1]}));
    FakeGame empty;
    CHECK_FALSE(CoopGoldMode::ReadRace(empty, kSkull, kCount, kGet).has_value());  // no save in the game yet

    // A loaded save that has fewer: ours go back in.
    FakeGame loaded = MakeGame(0, false);
    loaded.Bool(kSkull[2], true);  // one the save has that the race does not: left on
    auto put = CoopGoldMode::PutRaceBack(loaded, *seen, kCount, kGet);
    CHECK(put.done());
    CHECK(put.written == 5);
    CHECK(loaded.On(kSkull[0]));
    CHECK(loaded.On(kSkull[0] + 2));
    CHECK(loaded.On(kSkull[1]));
    CHECK(loaded.On(kSkull[2]));
    CHECK(loaded.Value(kCount) == 2);
    CHECK(loaded.Value(kGet) == 1);
    // Again: nothing to write.
    CHECK(CoopGoldMode::PutRaceBack(loaded, *seen, kCount, kGet).written == 0);
    // A higher count in the game stays.
    FakeGame more = MakeGame(5, true);
    CoopGoldMode::PutRaceBack(more, *seen, kCount, kGet);
    CHECK(more.Value(kCount) == 5);
    // Not loaded yet, or refused: not done (the hook tries again).
    FakeGame partial;
    partial.Named(kCount, proto::FlagKind::int32, 0);
    CHECK(CoopGoldMode::PutRaceBack(partial, *seen, kCount, kGet).unread == 4);
    FakeGame refusing = MakeGame(0, false);
    refusing.refuse = true;
    put = CoopGoldMode::PutRaceBack(refusing, *seen, kCount, kGet);
    CHECK_FALSE(put.done());
    CHECK(put.failed == 5);

    // Join: only grows.
    auto race = MakeRace(3, false, {kSkull[0]});
    CHECK(race.Join(*seen));
    CHECK(race == MakeRace(3, true, {kSkull[0], kSkull[0] + 2, kSkull[1]}));
    CHECK_FALSE(race.Join(MakeRace(1, false, {kSkull[1]})));
}

TEST_CASE("gold mode: Not shared, a guest's race with a host is kept per host across sessions and loads") {
    const fs::path dir = FreshDir("coop_tests_race_store");
    CoopGoldMode::ResetForTest();
    CoopGoldMode::SetRaceDir(dir);
    const std::string host_a = CoopGoldMode::RaceIdOf(kHostKey);
    const std::string host_b = CoopGoldMode::RaceIdOf(kOtherHostKey);

    // Session 1 with host A: the race starts from 0, and grows from what the game shows.
    JoinRace(host_a);
    CHECK(CoopGoldMode::RaceId() == host_a);
    CHECK(CoopGoldMode::TheRace().empty());
    FakeGame game = MakeGame(1, true);
    game.Bool(kSkull[0], true);
    game.Bool(kSkull[0] + 2, true);
    CHECK(CoopGoldMode::GrowRace(*CoopGoldMode::ReadRace(game, kSkull, kCount, kGet)));
    CHECK_FALSE(CoopGoldMode::GrowRace(*CoopGoldMode::ReadRace(game, kSkull, kCount, kGet)));  // nothing new
    const auto race = MakeRace(1, true, {kSkull[0], kSkull[0] + 2});
    CHECK(CoopGoldMode::LoadRace(dir, host_a) == race);  // on disk at once

    // A save load that lost the token (not saved): it goes back in.
    FakeGame reloaded = MakeGame(0, false);
    CHECK(CoopGoldMode::PutRaceBack(reloaded, CoopGoldMode::TheRace(), kCount, kGet).done());
    CHECK(reloaded.Value(kCount) == 1);
    CHECK(reloaded.On(kSkull[0]));

    // Both games stopped and started again: the race is read from its file. Join and Resync: the host's save keeps it.
    CoopGoldMode::ResetForTest();
    CoopGoldMode::SetRaceDir(dir);
    JoinRace(host_a);
    CHECK(CoopGoldMode::TheRace() == race);
    const auto kept = CoopGoldMode::TheRace();
    std::vector<std::uint8_t> host_save = MakeSave(4, 1, {kSkull[1], kSkull[1] + 2, kSkull[2], kSkull[2] + 2, kWeb});
    CHECK(CoopGoldMode::KeepRaceIn(host_save, &kept, kSkull, kCount, kGet).done);
    CHECK(host_save == MakeSave(1, 1, {kSkull[0], kSkull[0] + 2, kWeb}));
    std::vector<std::uint8_t> resync = MakeSave(5, 1, {kSkull[1], kSkull[1] + 2, kWeb});
    CHECK(CoopGoldMode::KeepRaceIn(resync, &kept, kSkull, kCount, kGet).done);
    CHECK(resync == MakeSave(1, 1, {kSkull[0], kSkull[0] + 2, kWeb}));

    // Another host's game does not take it: a race of its own, from 0.
    JoinRace(host_b);
    CHECK(CoopGoldMode::TheRace().empty());
    std::vector<std::uint8_t> other = MakeSave(4, 1, {kSkull[1], kWeb});
    const auto none = CoopGoldMode::TheRace();
    CHECK(CoopGoldMode::KeepRaceIn(other, &none, kSkull, kCount, kGet).done);
    CHECK(other == MakeSave(0, 0, {kWeb}));
    CHECK(CoopGoldMode::LoadRace(dir, host_a) == race);  // A's untouched

    // An older host that sends no id: the race lives for this session only, no file.
    CoopGoldMode::OnConnection();
    CoopGoldMode::OnWelcomed(false);
    CoopGoldMode::OnHostSettings({CoopGoldMode::AsSetting(Mode::own)});
    CHECK(CoopGoldMode::RaceId().empty());
    CHECK(CoopGoldMode::GrowRace(MakeRace(2, false, {kSkull[2]})));
    CHECK(CoopGoldMode::TheRace() == MakeRace(2, false, {kSkull[2]}));
    // A told id that is not one: ignored.
    CoopGoldMode::OnHostSettings({{"coop", "gold_race", "../../x"}});
    CHECK(CoopGoldMode::RaceId().empty());
    CoopGoldMode::ResetForTest();
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("gold mode: the host sends its race id only Not shared, and a switch to Not shared starts from 0") {
    const fs::path dir = FreshDir("coop_tests_race_switch");
    CoopGoldMode::ResetForTest();
    CoopGoldMode::SetRaceDir(dir);
    CoopGoldMode::SetOwnRaceId(CoopGoldMode::RaceIdOf(kHostKey));

    // A Shared host: no id in its settings.
    CoopGoldMode::SetChoice(Mode::shared);
    CoopGoldMode::OnConnection();
    CoopGoldMode::OnWelcomed(true);
    CHECK_FALSE(CoopGoldMode::RaceSetting().has_value());
    // Not shared: the coop entry carries its own id.
    CoopGoldMode::SetChoice(Mode::own);
    CoopGoldMode::OnConnection();
    CoopGoldMode::OnWelcomed(true);
    const auto id = CoopGoldMode::RaceSetting();
    REQUIRE(id.has_value());
    CHECK(id->mod_id == "coop");
    CHECK(id->key == "gold_race");
    CHECK(id->value == CoopGoldMode::RaceIdOf(kHostKey));

    // A guest in a Shared session never keeps a race (the hook keeps one only from a race save): a switch to Not
    // shared later starts from 0 with that host, never from the shared count.
    JoinRace(CoopGoldMode::RaceIdOf(kOtherHostKey));
    CHECK(CoopGoldMode::TheRace().empty());
    std::vector<std::uint8_t> shared = MakeSave(7, 1, {kSkull[0], kSkull[0] + 2, kSkull[1], kSkull[1] + 2, kWeb});
    const auto race = CoopGoldMode::TheRace();
    CHECK(CoopGoldMode::KeepRaceIn(shared, &race, kSkull, kCount, kGet).done);
    CHECK(shared == MakeSave(0, 0, {kWeb}));

    // A host that was a guest (migration) keeps the old host's id, not its own.
    CoopGoldMode::OnHosting();
    CHECK(CoopGoldMode::RaceId() == CoopGoldMode::RaceIdOf(kOtherHostKey));
    CoopGoldMode::ResetForTest();
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("gold mode: the first player in hosts from its welcome, with no host change, and plays its own choice") {
    // Lane run of 2026-11-23: a Not shared host that was the first player in played Shared, since OnHosting ran
    // only on a host change and the first player gets none.
    CoopGoldMode::ResetForTest();
    CoopGoldMode::SetChoice(Mode::own);
    CoopGoldMode::OnWelcomed(true);
    CHECK(CoopGoldMode::Told());
    CHECK(CoopGoldMode::NotShared());
    CHECK(CoopGoldMode::KeepLocalNow());

    // A guest's welcome: Shared until the host says, whatever its own choice.
    CoopGoldMode::OnWelcomed(false);
    CHECK_FALSE(CoopGoldMode::Told());
    CHECK(CoopGoldMode::InForce() == Mode::shared);
    CoopGoldMode::OnHostSettings({CoopGoldMode::AsSetting(Mode::own)});
    CHECK(CoopGoldMode::KeepLocalNow());

    // A Shared host's welcome after a Not shared session: Shared.
    CoopGoldMode::SetChoice(Mode::shared);
    CoopGoldMode::OnWelcomed(true);
    CHECK(CoopGoldMode::InForce() == Mode::shared);
    CoopGoldMode::ResetForTest();
}

TEST_CASE("gold mode: Not shared, a guest's race never changes the host's save or the host's game") {
    // Coordinator #11: the race goes into the guest's copy of the host's save (the bytes it got, written to its own
    // co-op folder) and into the guest's game; the host's save and the host's game keep the host's count.
    const std::vector<std::uint8_t> host_save = MakeSave(4, 1, {kSkull[1], kSkull[1] + 2, kWeb});
    FakeGame host_game = MakeGame(4, true);
    host_game.Bool(kSkull[1], true);
    host_game.Bool(kSkull[1] + 2, true);
    const FakeGame host_before = host_game;

    // The guest: its copy of what the host sent gets its race (none yet: from 0).
    const auto race = MakeRace(1, false, {kSkull[0], kSkull[0] + 2});
    std::vector<std::uint8_t> guest_copy = host_save;
    CHECK(CoopGoldMode::KeepRaceIn(guest_copy, &race, kSkull, kCount, kGet).done);
    CHECK(guest_copy == MakeSave(1, 0, {kSkull[0], kSkull[0] + 2, kWeb}));
    CHECK(host_save == MakeSave(4, 1, {kSkull[1], kSkull[1] + 2, kWeb}));   // the host's own, unchanged

    // The guest's game after its load: its race goes back in there only.
    FakeGame guest_game = MakeGame(0, false);
    CHECK(CoopGoldMode::PutRaceBack(guest_game, race, kCount, kGet).done());
    CHECK(guest_game.Value(kCount) == 1);
    CHECK(host_game.flags == host_before.flags);
    CHECK(CoopGoldMode::ReadRace(host_game, kSkull, kCount, kGet) ==
          MakeRace(4, true, {kSkull[1], kSkull[1] + 2}));
}

TEST_CASE("gold mode: each playthrough of the host's has its own race id") {
    const std::string p1 = CoopGoldMode::NewPlaythroughId();
    const std::string p2 = CoopGoldMode::NewPlaythroughId();
    CHECK(p1.size() == 16);
    CHECK(p1 != p2);
    CHECK(CoopGoldMode::RaceIdOf(kHostKey, "") == CoopGoldMode::RaceIdOf(kHostKey));   // none known: as before
    CHECK(CoopGoldMode::RaceIdOf(kHostKey, p1) != CoopGoldMode::RaceIdOf(kHostKey));
    CHECK(CoopGoldMode::RaceIdOf(kHostKey, p1) != CoopGoldMode::RaceIdOf(kHostKey, p2));
    CHECK(CoopGoldMode::RaceIdOf(kHostKey, p1) != CoopGoldMode::RaceIdOf(kOtherHostKey, p1));
    CHECK(CoopGoldMode::RaceIdOf(kHostKey, p1) == CoopGoldMode::RaceIdOf(kHostKey, p1));
    CHECK(CoopGoldMode::RaceIdOf(kHostKey, p1).size() == 16);
}

TEST_CASE("gold mode: the host's playthroughs per slot, and a new game's") {
    const fs::path dir = FreshDir("coop_tests_playthroughs");
    std::string a, b, fresh;
    {
        CoopGoldMode::Playthroughs pts(dir);
        a = pts.OnSlot("slot_00", false);   // a load of a save from before: an id of its own, kept
        CHECK(a.size() == 16);
        CHECK(pts.Current() == a);
        CHECK(pts.OnSlot("slot_00", false) == a);
        b = pts.OnSlot("slot_01", false);   // an older playthrough, loaded once
        CHECK(b != a);
        CHECK(pts.OnSlot("slot_00", false) == a);   // back to A
        // Review of af66da86: a save while playing A is A, whatever slot it goes to (another slot, a rotated autosave
        // slot never seen).
        CHECK(pts.OnSlot("slot_01", true) == a);
        CHECK(pts.OnSlot("slot_02", true) == a);
        CHECK(pts.Current() == a);
        CHECK(pts.OnSlot("slot_01", false) == a);   // slot_01 now holds A
        CHECK(pts.OnSlot("", false).empty());

        // Load X, the title, New Game: a fresh id, the same until the title; saved into the slot loaded before it.
        pts.OnTitle();
        fresh = pts.OnNewGame();
        CHECK(fresh != a);
        CHECK(pts.OnNewGame() == fresh);
        CHECK(pts.OnSlot("slot_00", true) == fresh);
        CHECK(pts.OnSlot("slot_03", true) == fresh);   // its next autosave into another slot
        // Load X, the title, New Game (not saved), the title, Continue X: X's own.
        pts.OnSlot("slot_02", false);
        pts.OnTitle();
        const std::string unsaved = pts.OnNewGame();
        CHECK(unsaved != fresh);
        pts.OnTitle();
        CHECK(pts.OnSlot("slot_02", false) == a);
        CHECK(pts.OnNewGame() != unsaved);   // (the title forgot that new game)
    }
    // Kept across runs.
    CoopGoldMode::Playthroughs again(dir);
    CHECK(again.Slots().size() == 4);
    CHECK(again.Current().empty());
    CHECK(again.OnSlot("slot_00", false) == fresh);
    CHECK(again.OnSlot("slot_01", false) == a);
    // Hosting began mid-game (nothing in force yet), and the game saves: that slot's own.
    CoopGoldMode::Playthroughs mid(dir);
    CHECK(mid.OnSlot("slot_03", true) == fresh);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("gold mode: the host's race id follows its playthrough, and its save waits until the id is sent") {
    const fs::path dir = FreshDir("coop_tests_race_playthrough");
    CoopGoldMode::ResetForTest();
    CoopGoldMode::SetRaceDir(dir);
    CoopGoldMode::SetOwnKey(kHostKey);
    CoopGoldMode::SetChoice(Mode::own);
    CoopGoldMode::OnWelcomed(true);
    CHECK(CoopGoldMode::RaceId() == CoopGoldMode::RaceIdOf(kHostKey));   // no playthrough known yet
    CHECK(CoopGoldMode::RaceSendDue());
    CoopGoldMode::MarkRaceSent(CoopGoldMode::RaceId());
    CHECK_FALSE(CoopGoldMode::RaceSendDue());

    const std::string p1 = CoopGoldMode::NewPlaythroughId();
    CoopGoldMode::SetPlaythrough(p1);
    const std::string id1 = CoopGoldMode::RaceIdOf(kHostKey, p1);
    CHECK(CoopGoldMode::RaceId() == id1);
    REQUIRE(CoopGoldMode::RaceSetting().has_value());
    CHECK(CoopGoldMode::RaceSetting()->value == id1);
    CHECK(CoopGoldMode::RaceSendDue());   // the save waits for this id to go out
    CoopGoldMode::MarkRaceSent(id1);
    CHECK_FALSE(CoopGoldMode::RaceSendDue());
    CoopGoldMode::SetPlaythrough("not an id");   // ignored: the key's alone
    CHECK(CoopGoldMode::RaceId() == CoopGoldMode::RaceIdOf(kHostKey));
    CoopGoldMode::SetPlaythrough(p1);

    // Shared: nothing waits.
    CoopGoldMode::OnConnection();
    CoopGoldMode::SetChoice(Mode::shared);
    CoopGoldMode::OnWelcomed(true);
    CHECK_FALSE(CoopGoldMode::RaceSendDue());

    // A host after a migration keeps the told id, whatever its own game plays.
    CoopGoldMode::SetChoice(Mode::own);
    JoinRace(id1);
    CoopGoldMode::OnHosting();
    CoopGoldMode::SetPlaythrough(CoopGoldMode::NewPlaythroughId());
    CHECK(CoopGoldMode::RaceId() == id1);
    CHECK_FALSE(CoopGoldMode::RaceSendDue());

    // A guest: each playthrough of the host's is a race of its own.
    const std::string id2 = CoopGoldMode::RaceIdOf(kHostKey, CoopGoldMode::NewPlaythroughId());
    JoinRace(id1);
    CHECK(CoopGoldMode::GrowRace(MakeRace(3, true, {kSkull[0]})));
    JoinRace(id2);
    CHECK(CoopGoldMode::TheRace().empty());
    CHECK(CoopGoldMode::GrowRace(MakeRace(1, false, {kSkull[1]})));
    JoinRace(id1);
    CHECK(CoopGoldMode::TheRace() == MakeRace(3, true, {kSkull[0]}));
    CoopGoldMode::ResetForTest();
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("gold mode: the host's save sent before its game loads a slot carries the race id the load keeps") {
    // Lane run (race-persist-3): both games start at the title. The host sends its newest slot (the slot played not
    // known yet), the guest loads it, then the host loads that slot. The id the guest got must not change at the load,
    // or the guest's loaded save is not its race any more and nothing is kept.
    const fs::path dir = FreshDir("coop_tests_race_sent");
    CoopGoldMode::ResetForTest();
    CoopGoldMode::SetRaceDir(dir);
    CoopGoldMode::SetOwnKey(kHostKey);
    CoopGoldMode::SetChoice(Mode::own);
    CoopGoldMode::OnWelcomed(true);
    CoopGoldMode::MarkRaceSent(CoopGoldMode::RaceId());

    CoopGoldMode::Playthroughs host(dir);
    const std::string sent = host.OnSend("slot_02", "");
    REQUIRE_FALSE(sent.empty());
    CoopGoldMode::SetPlaythrough(sent);
    const std::string id = CoopGoldMode::RaceId();
    CHECK(id == CoopGoldMode::RaceIdOf(kHostKey, sent));
    CHECK(CoopGoldMode::RaceSendDue());   // the save waits for this id
    CoopGoldMode::MarkRaceSent(id);

    // The host's game loads the slot it sent: the same id, nothing new to send.
    CoopGoldMode::SetPlaythrough(host.OnSlot("slot_02", false));
    CHECK(CoopGoldMode::RaceId() == id);
    CHECK_FALSE(CoopGoldMode::RaceSendDue());
    // Its autosave goes on into the next slot: still the same playthrough.
    CoopGoldMode::SetPlaythrough(host.OnSlot("slot_03", true));
    CHECK(CoopGoldMode::RaceId() == id);

    // Both games started again: the newest slot sent at the title is the same playthrough, so the same race.
    CoopGoldMode::Playthroughs again(dir);
    CHECK(again.OnSend("slot_03", "") == sent);

    // The host loads another slot after the send: another playthrough, its id goes out before the save that follows.
    CoopGoldMode::SetPlaythrough(host.OnSlot("slot_01", false));
    CHECK(CoopGoldMode::RaceId() != id);
    CHECK(CoopGoldMode::RaceSendDue());
    CoopGoldMode::ResetForTest();
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("gold mode: Not shared, the Skulltula House's rewards and what they give are in each racer's race (review-01112-h)") {
    constexpr std::uint32_t kReward10 = 0x9086a22e;  // KakarikoVillage_GatheringTokenGold_GetReward10 (a Bool)
    constexpr std::uint32_t kWallet = 0x1d74617e;    // PouchData.Item_Wallet_Small.StockNum (an Int)
    const std::uint32_t flags[] = {kReward10, kWallet};

    // Read from the game with the race, kept as text, joined by the higher value.
    FakeGame game = MakeGame(10, true);
    game.Named(kReward10, proto::FlagKind::boolean, 1);
    game.Named(kWallet, proto::FlagKind::int32, 1);
    const auto seen = CoopGoldMode::ReadRace(game, kSkull, kCount, kGet, flags);
    REQUIRE(seen.has_value());
    CHECK(seen->flags == std::map<std::uint32_t, std::uint32_t>{{kReward10, 1}, {kWallet, 1}});
    CHECK_FALSE(seen->empty());
    const auto back = CoopGoldMode::ParseRace(CoopGoldMode::RaceText(*seen));
    REQUIRE(back.has_value());
    CHECK(*back == *seen);
    CHECK(CoopGoldMode::RaceText(*seen).find("flag 9086a22e 1\n") != std::string::npos);
    for (const char* bad : {"flag 9086a22e 0", "flag 9086a22e", "flag 9086a2 1", "flag 00000000 1", "flag 9086a22e x"}) {
        INFO(bad);
        CHECK_FALSE(CoopGoldMode::ParseRace(std::string("oot-coop gold race 1\n") + bad + "\n").has_value());
    }
    CoopGoldMode::Race race = MakeRace(10, true, {});
    CHECK(race.Join(*seen));
    CoopGoldMode::Race higher;
    higher.flags = {{kWallet, 2}};
    CHECK(race.Join(higher));
    CHECK(race.flags.at(kWallet) == 2);
    CHECK_FALSE(race.Join(*seen));   // never lowered

    // Put back after a load of a save that has neither: both in again; a higher value in the game stays.
    FakeGame loaded = MakeGame(0, false);
    loaded.Named(kReward10, proto::FlagKind::boolean, 0);
    loaded.Named(kWallet, proto::FlagKind::int32, 0);
    auto put = CoopGoldMode::PutRaceBack(loaded, race, kCount, kGet);
    CHECK(put.done());
    CHECK(loaded.Value(kReward10) == 1);
    CHECK(loaded.Value(kWallet) == 2);
    FakeGame unread = MakeGame(10, true);
    CHECK(CoopGoldMode::PutRaceBack(unread, race, kCount, kGet).unread == 2);   // not loaded yet: tried again
}

TEST_CASE("gold mode: Not shared, two racers each reaching 10: the host's save carries each one's own reward (review-01112-h)") {
    // kOther stands for a race flag the host's save has (MakeSave's second Int, 77 there: the host's own).
    const std::uint32_t flags[] = {kOther, 0x22222222 /* not in this save */};
    CoopGoldMode::Race a = MakeRace(10, true, {kSkull[0]});
    a.flags = {{kOther, 1}};
    CoopGoldMode::Race b = MakeRace(10, true, {kSkull[1]});
    b.flags = {{kOther, 3}};
    for (const auto* race : {&a, &b}) {
        std::vector<std::uint8_t> host = MakeSave(25, 1, {kSkull[2], kSkull[2] + 2, kWeb});
        const auto kept = CoopGoldMode::KeepRaceIn(host, race, kSkull, kCount, kGet, flags);
        REQUIRE(kept.done);
        CHECK(kept.flags == 1);
        std::vector<std::uint8_t> want = MakeSave(10, 1, {*race->keys.begin(), kWeb});
        // the race flag's value word: the Int after kCount's pair in MakeSave
        const std::uint32_t own = race->flags.at(kOther);
        CHECK(Get32(host, 0x20 + 8 * 4 + 4 + 8) == own);   // (MakeSave: type 0 opener, kGet, type 1, type 2, kCount, kOther)
        want[0x20 + 8 * 4 + 4 + 8] = static_cast<std::uint8_t>(own);
        want[0x20 + 8 * 4 + 4 + 8 + 1] = want[0x20 + 8 * 4 + 4 + 8 + 2] = want[0x20 + 8 * 4 + 4 + 8 + 3] = 0;
        CHECK(host == want);
    }
    // No race with this host: a race from 0, the host's reward is not ours.
    std::vector<std::uint8_t> host = MakeSave(25, 1, {kWeb});
    CHECK(CoopGoldMode::KeepRaceIn(host, nullptr, kSkull, kCount, kGet, flags).done);
    CHECK(Get32(host, 0x20 + 8 * 4 + 4 + 8) == 0);
}

TEST_CASE("gold mode: a slot sent in place of a known played slot leaves the host's playthrough as it is") {
    // Review of ba791a54: the host's game plays slot_01 (known), but its save cannot be sent (its progress.sav is
    // gone), so the newest slot_03 goes out. The host still plays slot_01's playthrough: its race id stays.
    const fs::path dir = FreshDir("coop_tests_race_sent_known");
    CoopGoldMode::Playthroughs host(dir);
    const std::string played = host.OnSlot("slot_01", false);
    REQUIRE_FALSE(played.empty());
    CHECK(host.OnSend("slot_03", "slot_01").empty());   // nothing new in force
    CHECK(host.Current() == played);
    CHECK(host.Slots().count("slot_03") == 0);   // and no id made up for the slot sent
    // The slot played not known (the title): the sent slot's playthrough is in force, as before.
    CoopGoldMode::Playthroughs title(dir);
    const std::string sent = title.OnSend("slot_03", "");
    REQUIRE_FALSE(sent.empty());
    CHECK(title.Current() == sent);
    CHECK(sent != played);
    std::error_code ec;
    fs::remove_all(dir, ec);
}
