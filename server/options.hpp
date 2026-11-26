#pragma once

// coop_server's command line.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "session.hpp"
#include "test_delay.hpp"

namespace coop::server {

struct CommandLine {
  std::uint16_t port = 27510;  // docs/protocol.md, "Transport"
  std::string game_build_id;   // empty: the first player decides
  std::optional<std::uint32_t> run_for_ms;  // stop by itself after this long
  // --owner-hold-ms: SessionConfig::owner_hold_us in ms; none: its default.
  std::optional<std::uint32_t> owner_hold_ms;
  // Connections one address may hold before HELLO; 0: no limit.
  std::uint32_t max_half_open = 4;
  std::uint32_t max_players = 16;           // --max-players, 1 to 16
  std::uint32_t pose_upload_kbps = 0;       // --pose-upload-kbps, 0: no cap
  // --bind: listen on this one IPv4 address only (a VPN adapter's); none: all.
  std::optional<std::uint32_t> bind;
  // --allow: only these addresses may connect; empty: any.
  std::vector<AddressRange> allowed;
  // --password: the join password (empty: none). --password-env names an
  // environment variable that holds it instead, so it is not on the command
  // line (where other programs can read it); main() reads that variable.
  std::string password;
  std::string password_env;
  bool port_given = false;  // --port was on the command line
  // --upnp: ask the router to forward the port while the server runs (upnp.hpp).
  bool upnp = false;
  // --relay: run as a relay (relay.hpp) instead of a game server; its port is
  // 27530 unless --port says otherwise.
  bool relay = false;
  // --relay-via ADDRESS[:PORT]: host through that relay and get a join code.
  std::string relay_via;
  // --relay-room NAME: with --relay-via, also hold this room name on the relay
  // (players join with the name instead of the code).
  std::string relay_room;
  // --relay-list [NAME]: with --relay-room, put the room on the relay's public
  // room list, shown as NAME; without one, as the room name (players, most
  // players and the protocol version go with it; never the password).
  std::string relay_list;
  // --public: a public game (HostServiceConfig::public_game): anyone with the
  // room name joins without a password, through the relay only, listed. Needs
  // --relay-via and --relay-room; no --upnp. A --password given is replaced by
  // the room's (protocol::public_join_password), as the game's Host page does.
  bool public_game = false;
  // --dedicated: an always-on server with no hosting game (docs/official-game.md):
  // it reads moderation commands typed into its console (lines starting '/':
  // Session::admin). Needs --public.
  bool dedicated = false;
  // --bans-file PATH: the bans by player key kept across runs (one key id a
  // line, read_ban_file); rewritten when a ban is made or lifted.
  std::string bans_file;
  // --official-key-file PATH: the official game's private key (64 hex digits; docs/official-game.md): each
  // listing goes with a signed proof, so kits mark the room official. Needs --public. On Linux the file must
  // not be readable by group or others (chmod 600).
  std::string official_key_file;
  // --official-relay HOST[:PORT]: the relay as the kits reach it (their default relay; docs/official-game.md), signed
  // into each proof. Needed with --official-key-file (--relay-via is this machine's own way to it, 127.0.0.1).
  std::string official_relay;
  // --relay-key / --relay-key-env: the relay's host key (on the relay: the one
  // hosts must give; with --relay-via: the one to give).
  std::string relay_key;
  std::string relay_key_env;
  // --relay-max-per-address N: with --relay, sessions allowed from one address (0: the relay's default, 2).
  std::size_t relay_max_per_address = 0;
  // --invite: learn this PC's internet address with STUN from the game port,
  // print an invite code (address + join password) and take answer codes
  // typed into the window (punch.hpp). Needs a join password.
  bool invite = false;
  // --stun NAME:PORT (more than once): the STUN servers; none given: the
  // free public ones (nat_net.hpp).
  std::vector<std::string> stun;
  // --join ADDRESS:PORT: run as Join.bat's helper for an invite code
  // (join_helper.hpp) instead of a server; the game connects to 127.0.0.1
  // on --game-port (27512 unless given).
  std::string join;
  std::uint16_t game_port = 27512;
  // --pvp on|off: players hurt each other (docs/play-online.md, "PvP").
  bool pvp = false;
  // --dbno on|off: down but not out (docs/play-online.md); on by default.
  bool dbno = true;
  // --approve-joins: the host lets each new player in (docs/protocol.md,
  // "Host approval"); off by default.
  bool approve_joins = false;
  // --same-network-open: players from a private address join without the
  // join password (docs/protocol.md "Same network").
  bool same_network_open = false;
  // --test-delay-ms / --test-jitter-ms: TEST ONLY (test_delay.hpp); main
  // refuses them unless the environment has OOT_COOP_TEST=1.
  TestDelay test_delay;
  bool help = false;
};

// Parses the arguments after the program name. On a mistake, returns nothing
// and puts the reason in `error`.
std::optional<CommandLine> parse_command_line(const std::vector<std::string>& args,
                                              std::string& error);

// Why a join password cannot be used (too long, a control character), or
// null when it is fine. Empty passes here (the option may come from an
// environment variable); main then refuses to start without a password.
const char* password_problem(std::string_view password);

// The --help text.
const char* usage();

}  // namespace coop::server
