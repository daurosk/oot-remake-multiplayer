#pragma once

// One filled-in example of every message, shared by the protocol tests.

#include <vector>

#include "coop/protocol/messages.hpp"

namespace coop::protocol::test {

inline PlayerSample full_sample() {
  PlayerSample s;
  s.player_number = 2;
  s.time = {1'234'567'890'123ull, 42};
  s.position = Vec3{1.5f, -2.25f, 300.0f};
  s.velocity = Vec3{0.0f, -9.8f, 0.125f};
  s.rotation = Quat{0.0f, 0.70710677f, 0.0f, 0.70710677f};
  s.scene = "spot00";
  s.age = Age::adult;
  s.animation = Animation{"link_run", 12.5f, 1.0f};
  return s;
}

inline std::vector<FlagEntry> flag_entries() {
  return {
      {0x248bfa47u, 0, FlagKind::boolean, 1},
      {0x248bfa47u, 3, FlagKind::boolean, 0},  // same flag, another element
      {0x12345678u, 0, FlagKind::int32, 0xFFFFFFFFu},  // -1
      {0x9abcdef0u, 0, FlagKind::float32, 0x3FC00000u},  // 1.5
      {0x0badf00du, 7, FlagKind::uint32, 0xDEADBEEFu},
      {FlagKey::key64_flag(0x0123456789abcdefull), FlagKind::boolean, 1},  // a chest
      {FlagKey::key64_flag(0x0123456789abcdefull, 0x55u), FlagKind::boolean, 0},  // other table
      {FlagKey::key64_flag(0x248bfa47u), FlagKind::boolean, 1},  // not the named 0x248bfa47
  };
}

// A request, an accept and a refuse, as a client sends them.
inline std::vector<JoinPartner> join_messages() {
  JoinPartner ask{0, 2, 7, JoinKind::request, std::nullopt};
  JoinPartner yes{0, 1, 7, JoinKind::accept,
                  JoinTarget{"spot00", {10.0f, 0.5f, -3.0f}, {0.0f, 1.0f, 0.0f, 0.0f}}};
  JoinPartner no{0, 3, 8, JoinKind::refuse, std::nullopt, JoinRefuseReason::cannot_warp};
  return {ask, yes, no};
}

inline PlayerEquip equip_message() {
  PlayerEquip m;
  m.equipment.weapon = "Weapon_Sword_Kokiri";
  m.equipment.shield = "Weapon_Shield_Deku";
  m.equipment.armor = "";  // nothing in that slot
  m.equipment.head = "Armor_Child_Mask_Keaton";
  m.equipment.drawn = 0x05;
  m.equipment.held = "Weapon_Range_SlingShot";
  return m;
}

inline ObjectState full_object_state() {
  ObjectState s;
  s.time = {123456789, 42};
  s.position = Vec3{1.5f, -2.0f, 3.25f};
  s.velocity = Vec3{0.0f, 9.8f, -1.0f};
  s.rotation = Quat{0.0f, 0.0f, 0.0f, 1.0f};
  s.animation = Animation{"Wait", 3.0f, 1.0f};
  s.life = ObjectLife{5, 12};
  s.fuse = 2.5f;
  s.state = object_state::in_fight | object_state::held;
  return s;
}

inline const ObjectId kPlacedId{ObjectKind::placed, 0, 0xf9b880ab096a9cd6ull};
inline const ObjectId kMadeId{ObjectKind::made, 2, 7};

inline std::vector<Message> object_messages() {
  std::vector<Message> all;
  all.push_back(ObjectPresent{0, "Kakariko_Graveyard",
                              {{kPlacedId, object_presence::awake | object_presence::in_fight},
                               {kMadeId, 0}}});
  all.push_back(ObjectPresent{3, "", {}});
  all.push_back(ObjectOwner{kPlacedId, 2, 9, 4});
  all.push_back(ObjectOwner{kMadeId, 0, 0, 0});
  all.push_back(HandoverAsk{kPlacedId, 9, 3});
  all.push_back(ObjectGroup{0, {{kPlacedId, kMadeId, object_group::boss}, {kMadeId, kMadeId, 0}}});
  all.push_back(HandoverState{0, kPlacedId, 9, full_object_state()});
  all.push_back(ObjectSpawn{0, kMadeId, "NormalArrow", object_spawn::projectile, full_object_state()});
  all.push_back(ObjectSample{1, kPlacedId, 65535, full_object_state()});
  ObjectSample minimal;
  minimal.id = kMadeId;
  minimal.state.time = {1, 2};
  all.push_back(minimal);
  all.push_back(ObjectRemove{0, kMadeId, 0, RemoveReason::exploded, {1.0f, 2.0f, 3.0f}});
  all.push_back(Damage{4, kPlacedId, 3, 2, static_cast<std::uint8_t>(DamageCause::sword), {0.5f, 0.0f, -0.5f}});
  all.push_back(PlayerPose{2, {123456789, 7}, 0x00a1b2c3, {PoseBone{3, 0x1f, std::array<std::int16_t, 4>{0, 0, 0, 32767}, std::array<std::int16_t, 3>{-8192, 1, 2}, std::nullopt}, PoseBone{300, 0, std::nullopt, std::nullopt, std::array<std::int16_t, 3>{8192, 8192, 8192}}}});
  all.push_back(PlayerPose{0, {1, 1}, 1, {}});
  all.push_back(TimeOfDay{3, 86399, time_of_day::jump});
  all.push_back(TimeOfDay{0, 0, 0});
  all.push_back(DropSpawn{0, kMadeId, "Graveyard", "Obj_Rupee_Green", {1.0f, 2.0f, 3.0f}, 123456789, drop_spawn::owned});
  all.push_back(DropSpawn{2, kMadeId, "spot00", "Obj_Heart", {0.0f, 0.0f, 0.0f}, 0, drop_spawn::again});
  all.push_back(DropTake{0, kMadeId, DropTakeReason::picked_up});
  all.push_back(DropTake{3, kMadeId, DropTakeReason::gone});
  all.push_back(DropAsk{0, "Graveyard"});
  return all;
}

inline std::vector<Message> every_message() {
  std::vector<Message> all;
  all.push_back(Hello{kProtocolVersion, "lichii 0.1.0", "e0f3ccfaf50065ae", "1.1.1", "Navi"});
  all.push_back(Welcome{3, 987'654'321ull, 60, {{1, "Saria"}, {2, "Mido \xE2\x98\x85"}}});
  all.push_back(Refused{RefuseReason::server_full});
  all.push_back(PlayerJoined{4, "Darunia"});
  all.push_back(PlayerLeft{1, LeaveReason::timed_out});
  all.push_back(Ping{0xFFFF'FFFF'FFFF'FFFEull});
  all.push_back(Pong{111, 222});
  all.push_back(PlayerStatus{0, status::paused | status::in_menu});
  all.push_back(Chat{1, "hello, Hyrule"});
  all.push_back(full_sample());
  PlayerSample minimal;
  minimal.time = {5, 6};
  all.push_back(minimal);
  all.push_back(TestPayload{77, {1, 2, 3, 4, 5, 0, 255}});
  for (const JoinPartner& j : join_messages()) all.push_back(j);
  all.push_back(FlagSet{0, {flag_entries()[0]}});
  all.push_back(FlagState{1, flag_entries()});
  all.push_back(FlagSnapshot{0, 1, flag_entries()});
  all.push_back(FlagSnapshot{2, 3, {}});
  all.push_back(Host{2});
  all.push_back(equip_message());
  all.push_back(PlayerEquip{3, {}});
  for (Message& m : object_messages()) all.push_back(std::move(m));
  return all;
}

}  // namespace coop::protocol::test
