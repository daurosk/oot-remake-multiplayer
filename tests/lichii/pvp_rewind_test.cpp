// Tests for PvP on real bodies: the rewind check and who decides (lichii/hook/coop_pvp_native.h,
// docs/native-puppets.md 5.6), case by case on one clock: lag, jitter, replays, a shield raised just before,
// both hit at once, death during the hit, pause / cutscene / menu, a room change mid-swing, a cheating client.
#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <ostream>

#include "coop_pvp_native.h"

namespace proto = coop::protocol;
using namespace CoopPvpNative;

namespace {

// A duel as the two games live it. We are player 2 (the victim); player 1 swings at kHit. One trip between
// the two games (both ways to the server) takes `trip_us`; the attacker draws our copy `show_us` late. So its
// copy showed our place of `kHit - trip_us - show_us` (the strike's seen_us), and the strike reaches us at
// `kHit + trip_us`, when we have its samples up to kHit.
struct Duel {
    static constexpr std::uint64_t kStart = 9'000'000;
    static constexpr std::uint64_t kHit = 10'000'000;
    std::uint64_t trip_us = 100'000;
    std::uint64_t show_us = 100'000;
    Vec3 victim_from{0, 0, 0};    // at kStart
    Vec3 victim_speed{0, 0, 0};   // m/s
    Vec3 attacker_from{1, 0, 0};
    Vec3 attacker_speed{0, 0, 0};
    std::uint64_t frame_us = 16'000;   // our own ring: a place each frame
    std::uint64_t sample_us = 50'000;  // the attacker's samples
    int lost_newest = 0;               // that many of the attacker's newest samples never came

    PlaceRing own;
    PlaceRing attacker;
    StrikeView view;

    static Vec3 Along(const Vec3& from, const Vec3& speed, std::uint64_t t_us) {
        const float s = static_cast<float>(static_cast<double>(t_us - kStart) / 1e6);
        return Vec3{from[0] + speed[0] * s, from[1] + speed[1] * s, from[2] + speed[2] * s};
    }
    Vec3 VictimAt(std::uint64_t t_us) const { return Along(victim_from, victim_speed, t_us); }
    Vec3 AttackerAt(std::uint64_t t_us) const { return Along(attacker_from, attacker_speed, t_us); }
    std::uint64_t Seen() const { return kHit - trip_us - show_us; }
    std::uint64_t Now() const { return kHit + trip_us; }

    // The strike the attacker's game sends, and our game's rings when it arrives.
    proto::PvpStrike Swing(std::uint32_t seq = 1) {
        own.Clear();
        attacker.Clear();
        for (std::uint64_t t = kStart; t <= Now(); t += frame_us) own.Push(t, VictimAt(t));
        const std::uint64_t newest = kHit - static_cast<std::uint64_t>(lost_newest) * sample_us;
        for (std::uint64_t t = kStart; t <= newest; t += sample_us) attacker.Push(t, AttackerAt(t));
        view.local.self = 2;
        view.local.scene = "spot00";
        view.local.life = 48;
        view.attacker.scene = "spot00";
        view.attacker.known = true;
        view.own = &own;
        view.attacker_ring = &attacker;
        proto::PvpStrike s;
        s.player_number = 1;
        s.victim = 2;
        s.seq = seq;
        s.seen_us = Seen();
        s.kind[0] = 7;
        s.kind[1] = 1;
        s.amount = 80;  // the attacker's word: never used
        const Vec3 copy = VictimAt(Seen());
        s.at = {copy[0], copy[1] + 1.0f, copy[2]};
        s.copy_at = copy;
        s.from = AttackerAt(kHit);
        return s;
    }
};

Tables KokiriTables() {
    Tables t;
    t.base_attack[11] = 1;  // the Kokiri Sword
    t.multiplier[(std::uint64_t{7} << 32) | 1] = 1.0f;  // a plain slash
    t.multiplier[(std::uint64_t{7} << 32) | 2] = 2.0f;  // a spin
    return t;
}

PathBInput Kokiri() {
    PathBInput in;
    in.weapon = 11;
    return in;
}

// The rotation that turns the game's forward (+z) to +x: a quarter turn about y. And none: facing +z.
constexpr proto::Quat kFacesX{0.0f, 0.70710678f, 0.0f, 0.70710678f};
constexpr proto::Quat kFacesZ{0.0f, 0.0f, 0.0f, 1.0f};

}  // namespace

