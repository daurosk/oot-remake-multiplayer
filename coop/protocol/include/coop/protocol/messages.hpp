#pragma once

// The messages of protocol version 0, stages 0 and 1 (docs/protocol.md).
// Plain structs only: no sockets, no ENet, no game. The codec in codec.hpp
// turns them into bytes and back.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace coop::protocol {

inline constexpr std::uint16_t kProtocolVersion = 0;
inline constexpr std::uint16_t kDefaultPort = 27510;
inline constexpr std::uint8_t kMaxPlayers = 4;

// Size of the message header: u16 type, u16 length.
inline constexpr std::size_t kHeaderSize = 4;
// Size of a component header: u16 id, u16 length.
inline constexpr std::size_t kComponentHeaderSize = 4;

// Maximum lengths of string fields, in Unicode characters (code points).
// A string may also never be longer than 4 bytes per character, must be valid
// UTF-8 and holds no control characters (U+0000-U+001F, U+007F-U+009F).
inline constexpr std::size_t kMaxBuildIdChars = 64;
inline constexpr std::size_t kMaxGameBuildIdChars = 32;
inline constexpr std::size_t kMaxGameVersionChars = 16;
inline constexpr std::size_t kMaxNicknameChars = 16;
inline constexpr std::size_t kMaxChatChars = 200;
inline constexpr std::size_t kMaxSceneChars = 64;
inline constexpr std::size_t kMaxAnimationChars = 64;

// ENet channels (docs/protocol.md, "Transport").
inline constexpr std::uint8_t kChannelSession = 0;
inline constexpr std::uint8_t kChannelProgress = 1;
inline constexpr std::uint8_t kChannelOwnership = 2;
inline constexpr std::uint8_t kChannelPlayerSamples = 3;
inline constexpr std::uint8_t kChannelObjectSamples = 4;
inline constexpr std::uint8_t kChannelCount = 5;

// Message type numbers on the wire. Never renumber; add new ones at the end.
enum class MessageType : std::uint16_t {
  hello = 1,
  welcome = 2,
  refused = 3,
  player_joined = 4,
  player_left = 5,
  ping = 6,
  pong = 7,
  player_status = 8,
  chat = 9,
  player_sample = 10,
  test_payload = 11,
};

// PLAYER_SAMPLE component ids.
enum class SampleComponent : std::uint16_t {
  time = 1,
  position = 2,
  velocity = 3,
  rotation = 4,
  scene = 5,
  age = 6,
  animation = 7,
};

// Player numbers are 1 to 4. In PLAYER_STATUS, CHAT and PLAYER_SAMPLE a
// client sends 0 ("me"); the server fills in the sender's number when it
// relays the message to the others.
inline constexpr std::uint8_t kPlayerSelf = 0;

// HELLO, client to server. The protocol version is always the first two bytes
// of the body, in every protocol version, so a server can refuse a client
// whose HELLO it cannot otherwise read (see hello_protocol_version).
struct Hello {
  std::uint16_t protocol_version = kProtocolVersion;
  std::string build_id;       // emulator or recomp, and its version
  std::string game_build_id;  // for example "e0f3ccfaf50065ae"
  std::string game_version;   // for example "1.1.1"
  std::string nickname;       // 1 to 16 characters
  bool operator==(const Hello&) const = default;
};

struct PlayerInfo {
  std::uint8_t number = 1;  // 1 to 4
  std::string nickname;
  bool operator==(const PlayerInfo&) const = default;
};

struct Welcome {
  std::uint8_t player_number = 1;  // 1 to 4
  std::uint64_t server_time_us = 0;
  std::uint16_t tick_rate = 0;
  std::vector<PlayerInfo> players;  // the players already in, at most 4
  bool operator==(const Welcome&) const = default;
};

// Reasons are kept as raw numbers on decode, so a newer server's reason
// still reaches the client (which shows it as unknown).
enum class RefuseReason : std::uint8_t {
  wrong_protocol_version = 1,
  wrong_game_build = 2,
  server_full = 3,
  name_taken = 4,
  bad_name = 5,  // empty or nothing but spaces
};

