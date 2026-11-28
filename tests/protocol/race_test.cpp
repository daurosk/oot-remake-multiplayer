// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <cstddef>
#include <type_traits>
#include <utility>
#include <variant>

#include "coop/protocol/race.hpp"

namespace proto = coop::protocol;

TEST_CASE("race mode: the host's setting is read from the coop entry only") {
  CHECK_FALSE(proto::race_of({}).has_value());  // an older host: off
  CHECK_FALSE(proto::race_of({{"rando", "race", "on"}}).has_value());
  CHECK(proto::race_of({{"coop", "gold_skulltula", "own"}, {"coop", "race", "on"}}) == true);
  CHECK(proto::race_of({{"coop", "race", "off"}}) == false);
  CHECK(proto::race_of({proto::race_setting(true)}) == true);
}

TEST_CASE("race mode: world messages stay in each game, the players' own do not") {
  CHECK(proto::race_private(proto::FlagSet{}));
  CHECK(proto::race_private(proto::FlagSnapshot{}));
  CHECK(proto::race_private(proto::ObjectSample{}));
  CHECK(proto::race_private(proto::DropTake{}));
  CHECK(proto::race_private(proto::SaveAsk{}));
  CHECK(proto::race_private(proto::SwitchState{}));
  CHECK(proto::race_private(proto::JoinPartner{}));
  CHECK_FALSE(proto::race_private(proto::Chat{}));
  CHECK_FALSE(proto::race_private(proto::PlayerSample{}));
  CHECK_FALSE(proto::race_private(proto::PlayerLook{}));
  CHECK_FALSE(proto::race_private(proto::VoiceFrame{}));
  CHECK_FALSE(proto::race_private(proto::PvpHit{}));
  CHECK_FALSE(proto::race_private(proto::ModSettings{}));
  CHECK_FALSE(proto::race_private(proto::Host{}));
}

namespace {
using namespace coop::protocol;
template <typename... T>
struct List {
  static constexpr std::size_t size = sizeof...(T);
};
template <typename T, typename L>
constexpr bool kIn = false;
template <typename T, typename... U>
constexpr bool kIn<T, List<U...>> = (std::is_same_v<T, U> || ...);

// Every message type is in exactly one list. A new type fails here until it is put in one, and a
// world type also goes in race_private (coop/protocol/src/race.cpp).
using Private = List<FlagSet, FlagState, FlagSnapshot, FlagReset, FlagOrigin, SaveAsk, SavePart, ObjectPresent,
                     ObjectOwner, HandoverAsk, HandoverState, ObjectSpawn, ObjectSample, ObjectRemove, ObjectGroup,
                     Damage, BossRoom, BossScene, BossHeart, BossAlive, FishLure, WorldBreak, WorldMove, WorldPlace,
                     WorldHit, SignCut, GrassCut, DropSpawn, DropTake, DropAsk, DungeonEvent, SwitchState,
                     PlatformClock, PlatformCounter, TimeOfDay, BombTake, BombAnswer, BombShare, LiftAsk,
                     LiftAnswer, LiftPlace, LiftShare, JoinPartner>;
using Public = List<Hello, Welcome, Refused, PlayerJoined, PlayerLeft, Ping, Pong, PlayerStatus, Chat, PlayerSample,
                    TestPayload, Host, PlayerEquip, PlayerPose, JoinChallenge, JoinProof, PlayerLife, PvpMode, PvpHit,
                    PvpHurt, RejoinToken, RejoinClaim, PlayerRide, OcarinaNote, DbnoMode, DbnoStatus, DbnoRevive,
                    DbnoThreat, Difficulty, VoiceFrame, ModList, ModSettings, ModData, LobbyAction, LobbyMutes, LobbyResult,
                    LocalJoin, LocalJoined, LocalLeave, LinkSound, LinkEffect, PlayerLook, JoinKnock, JoinAllow, PvpStrike,
                    PvpResult, PvpNative, Emote, PartyWaitMode, NaviSample>;
using Envelope = List<LocalSample, LocalMessage>;   // judged by what they carry

template <std::size_t I>
void check_alternative() {
  using T = std::variant_alternative_t<I, Message>;
  constexpr bool priv = kIn<T, Private>;
  constexpr bool pub = kIn<T, Public>;
  constexpr bool env = kIn<T, Envelope>;
  INFO("Message alternative ", I, " is in ", int(priv) + int(pub) + int(env), " lists");
  CHECK(int(priv) + int(pub) + int(env) == 1);
  if constexpr (!env) {
    CHECK(race_private(Message{T{}}) == priv);
  }
}
template <std::size_t... I>
void check_all(std::index_sequence<I...>) {
  (check_alternative<I>(), ...);
}
}  // namespace

TEST_CASE("race mode: every message type is classed private or public") {
  check_all(std::make_index_sequence<std::variant_size_v<Message>>{});
  CHECK(Private::size + Public::size + Envelope::size == std::variant_size_v<Message>);
}