TEST_CASE("pvp rewind: lag: fair up to 200 ms pings, late past them") {
    for (const std::uint64_t trip : {std::uint64_t{20'000}, std::uint64_t{100'000}, std::uint64_t{150'000},
                                     std::uint64_t{200'000}}) {
        CAPTURE(trip);
        {
            Duel d;  // both stand
            d.trip_us = trip;
            const auto s = d.Swing();
            CHECK_FALSE(Refusal(s, d.view, d.Now()));
        }
        {
            Duel d;  // the victim walks and the attacker follows beside it
            d.trip_us = trip;
            d.victim_speed = {0, 0, 4.0f};
            d.attacker_speed = {0, 0, 4.0f};
            d.attacker_from = {1.0f, 0, -4.0f * static_cast<float>(trip + d.show_us) / 1e6f};  // beside its copy
            const auto s = d.Swing();
            CHECK_FALSE(Refusal(s, d.view, d.Now()));
        }
        {
            Duel d;  // the victim runs off after the swing: what the attacker saw is what it hit
            d.trip_us = trip;
            d.victim_speed = {-6.0f, 0, 0};
            d.attacker_from = {d.VictimAt(d.Seen())[0] + 1.0f, 0, 0};  // 1 m from its copy of us when it hit
            const auto s = d.Swing();
            CHECK_FALSE(Refusal(s, d.view, d.Now()));
        }
        {
            Duel d;  // a running attacker: its place when it hit, not its place at seen_us (1.4 m to 3.5 m back)
            d.trip_us = trip;
            d.attacker_speed = {-7.0f, 0, 0};
            d.attacker_from = {1.0f + 7.0f, 0, 0};  // 1 m from us at kHit, one second after kStart
            const auto s = d.Swing();
            CHECK_FALSE(Refusal(s, d.view, d.Now()));
            // Its place at seen_us alone would be out of the jitter: that is why the path since then is asked.
            const auto then = d.attacker.At(s.seen_us, kCopyRingSlackUs);
            REQUIRE(then);
            CHECK(Distance(*then, s.from) > kJitterM);
        }
    }
    // The longest fair age: two trips of 200 ms and the 100 ms display delay.
    CHECK(kStrikeLateUs >= 2 * 200'000 + 100'000);
    Duel slow;
    slow.trip_us = 250'000;
    const auto s = slow.Swing();
    CHECK(Refusal(s, slow.view, slow.Now()) == proto::PvpOutcome::late);
}

TEST_CASE("pvp rewind: jitter and lost samples") {
    Duel d;
    d.victim_speed = {0, 0, 2.0f};
    d.attacker_speed = {0, 0, 2.0f};
    d.attacker_from = {1.0f, 0, -2.0f * 0.2f};  // beside its copy of us, which is 200 ms behind us
    SUBCASE("our frames are slow against the samples") { d.frame_us = 33'000; }
    SUBCASE("the attacker's newest sample was lost") { d.lost_newest = 1; }
    SUBCASE("its two newest samples were lost") { d.lost_newest = 2; }
    SUBCASE("its samples come every 70 ms") { d.sample_us = 70'000; }
    const auto s = d.Swing();
    CHECK_FALSE(Refusal(s, d.view, d.Now()));
}

TEST_CASE("pvp rewind: a strike from where the attacker was not is out of reach") {
    Duel d;
    SUBCASE("it says it stood beside us, our samples have it 5 m away all along") {
        d.attacker_from = {6.0f, 0, 0};
        auto s = d.Swing();
        s.from = {1.0f, 0, 0};
        CHECK(Refusal(s, d.view, d.Now()) == proto::PvpOutcome::out_of_reach);
    }
    SUBCASE("it stood beside us, but before seen_us: it left since") {
        const auto s = d.Swing();
        d.attacker.Clear();
        for (std::uint64_t t = Duel::kStart; t <= Duel::kHit; t += 50'000) {
            d.attacker.Push(t, t < Duel::kStart + 300'000 ? Vec3{1, 0, 0} : Vec3{6, 0, 0});
        }
        CHECK(Refusal(s, d.view, d.Now()) == proto::PvpOutcome::out_of_reach);
    }
    SUBCASE("a warp is no path: between two far samples 300 ms apart nothing is near") {
        const auto s = d.Swing();
        d.attacker.Clear();
        d.attacker.Push(d.Seen(), Vec3{-4, 0, 0});
        d.attacker.Push(d.Seen() + 300'000, Vec3{6, 0, 0});
        CHECK(Refusal(s, d.view, d.Now()) == proto::PvpOutcome::out_of_reach);
        // The same two samples 100 ms apart are a path (a fast move), and (1, 0, 0) is on it.
        d.attacker.Clear();
        d.attacker.Push(d.Seen(), Vec3{-4, 0, 0});
        d.attacker.Push(d.Seen() + 100'000, Vec3{6, 0, 0});
        CHECK_FALSE(Refusal(s, d.view, d.Now()));
    }
    SUBCASE("the ring's path: the nearest point, on a line between two samples") {
        PlaceRing r;
        r.Push(1'000'000, Vec3{0, 0, 0});
        r.Push(1'050'000, Vec3{2, 0, 0});
        const auto near_path = r.NearestSince(900'000, Vec3{1, 0, 1});
        REQUIRE(near_path);
        CHECK((*near_path)[0] == doctest::Approx(1.0f));
        CHECK((*near_path)[2] == doctest::Approx(0.0f));
        CHECK_FALSE(r.NearestSince(1'050'001, Vec3{1, 0, 1}));
        const auto last = r.NearestSince(1'050'000, Vec3{0, 0, 0});  // only the newest place is left
        REQUIRE(last);
        CHECK((*last)[0] == doctest::Approx(2.0f));
    }
}

TEST_CASE("pvp rewind: the channel is ordered: an older or repeated strike is a replay") {
    const Tables t = KokiriTables();
    Duel d;
    SeqGate seq;
    Mercy mercy;
    const auto s2 = d.Swing(2);
    const auto first = DecidePathB(s2, true, d.view, Kokiri(), t, seq, mercy, d.Now());
    REQUIRE(first);
    CHECK(first->result.outcome == proto::PvpOutcome::taken);
    CHECK(first->take == 1);
    // The same strike again (a duplicate), and an older one after it (the reliable, ordered channel 2 never
    // reorders: it is a replay): no answer, nothing taken, also after the mercy time.
    CHECK_FALSE(DecidePathB(s2, true, d.view, Kokiri(), t, seq, mercy, d.Now() + 1'000'000));
    auto s1 = s2;
    s1.seq = 1;
    CHECK_FALSE(DecidePathB(s1, true, d.view, Kokiri(), t, seq, mercy, d.Now() + 1'000'000));
}

TEST_CASE("pvp rewind: a shield raised or lowered just before") {
    Duel d;
    const auto s = d.Swing();  // we stand at the origin, the attacker at +x
    const Vec3 us{0, 0, 0};
    GuardRing ring;
    const auto fill = [&](bool guard_at_seen, bool guard_after, const proto::Quat& facing) {
        ring.Clear();
        for (std::uint64_t t = Duel::kStart; t <= d.Now(); t += 16'000) {
            const bool guard = t <= d.Seen() + 16'000 ? guard_at_seen : guard_after;
            ring.Push(t, guard, facing, us);
        }
    };
    SUBCASE("up when the attacker saw us, lowered since: guarded (its game saw the shield)") {
        fill(true, false, kFacesX);
        CHECK(ring.Toward(s.seen_us, kOwnRingSlackUs, s.from));
        CHECK(GuardsStrike(false, ring, s));
    }
    SUBCASE("raised after the swing, up when the strike comes: guarded (never through a raised shield)") {
        fill(false, true, kFacesX);
        CHECK_FALSE(ring.Toward(s.seen_us, kOwnRingSlackUs, s.from));
        CHECK(GuardsStrike(true, ring, s));
    }
    SUBCASE("up then, but facing away from the attacker: not guarded") {
        fill(true, false, kFacesZ);
        CHECK_FALSE(GuardsStrike(false, ring, s));
    }
    SUBCASE("never up: not guarded") {
        fill(false, false, kFacesX);
        CHECK_FALSE(GuardsStrike(false, ring, s));
    }
    SUBCASE("no frame near seen_us (a load since), or no rotation read: not guarded") {
        ring.Push(d.Now(), true, kFacesX, us);
        CHECK_FALSE(GuardsStrike(false, ring, s));
        ring.Clear();
        ring.Push(s.seen_us, true, std::nullopt, us);
        CHECK_FALSE(GuardsStrike(false, ring, s));
    }
    SUBCASE("a strike with no finite place guards nothing") {
        fill(true, true, kFacesX);
        auto odd = s;
        odd.from[0] = std::numeric_limits<float>::quiet_NaN();
        CHECK_FALSE(GuardsStrike(false, ring, odd));
    }
}

TEST_CASE("pvp rewind: both hit at once") {
    const Tables t = KokiriTables();
    SUBCASE("a trade: each game judges the strike on its own Link, both are taken") {
        Duel on_two;  // player 2's game judges player 1's strike
        Duel on_one;  // player 1's game judges player 2's: the same duel seen from the other side
        SeqGate seq_two, seq_one;
        Mercy mercy_two, mercy_one;
        const auto a = on_two.Swing();
        auto b = on_one.Swing();
        b.player_number = 2;
        b.victim = 1;
        on_one.view.local.self = 1;
        const auto took_two = DecidePathB(a, true, on_two.view, Kokiri(), t, seq_two, mercy_two, on_two.Now());
        const auto took_one = DecidePathB(b, true, on_one.view, Kokiri(), t, seq_one, mercy_one, on_one.Now());
        REQUIRE(took_two);
        REQUIRE(took_one);
        CHECK(took_two->result.outcome == proto::PvpOutcome::taken);
        CHECK(took_one->result.outcome == proto::PvpOutcome::taken);
    }
    SUBCASE("the attacker fell to our strike while its own was on its way: its strike still counts") {
        Duel d;
        const auto s = d.Swing();
        d.view.attacker.status = proto::status::dead;
        d.view.attacker_away_us = d.Now() - 30'000;  // the news of its fall overtook its strike
        CHECK_FALSE(Refusal(s, d.view, d.Now()));
        // Away for longer than the grace, or since an unknown time: kept off as before.
        d.view.attacker_away_us = d.Now() - kTradeGraceUs - 1;
        CHECK(Refusal(s, d.view, d.Now()) == proto::PvpOutcome::kept_off);
        d.view.attacker_away_us = 0;
        CHECK(Refusal(s, d.view, d.Now()) == proto::PvpOutcome::kept_off);
        d.view.attacker_away_us = d.Now() + 1;  // a time after now is no grace
        CHECK(Refusal(s, d.view, d.Now()) == proto::PvpOutcome::kept_off);
    }
    SUBCASE("two attackers in one frame: the first is taken, the second meets the invincible time") {
        Duel d;
        SeqGate seq;
        Mercy mercy;
        const auto from_one = d.Swing();
        auto from_three = from_one;
        from_three.player_number = 3;
        const auto first = DecidePathB(from_one, true, d.view, Kokiri(), t, seq, mercy, d.Now());
        REQUIRE(first);
        CHECK(first->take == 1);
        d.view.local.life = first->result.life;
        const auto second = DecidePathB(from_three, true, d.view, Kokiri(), t, seq, mercy, d.Now());
        REQUIRE(second);
        CHECK(second->result.outcome == proto::PvpOutcome::invincible);
        CHECK(second->take == 0);
        CHECK(second->result.life == 47);
    }
}

TEST_CASE("pvp rewind: death during the hit") {
    const Tables t = KokiriTables();
    Duel d;
    SeqGate seq;
    Mercy mercy;
    auto s = d.Swing(1);
    SUBCASE("the strike that brings us to 0 is taken once, the next ones are kept off") {
        d.view.local.life = 1;
        s.kind[1] = 2;  // a spin: 2 on the Kokiri Sword, we have 1
        const auto last = DecidePathB(s, true, d.view, Kokiri(), t, seq, mercy, d.Now());
        REQUIRE(last);
        CHECK(last->result.outcome == proto::PvpOutcome::taken);
        CHECK(last->take == 1);
        CHECK(last->result.life == 0);
        d.view.local.life = 0;
        s.seq = 2;
        auto after = DecidePathB(s, true, d.view, Kokiri(), t, seq, mercy, d.Now() + 1'000'000);
        REQUIRE(after);
        CHECK(after->result.outcome == proto::PvpOutcome::kept_off);
        CHECK(after->take == 0);
        CHECK_FALSE(after->replay);
        d.view.local.life = 12;  // revived by a teammate's fairy, still down
        d.view.downed = true;
        s.seq = 3;
        after = DecidePathB(s, true, d.view, Kokiri(), t, seq, mercy, d.Now() + 2'000'000);
        REQUIRE(after);
        CHECK(after->result.outcome == proto::PvpOutcome::kept_off);
    }
    SUBCASE("an enemy killed us between the swing and the strike") {
        d.view.local.life = 0;
        const auto late = DecidePathB(s, true, d.view, Kokiri(), t, seq, mercy, d.Now());
        REQUIRE(late);
        CHECK(late->result.outcome == proto::PvpOutcome::kept_off);
        CHECK(late->take == 0);
        CHECK(late->result.life == 0);
    }
}

TEST_CASE("pvp rewind: pause, cutscene, menu and load keep a strike off, with an answer") {
    const Tables t = KokiriTables();
    Duel d;
    SeqGate seq;
    Mercy mercy;
    const auto s = d.Swing();
    SUBCASE("we paused") { d.view.local.status = proto::status::paused; }
    SUBCASE("we are in a cutscene") { d.view.local.status = proto::status::in_cutscene; }
    SUBCASE("we opened a menu") { d.view.local.status = proto::status::in_menu; }
    SUBCASE("we are loading") { d.view.local.status = proto::status::loading; }
    SUBCASE("an event has our Link") { d.view.local.event = true; }
    SUBCASE("the attacker has been in a menu for a second") {
        d.view.attacker.status = proto::status::in_menu;
        d.view.attacker_away_us = d.Now() - 1'000'000;
    }
    const auto kept = DecidePathB(s, true, d.view, Kokiri(), t, seq, mercy, d.Now());
    REQUIRE(kept);  // the attacker is told (kept off), the seq is spent
    CHECK(kept->result.outcome == proto::PvpOutcome::kept_off);
    CHECK(kept->take == 0);
    CHECK_FALSE(kept->replay);
    CHECK(kept->result.life == 48);
    CHECK_FALSE(mercy.Refuses(d.Now()));  // nothing was taken: no invincible time starts
    CHECK_FALSE(DecidePathB(s, true, d.view, Kokiri(), t, seq, mercy, d.Now() + 1));
}

TEST_CASE("pvp rewind: a room change mid-swing") {
    Duel d;
    const auto s = d.Swing();
    SUBCASE("we left for another scene: kept off") {
        d.view.local.scene = "spot01";
        CHECK(Refusal(s, d.view, d.Now()) == proto::PvpOutcome::kept_off);
    }
    SUBCASE("the attacker left: kept off") {
        d.view.attacker.scene = "spot01";
        CHECK(Refusal(s, d.view, d.Now()) == proto::PvpOutcome::kept_off);
    }
    SUBCASE("our scene is not known (a load): kept off") {
        d.view.local.scene.reset();
        CHECK(Refusal(s, d.view, d.Now()) == proto::PvpOutcome::kept_off);
    }
    SUBCASE("we came back in: our ring starts again with the scene, the old place is no place here") {
        d.own.Clear();
        d.own.Push(d.Now(), Vec3{0, 0, 0});
        CHECK(Refusal(s, d.view, d.Now()) == proto::PvpOutcome::out_of_reach);
    }
    SUBCASE("the attacker came in from another scene: its ring starts again, no place of it yet") {
        d.attacker.Clear();
        CHECK(Refusal(s, d.view, d.Now()) == proto::PvpOutcome::out_of_reach);
    }
}

TEST_CASE("pvp rewind: a cheating client") {
    const Tables t = KokiriTables();
    Duel d;
    SeqGate seq;
    Mercy mercy;
    SUBCASE("ten strikes in one frame: one is taken, nine meet the invincible time") {
        int taken = 0;
        std::int32_t life = 48;
        for (std::uint32_t n = 1; n <= 10; ++n) {
            const auto s = d.Swing(n);
            d.view.local.life = life;
            const auto r = DecidePathB(s, true, d.view, Kokiri(), t, seq, mercy, d.Now());
            REQUIRE(r);
            if (r->result.outcome == proto::PvpOutcome::taken) ++taken;
            life = r->result.life;
        }
        CHECK(taken == 1);
        CHECK(life == 47);
    }
    SUBCASE("it picks an old seen_us to hit where we stood long ago: late") {
        auto s = d.Swing();
        s.seen_us = d.Now() - kStrikeLateUs - 1;
        const auto r = DecidePathB(s, true, d.view, Kokiri(), t, seq, mercy, d.Now());
        REQUIRE(r);
        CHECK(r->result.outcome == proto::PvpOutcome::late);
    }
    SUBCASE("it says 80 and a kind our tables do not have: our own count, or kept off") {
        auto s = d.Swing();
        auto r = DecidePathB(s, true, d.view, Kokiri(), t, seq, mercy, d.Now());
        REQUIRE(r);
        CHECK(r->take == 1);
        Mercy fresh;
        s.seq = 2;
        s.kind[0] = 0xdeadbeefu;
        r = DecidePathB(s, true, d.view, Kokiri(), t, seq, fresh, d.Now());
        REQUIRE(r);
        CHECK(r->result.outcome == proto::PvpOutcome::kept_off);
        CHECK(r->take == 0);
    }
    SUBCASE("a seq at the top locks out only its own later strikes, until it joins again") {
        auto s = d.Swing(0xffffffffu);
        REQUIRE(DecidePathB(s, true, d.view, Kokiri(), t, seq, mercy, d.Now()));
        s.seq = 1;
        CHECK_FALSE(DecidePathB(s, true, d.view, Kokiri(), t, seq, mercy, d.Now()));
        auto other = s;
        other.player_number = 3;  // another attacker is not locked out by it
        CHECK(DecidePathB(other, true, d.view, Kokiri(), t, seq, mercy, d.Now()));
        seq.Forget(1);
        CHECK(DecidePathB(s, true, d.view, Kokiri(), t, seq, mercy, d.Now()));
    }
    SUBCASE("it strikes for someone else, as us, or as nobody") {
        auto s = d.Swing();
        s.victim = 3;
        CHECK_FALSE(DecidePathB(s, true, d.view, Kokiri(), t, seq, mercy, d.Now()));
        s.victim = 2;
        s.player_number = 2;
        CHECK_FALSE(DecidePathB(s, true, d.view, Kokiri(), t, seq, mercy, d.Now()));
        s.player_number = 0;
        CHECK_FALSE(DecidePathB(s, true, d.view, Kokiri(), t, seq, mercy, d.Now()));
    }
}