struct Refused {
  RefuseReason reason = RefuseReason::wrong_protocol_version;
  bool operator==(const Refused&) const = default;
};

struct PlayerJoined {
  std::uint8_t player_number = 1;  // 1 to 4
  std::string nickname;
  bool operator==(const PlayerJoined&) const = default;
};

enum class LeaveReason : std::uint8_t {
  left = 0,
  timed_out = 1,
  kicked = 2,
};

struct PlayerLeft {
  std::uint8_t player_number = 1;  // 1 to 4
  LeaveReason reason = LeaveReason::left;  // raw value kept on decode
  bool operator==(const PlayerLeft&) const = default;
};

struct Ping {
  std::uint64_t sender_time_us = 0;
  bool operator==(const Ping&) const = default;
};

struct Pong {
  std::uint64_t ping_sender_time_us = 0;  // echoed from the PING
  std::uint64_t receiver_time_us = 0;     // the PONG sender's clock
  bool operator==(const Pong&) const = default;
};

// PLAYER_STATUS flag bits. Unknown bits are kept, not refused.
namespace status {
inline constexpr std::uint8_t paused = 1u << 0;
inline constexpr std::uint8_t loading = 1u << 1;
inline constexpr std::uint8_t in_cutscene = 1u << 2;
inline constexpr std::uint8_t in_menu = 1u << 3;
inline constexpr std::uint8_t dead = 1u << 4;
}  // namespace status

struct PlayerStatus {
  std::uint8_t player_number = kPlayerSelf;  // 0 from a client, 1 to 4 relayed
  std::uint8_t flags = 0;
  bool operator==(const PlayerStatus&) const = default;
};

struct Chat {
  std::uint8_t player_number = kPlayerSelf;  // 0 from a client, 1 to 4 relayed
  std::string text;  // up to 200 characters
  bool operator==(const Chat&) const = default;
};

struct SampleTime {
  std::uint64_t sender_time_us = 0;
  std::uint32_t sequence = 0;
  bool operator==(const SampleTime&) const = default;
};

using Vec3 = std::array<float, 3>;
using Quat = std::array<float, 4>;  // x, y, z, w

enum class Age : std::uint8_t {
  child = 0,
  adult = 1,
};

struct Animation {
  std::string name;
  float frame = 0.0f;
  float rate = 0.0f;
  bool operator==(const Animation&) const = default;
};

// PLAYER_SAMPLE. The time component is always sent; the others only when the
// sender has them. Floats must be finite.
struct PlayerSample {
  std::uint8_t player_number = kPlayerSelf;  // 0 from a client, 1 to 4 relayed
  SampleTime time;
  std::optional<Vec3> position;
  std::optional<Vec3> velocity;  // metres a second
  std::optional<Quat> rotation;
  std::optional<std::string> scene;
  std::optional<Age> age;
  std::optional<Animation> animation;
  bool operator==(const PlayerSample&) const = default;
};

// TEST_PAYLOAD, used only by the stage 0 test tool. The filler is the rest of
// the body after the sequence number.
struct TestPayload {
  std::uint32_t sequence = 0;
  std::vector<std::uint8_t> filler;
  bool operator==(const TestPayload&) const = default;
};

using Message = std::variant<Hello, Welcome, Refused, PlayerJoined, PlayerLeft,
                             Ping, Pong, PlayerStatus, Chat, PlayerSample,
                             TestPayload>;

MessageType type_of(const Message& message);

// The channel a message is sent on. TEST_PAYLOAD may go on 0 or 3; this
// returns 3 and the test tool picks for itself.
std::uint8_t channel_of(const Message& message);
std::uint8_t channel_of(MessageType type);

// Whether a message of this type may arrive on this channel. False for a type
// this build does not know.
bool allowed_on_channel(MessageType type, std::uint8_t channel);

// The two ends of a connection.
enum class Sender : std::uint8_t {
  client,
  server,
};

// Whether this side may send this message type. HELLO is client only;
// WELCOME, REFUSED, PLAYER_JOINED and PLAYER_LEFT are server only; the rest go
// both ways. False for a type this build does not know.
bool may_send(Sender from, MessageType type);

}  // namespace coop::protocol
