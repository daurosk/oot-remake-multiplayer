// Tests for PvP on real bodies (lichii/hook/coop_pvp_native.h,
// docs/native-puppets.md section 5).
#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <ostream>

#include "coop_pvp_native.h"

namespace proto = coop::protocol;
using namespace CoopPvpNative;

namespace {

constexpr std::uint64_t kNow = 10'000'000;
constexpr std::uint64_t kSeen = kNow - 150'000;

// We are player 2 standing at the origin; player 1 stands 1 m away on x.
struct Scene {
    PlaceRing own;
    PlaceRing attacker;
    StrikeView view;
    Scene() {
        for (std::uint64_t t = kNow - 900'000; t <= kNow; t += 16'000) own.Push(t, Vec3{0, 0, 0});
        for (std::uint64_t t = kNow - 900'000; t <= kNow; t += 50'000) attacker.Push(t, Vec3{1, 0, 0});
        view.local.self = 2;
        view.local.scene = "spot00";
        view.local.life = 48;
        view.attacker.scene = "spot00";
        view.attacker.known = true;
        view.own = &own;
        view.attacker_ring = &attacker;
    }
};

proto::PvpStrike Strike(std::uint32_t seq = 1) {
    proto::PvpStrike s;
    s.player_number = 1;
    s.victim = 2;
    s.seq = seq;
    s.seen_us = kSeen;
    s.kind[0] = 7;
    s.kind[1] = 1;
    s.amount = 80;  // the attacker's word: never used
    s.at = {0.3f, 1.0f, 0};
    s.copy_at = {0, 0, 0};
    s.from = {1, 0, 0};
    return s;
}

Tables KokiriTables() {
    Tables t;
    t.base_attack[11] = 1;  // the Kokiri Sword
    t.base_attack[12] = 2;  // the Master Sword
    t.multiplier[(std::uint64_t{7} << 32) | 1] = 1.0f;  // a plain slash
    t.multiplier[(std::uint64_t{7} << 32) | 2] = 2.0f;  // a spin
    return t;
}

}  // namespace

TEST_CASE("pvp native: the _Pvp copy only when PvP is on, the glue on, a pvp pack and a native peer that is up") {
    CopyMode m{true, true, true, true};
    CHECK(m.peer_up);  // up unless DBNO says otherwise
    CHECK(WantedCopy(m) == CopyActor::pvp);
    for (int off = 0; off < 5; ++off) {
        CopyMode n = m;
        (off == 0 ? n.pvp_on : off == 1 ? n.glue_on : off == 2 ? n.pack_pvp : off == 3 ? n.peer_native : n.peer_up) =
            false;
        CHECK(WantedCopy(n) == CopyActor::plain);
    }
    CHECK(WantedCopy(CopyMode{}) == CopyActor::plain);  // PvP off by default: the plain copy
    CHECK(SayNative(true, true));
    CHECK_FALSE(SayNative(true, false));  // an old pack: PVP_NATIVE 0, today's PvP
    CHECK_FALSE(SayNative(false, true));
}

TEST_CASE("pvp native: a copy that is lifted or holding a bomb is not re-made until that ends") {
    CHECK(RemakeNow(CopyActor::plain, CopyActor::pvp, false));
    CHECK(RemakeNow(CopyActor::pvp, CopyActor::plain, false));
    CHECK_FALSE(RemakeNow(CopyActor::plain, CopyActor::pvp, true));
    CHECK_FALSE(RemakeNow(CopyActor::pvp, CopyActor::plain, true));
    CHECK_FALSE(RemakeNow(CopyActor::pvp, CopyActor::pvp, false));
}

