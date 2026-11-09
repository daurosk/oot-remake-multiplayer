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
  return all;
}

}  // namespace coop::protocol::test