TEST_CASE("pvp native: only our Link's hits and our own shots or blasts are sent") {
    CHECK(StrikeSource(RecordFrom{true, std::nullopt, false}, 1) == proto::PvpSource::sword);
    const proto::ObjectId ours{proto::ObjectKind::made, 1, 5};
    const proto::ObjectId theirs{proto::ObjectKind::made, 3, 5};
    const proto::ObjectId placed{proto::ObjectKind::placed, 0, 5};
    CHECK(StrikeSource(RecordFrom{false, ours, false}, 1) == proto::PvpSource::shot);
    CHECK(StrikeSource(RecordFrom{false, ours, true}, 1) == proto::PvpSource::blast);
    CHECK_FALSE(StrikeSource(RecordFrom{false, theirs, false}, 1));
    CHECK_FALSE(StrikeSource(RecordFrom{false, placed, false}, 1));
    CHECK_FALSE(StrikeSource(RecordFrom{}, 1));  // an enemy's hit on a copy
    CHECK_FALSE(StrikeSource(RecordFrom{false, ours, false}, 0));
}

TEST_CASE("pvp native: the place ring keeps a second and answers the nearest place") {
    PlaceRing r;
    r.Push(1'000'000, Vec3{1, 0, 0});
    r.Push(1'500'000, Vec3{2, 0, 0});
    r.Push(2'100'000, Vec3{3, 0, 0});  // the first is now older than a second
    CHECK_FALSE(r.At(1'000'000, 100'000));
    CHECK(r.At(1'450'000, 100'000) == Vec3{2, 0, 0});
    CHECK_FALSE(r.At(1'800'000, 100'000));
    // A clock that steps back starts the ring again.
    r.Push(500'000, Vec3{9, 0, 0});
    CHECK(r.At(500'000, 0) == Vec3{9, 0, 0});
    CHECK_FALSE(r.At(2'100'000, 100'000));
}

TEST_CASE("pvp native: a fair strike passes the checks") {
    Scene sc;
    CHECK_FALSE(Refusal(Strike(), sc.view, kNow));
}

TEST_CASE("pvp native: forged or laggy strikes are refused") {
    Scene sc;
    SUBCASE("seen_us in the future or too old: late, never wrapped") {
        auto s = Strike();
        s.seen_us = kNow + 1;
        CHECK(Refusal(s, sc.view, kNow) == proto::PvpOutcome::late);
        s.seen_us = kNow - kStrikeLateUs - 1;
        CHECK(Refusal(s, sc.view, kNow) == proto::PvpOutcome::late);
        s.seen_us = 0;
        CHECK(Refusal(s, sc.view, kNow) == proto::PvpOutcome::late);
    }
    SUBCASE("the hit place off our body at seen_us") {
        auto s = Strike();
        s.at = {2.0f, 1.0f, 0};
        CHECK(Refusal(s, sc.view, kNow) == proto::PvpOutcome::out_of_reach);
        s.at = {0, 3.0f, 0};
        CHECK(Refusal(s, sc.view, kNow) == proto::PvpOutcome::out_of_reach);
    }
    SUBCASE("the attacker far from where we saw it") {
        auto s = Strike();
        s.from = {1.6f, 0, 0};
        CHECK(Refusal(s, sc.view, kNow) == proto::PvpOutcome::out_of_reach);
    }
    SUBCASE("a sword past its reach: where we saw the attacker, not where it says") {
        sc.attacker.Clear();
        for (std::uint64_t t = kNow - 900'000; t <= kNow; t += 50'000) sc.attacker.Push(t, Vec3{3, 0, 0});
        auto s = Strike();
        s.from = {3, 0, 0};
        CHECK(Refusal(s, sc.view, kNow) == proto::PvpOutcome::out_of_reach);
        sc.view.weapon_range = 2.8f;  // a long weapon's Range from the game's data
        CHECK_FALSE(Refusal(s, sc.view, kNow));
        s.source = proto::PvpSource::other;  // the body: no weapon's reach
        CHECK(Refusal(s, sc.view, kNow) == proto::PvpOutcome::out_of_reach);
    }
    SUBCASE("a short Range never goes under 1.3 m") {
        sc.view.weapon_range = 0.2f;
        CHECK_FALSE(Refusal(Strike(), sc.view, kNow));
        sc.view.weapon_range = std::numeric_limits<float>::quiet_NaN();
        CHECK_FALSE(Refusal(Strike(), sc.view, kNow));
    }
    SUBCASE("a shot naming no object we have, or far from it") {
        auto s = Strike();
        s.source = proto::PvpSource::shot;
        s.object = {proto::ObjectKind::made, 1, 4};
        CHECK(Refusal(s, sc.view, kNow) == proto::PvpOutcome::out_of_reach);
        sc.view.object_at = Vec3{0, 1.0f, 2.5f};
        CHECK(Refusal(s, sc.view, kNow) == proto::PvpOutcome::out_of_reach);
        s.source = proto::PvpSource::blast;  // a blast reaches 3 m
        CHECK_FALSE(Refusal(s, sc.view, kNow));
        s.source = proto::PvpSource::shot;
        sc.view.object_at = Vec3{0.5f, 1.0f, 0};
        CHECK_FALSE(Refusal(s, sc.view, kNow));
    }
    SUBCASE("not finite places") {
        auto s = Strike();
        s.from[0] = std::numeric_limits<float>::infinity();
        CHECK(Refusal(s, sc.view, kNow) == proto::PvpOutcome::out_of_reach);
    }
    SUBCASE("no ring place near seen_us") {
        sc.own.Clear();
        sc.own.Push(kNow, Vec3{0, 0, 0});
        CHECK(Refusal(Strike(), sc.view, kNow) == proto::PvpOutcome::out_of_reach);
    }
}

TEST_CASE("pvp native: kept off as today, and while downed") {
    Scene sc;
    SUBCASE("downed") { sc.view.downed = true; }
    SUBCASE("an event") { sc.view.local.event = true; }
    SUBCASE("life not read") { sc.view.local.life.reset(); }
    SUBCASE("dead") { sc.view.local.life = 0; }
    SUBCASE("another scene") { sc.view.attacker.scene = "spot01"; }
    SUBCASE("the attacker not known") { sc.view.attacker.known = false; }
    SUBCASE("no rings") { sc.view.attacker_ring = nullptr; }
    CHECK(Refusal(Strike(), sc.view, kNow) == proto::PvpOutcome::kept_off);
}

TEST_CASE("pvp native: an attacker in the other era's scene of our place is not kept off (row 221)") {
    Scene sc;
    sc.view.local.scene = "Graveyard_Child";
    sc.view.attacker.scene = "Graveyard_Adult";
    CHECK_FALSE(Refusal(Strike(), sc.view, kNow));
    sc.view.attacker.scene = "Kakariko_Adult";  // another place: kept off as before
    CHECK(Refusal(Strike(), sc.view, kNow) == proto::PvpOutcome::kept_off);
}

TEST_CASE("pvp native: a repeated or old seq is dropped, and an attacker who comes back starts again") {
    SeqGate g;
    CHECK(g.Take(1, 1));
    CHECK(g.Take(1, 2));
    CHECK_FALSE(g.Take(1, 2));
    CHECK_FALSE(g.Take(1, 1));
    CHECK(g.Take(3, 1));  // per attacker
    g.Forget(1);          // it left and joined again: its game counts from 1
    CHECK(g.Take(1, 1));
}

TEST_CASE("pvp native: path B's amount is the victim's own, never the attacker 80") {
    const Tables t = KokiriTables();
    proto::PvpKind slash{};
    slash[0] = 7;
    slash[1] = 1;
    proto::PvpKind spin = slash;
    spin[1] = 2;
    CHECK(PathBAmount(t, 11, slash) == 1);
    CHECK(PathBAmount(t, 12, slash) == 2);
    CHECK(PathBAmount(t, 12, spin) == 4);
    proto::PvpKind unknown = slash;
    unknown[1] = 9;
    CHECK_FALSE(PathBAmount(t, 11, unknown));  // a kind the tables do not name
    CHECK_FALSE(PathBAmount(t, 99, slash));    // a weapon they do not name

    Scene sc;
    SeqGate seq;
    Mercy mercy;
    const auto d = DecidePathB(Strike(), true, sc.view, PathBInput{11}, t, seq, mercy, kNow);
    REQUIRE(d);
    CHECK(d->result.outcome == proto::PvpOutcome::taken);
    CHECK(d->take == 1);
    CHECK(d->result.amount == 1);
    CHECK(d->result.life == 47);
    CHECK(d->replay);
    CHECK(d->result.attacker == 1);
    CHECK(d->result.seq == 1);
}

TEST_CASE("pvp native: path B guards, guard breaks, mercy and drops") {
    const Tables t = KokiriTables();
    Scene sc;
    SeqGate seq;
    Mercy mercy;
    // Guarded: no life falls, the game's guard push plays.
    auto d = DecidePathB(Strike(1), true, sc.view, PathBInput{11, true, false}, t, seq, mercy, kNow);
    REQUIRE(d);
    CHECK(d->result.outcome == proto::PvpOutcome::guarded);
    CHECK(d->take == 0);
    CHECK_FALSE(d->replay);
    // A guard that fails: guard broken, taken.
    d = DecidePathB(Strike(2), true, sc.view, PathBInput{12, true, true}, t, seq, mercy, kNow);
    REQUIRE(d);
    CHECK(d->result.outcome == proto::PvpOutcome::guard_broken);
    CHECK(d->take == 2);
    // Within the mercy time: invincible.
    d = DecidePathB(Strike(3), true, sc.view, PathBInput{11}, t, seq, mercy, kNow + 100'000);
    REQUIRE(d);
    CHECK(d->result.outcome == proto::PvpOutcome::invincible);
    // The same seq again: dropped, no result.
    CHECK_FALSE(DecidePathB(Strike(3), true, sc.view, PathBInput{11}, t, seq, mercy, kNow + 900'000));
    // PvP off, our own, another victim's: dropped.
    CHECK_FALSE(DecidePathB(Strike(4), false, sc.view, PathBInput{11}, t, seq, mercy, kNow));
    auto other = Strike(5);
    other.victim = 3;
    CHECK_FALSE(DecidePathB(other, true, sc.view, PathBInput{11}, t, seq, mercy, kNow));
    auto own = Strike(6);
    own.player_number = 2;
    CHECK_FALSE(DecidePathB(own, true, sc.view, PathBInput{11}, t, seq, mercy, kNow));
    // A late strike still answers (late) and spends its seq.
    auto late = Strike(7);
    late.seen_us = kNow - 1'000'000;
    d = DecidePathB(late, true, sc.view, PathBInput{11}, t, seq, mercy, kNow);
    REQUIRE(d);
    CHECK(d->result.outcome == proto::PvpOutcome::late);
    CHECK_FALSE(DecidePathB(late, true, sc.view, PathBInput{11}, t, seq, mercy, kNow));
    // A kind our tables do not name: kept off.
    Scene fresh;
    SeqGate seq2;
    Mercy mercy2;
    auto odd = Strike(8);
    odd.kind[1] = 9;
    d = DecidePathB(odd, true, fresh.view, PathBInput{11}, t, seq2, mercy2, kNow);
    REQUIRE(d);
    CHECK(d->result.outcome == proto::PvpOutcome::kept_off);
    CHECK(d->take == 0);
}

TEST_CASE("pvp native: life never goes under zero on path B") {
    Tables t = KokiriTables();
    t.base_attack[13] = 4;  // Biggoron's
    Scene sc;
    sc.view.local.life = 3;
    SeqGate seq;
    Mercy mercy;
    proto::PvpStrike s = Strike();
    s.kind[1] = 2;  // a spin: 8
    const auto d = DecidePathB(s, true, sc.view, PathBInput{13}, t, seq, mercy, kNow);
    REQUIRE(d);
    CHECK(d->take == 3);
    CHECK(d->result.life == 0);
}

TEST_CASE("pvp native: the _Pvp actor name, and a manifest that has them") {
    CHECK(PvpActor("CoopPuppet_Child") == "CoopPuppet_Child_Pvp");
    CHECK(PvpActor("CoopPuppet_AdultQuiet_Red") == "CoopPuppet_AdultQuiet_Red_Pvp");
    CHECK(PvpActor("CoopPuppet_Child_Pvp") == "CoopPuppet_Child_Pvp");
    CHECK(PvpActor("Player_Child") == "Player_Child");  // a real Player: no copy of ours
    CHECK(IsPvpActor("CoopPuppet_Child_Pvp"));
    CHECK_FALSE(IsPvpActor("CoopPuppet_Child"));
    CHECK(OnlyPvpDiffers("CoopPuppet_Child", "CoopPuppet_Child_Pvp"));
    CHECK(OnlyPvpDiffers("CoopPuppet_Child_Pvp", "CoopPuppet_Child"));
    CHECK_FALSE(OnlyPvpDiffers("CoopPuppet_Child", "CoopPuppet_Adult_Pvp"));
    CHECK_FALSE(OnlyPvpDiffers("CoopPuppet_Child", "CoopPuppet_Child"));
    CHECK(PackHasPvp(R"({"actors": [{"actor": "A", "pvp": {"x": 1}}], "pvp": true})"));
    CHECK(PackHasPvp(R"({"pvp" : true})"));
    CHECK_FALSE(PackHasPvp(R"({"actors": [{"actor": "A", "pvp": {"x": 1}}]})"));
    CHECK_FALSE(PackHasPvp(R"({"liftable": true})"));
    CHECK_FALSE(PackHasPvp(""));
}

TEST_CASE("pvp native: the weapon from PLAYER_EQUIP's name, and the first tables") {
    CHECK(WeaponOf("Weapon_Sword_Kokiri") == kWeaponKokiri);
    CHECK(WeaponOf("Weapon_Sword_Kokiri_Alb") == kWeaponKokiri);
    CHECK(WeaponOf("Weapon_Sword_Master") == kWeaponMaster);
    CHECK(WeaponOf("Weapon_Sword_Biggoron_Alb") == kWeaponBiggoron);
    CHECK(WeaponOf("Weapon_Sword_Giant") == kWeaponGiant);
    CHECK(WeaponOf("Weapon_Sword_BrokenGiant") == kWeaponBroken);
    CHECK(WeaponOf("Weapon_Sword_BrokenBiggoron") == kWeaponBroken);
    CHECK(WeaponOf("Weapon_Stick") == kWeaponStick);
    CHECK(WeaponOf("Weapon_Hammer") == kWeaponHammer);
    CHECK(WeaponOf("Shield_Deku") == kWeaponNone);
    CHECK(WeaponOf("") == kWeaponNone);
    const Tables t = FirstTables();
    CHECK(t.base_attack.at(kWeaponKokiri) == 1);
    CHECK(t.base_attack.at(kWeaponMaster) == 2);
    CHECK(t.base_attack.at(kWeaponBiggoron) == 4);
    CHECK(t.base_attack.at(kWeaponGiant) == 4);
    CHECK(t.base_attack.at(kWeaponHammer) == 3);
    CHECK(t.base_attack.at(kWeaponStick) == 2);
    CHECK(t.base_attack.count(kWeaponBroken) == 0);
    // No kind is named until U2/U7: path B is not live, every strike is kept off and takes nothing.
    CHECK_FALSE(PathBLive(t));
    CHECK(PathBLive(KokiriTables()));
    Scene sc;
    SeqGate seq;
    Mercy mercy;
    PathBInput in;
    in.weapon = kWeaponKokiri;
    const auto d = DecidePathB(Strike(), true, sc.view, in, t, seq, mercy, kNow);
    REQUIRE(d);
    CHECK(d->result.outcome == proto::PvpOutcome::kept_off);
    CHECK(d->take == 0);
    CHECK_FALSE(d->replay);
}

TEST_CASE("pvp native: guarding is a guard animation facing the attacker") {
    const proto::Quat ahead{0, 0, 0, 1};                     // facing +z
    const proto::Quat back{0, 1, 0, 0};                      // turned 180 degrees about y: facing -z
    const float s = std::sqrt(0.5f);
    const proto::Quat right{0, s, 0, s};                     // 90 degrees about y: facing +x
    const Vec3 self{0, 0, 0};
    CHECK(GuardAnim("Act_Guard_Wait"));
    CHECK(GuardAnim("reactionGUARD"));
    CHECK_FALSE(GuardAnim("Act_Walk"));
    CHECK(FacesToward(ahead, self, Vec3{0, 0, 2}));
    CHECK(FacesToward(ahead, self, Vec3{1, 0, 2}));          // 27 degrees off
    CHECK_FALSE(FacesToward(ahead, self, Vec3{2, 0, 0.5f})); // 76 degrees off
    CHECK_FALSE(FacesToward(back, self, Vec3{0, 0, 2}));
    CHECK(FacesToward(right, self, Vec3{2, 0, 0}));
    CHECK_FALSE(FacesToward(ahead, self, self));              // on top of us: no direction
    const std::optional<std::string_view> guard{"Act_Guard_Wait"};
    CHECK(Guarding(guard, ahead, self, Vec3{0, 0, 2}));
    CHECK_FALSE(Guarding(guard, back, self, Vec3{0, 0, 2}));
    CHECK_FALSE(Guarding(std::optional<std::string_view>{"Act_Run"}, ahead, self, Vec3{0, 0, 2}));
    CHECK_FALSE(Guarding(std::nullopt, ahead, self, Vec3{0, 0, 2}));
    CHECK_FALSE(Guarding(guard, std::nullopt, self, Vec3{0, 0, 2}));
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(Guarding(guard, proto::Quat{nan, 0, 0, 1}, self, Vec3{0, 0, 2}));
}

TEST_CASE("pvp native: a copy's hit id is a strike only when it moved, on the same actor, not to our replay") {
    CopyHitWatch w;
    CHECK_FALSE(w.Moved(2, 0x100, 5, 0xffffffffu));  // first sight: no strike
    CHECK_FALSE(w.Moved(2, 0x100, 5, 0xffffffffu));  // unchanged
    CHECK(w.Moved(2, 0x100, 7, 0xffffffffu));        // our game's own hit
    CHECK_FALSE(w.Moved(2, 0x100, 9, 9));            // our own third-game replay on it
    CHECK_FALSE(w.Moved(2, 0x200, 11, 0xffffffffu)); // a re-made copy: starts again
    CHECK(w.Moved(2, 0x200, 13, 0xffffffffu));
    CHECK_FALSE(w.Moved(3, 0x300, 1, 0xffffffffu));  // each player its own
    w.Forget(2);
    CHECK_FALSE(w.Moved(2, 0x200, 15, 0xffffffffu)); // forgotten: first sight again
    w.Reset();
    CHECK_FALSE(w.Moved(3, 0x300, 2, 0xffffffffu));
}

TEST_CASE("pvp native: a strike is made only from our own hit on another player's copy") {
    CopyHit hit;
    hit.victim = 2;
    hit.from.our_link = true;
    hit.kind[0] = 7;
    hit.kind[1] = 1;
    hit.amount = 4;
    hit.at = {0.3f, 1.0f, 0};
    hit.attacker_at = {1, 0, 0};
    const Vec3 copy_at{0, 0, 0};
    const auto s = MakeStrike(hit, 1, 3, kSeen, copy_at);
    REQUIRE(s);
    CHECK(s->player_number == 1);
    CHECK(s->victim == 2);
    CHECK(s->seq == 3);
    CHECK(s->seen_us == kSeen);
    CHECK(s->kind == hit.kind);
    CHECK(s->amount == 4);
    CHECK(s->source == proto::PvpSource::sword);
    CHECK(s->object == proto::ObjectId{});
    CHECK(s->at == hit.at);
    CHECK(s->copy_at == copy_at);
    CHECK(s->from == hit.attacker_at);
    // The victim's rules take what we make (the Scene: we are P1 1 m away, P2 at the origin).
    Scene sc;
    SeqGate seq;
    Mercy mercy;
    PathBInput in;
    in.weapon = 11;
    const auto d = DecidePathB(*s, true, sc.view, in, KokiriTables(), seq, mercy, kNow);
    REQUIRE(d);
    CHECK(d->result.outcome == proto::PvpOutcome::taken);
    CHECK(d->take == 1);  // the victim's own count, never our 4

    CopyHit enemy = hit;
    enemy.from.our_link = false;  // an enemy's hit on the copy
    CHECK_FALSE(MakeStrike(enemy, 1, 4, kSeen, copy_at));
    CopyHit shot = enemy;
    shot.own_shot = true;  // ours, but its made object is not named: the victim could not check it
    CHECK_FALSE(MakeStrike(shot, 1, 4, kSeen, copy_at));
    CopyHit named = enemy;
    named.from.made = proto::ObjectId{proto::ObjectKind::made, 1, 9};
    const auto ns = MakeStrike(named, 1, 4, kSeen, copy_at);
    REQUIRE(ns);
    CHECK(ns->source == proto::PvpSource::shot);
    CHECK(ns->object == *named.from.made);
    CHECK_FALSE(MakeStrike(hit, 2, 4, kSeen, copy_at));  // our own copy? never
    CHECK_FALSE(MakeStrike(hit, 1, 4, 0, copy_at));      // no sample shown: no seen time
    CopyHit far = hit;
    far.victim = proto::kMaxPlayers + 1;
    CHECK_FALSE(MakeStrike(far, 1, 4, kSeen, copy_at));
    CopyHit bad = hit;
    bad.at[1] = std::numeric_limits<float>::infinity();
    CHECK_FALSE(MakeStrike(bad, 1, 4, kSeen, copy_at));
    CopyHit loud = hit;
    loud.amount = 1000;
    CHECK(MakeStrike(loud, 1, 4, kSeen, copy_at)->amount == proto::kMaxPvpLife);
}

TEST_CASE("pvp native: what a game that is not the victim does with a result") {
    proto::PvpResult r;
    r.player_number = 2;  // the victim
    r.attacker = 1;
    r.outcome = proto::PvpOutcome::taken;
    CHECK(PlayResult(r, 3) == ResultPlay::replay_on_copy);  // a third game
    CHECK(PlayResult(r, 1) == ResultPlay::none);            // the attacker saw its own hit
    CHECK(PlayResult(r, 2) == ResultPlay::none);            // the victim's own (the server never sends it back)
    CHECK(PlayResult(r, 0) == ResultPlay::none);
    r.outcome = proto::PvpOutcome::guard_broken;
    CHECK(PlayResult(r, 3) == ResultPlay::replay_on_copy);
    r.outcome = proto::PvpOutcome::guarded;
    CHECK(PlayResult(r, 1) == ResultPlay::attacker_recoil);
    CHECK(PlayResult(r, 3) == ResultPlay::none);
    for (auto o : {proto::PvpOutcome::invincible, proto::PvpOutcome::late, proto::PvpOutcome::out_of_reach,
                   proto::PvpOutcome::kept_off}) {
        r.outcome = o;
        CHECK(PlayResult(r, 3) == ResultPlay::none);
        CHECK(PlayResult(r, 1) == ResultPlay::none);
    }
}

TEST_CASE("pvp native: each live _Pvp copy's Life is made invincible once (review M1)") {
    CopyShield s;
    using C = CopyShield::Copy;
    const C a{0x100, 0x1100};
    const C b{0x200, 0x1200};
    CHECK_FALSE(s.Next({}));
    REQUIRE(s.Next({a, b}));
    CHECK(*s.Next({a, b}) == a);  // the first not yet shielded
    s.Called(a, true);
    CHECK(s.Shielded(a));
    CHECK(*s.Next({a, b}) == b);  // a is not asked again
    s.Called(b, true);
    CHECK_FALSE(s.Next({a, b}));  // all shielded: no call
    // a goes (a re-make, a load): forgotten with its component; a new copy at the same Life is shielded again.
    CHECK_FALSE(s.Next({b}));
    CHECK_FALSE(s.Shielded(a));
    const C a2{0x300, 0x1100};
    CHECK(*s.Next({a2, b}) == a2);
    // A call that fails is tried kCopyShieldTries times, then given up (logged by the caller).
    for (unsigned i = 0; i < kCopyShieldTries; ++i) {
        REQUIRE(s.Next({a2, b}));
        s.Called(a2, false);
    }
    CHECK(s.GivenUp(a2));
    CHECK_FALSE(s.Next({a2, b}));
    // Zero actors or Life components are never asked.
    CHECK_FALSE(s.Next({C{0, 0x1}, C{0x1, 0}, b}));
    s.Reset();
    CHECK(*s.Next({b}) == b);
    // The key and the function: ours, not the downed Link's, on the reacting list (IsNoReaction 0).
    CHECK(kCopyShieldKey != 0xC0DB0D01u);
    CHECK(kInvincibleReacting == 0xb34800);
}
