// SPDX-License-Identifier: GPL-3.0-or-later
// A freestanding co-op join client: joins a coop_server the way a Longshot game
// does (docs/protocol.md "Join password", "Encrypted traffic"), with ENet and
// Monocypher and no C++ library. It is the cross-play proof for the ExeFS
// module (v1), not the full client: after WELCOME it only answers PING and
// logs PLAYER_JOINED / PLAYER_LEFT / REFUSED.
//
// Wire format = coop/protocol (codec.cpp, traffic_crypto.cpp, join_password.cpp).
// tools/switch_mod/net_host_harness.cpp checks every byte this file makes against
// those functions, then joins a real coop_server next to a real coop_net client.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace coopnet {

struct JoinConfig {
    uint8_t ip[4];             // server address, for example {127, 0, 0, 1}
    uint16_t port;             // server port
    const char* password;      // the join password (never empty: coop_server requires one)
    const char* nickname;      // 1 to 15 characters
    const char* build_id;      // what HELLO says this build is
    const char* game_build_id; // e0f3ccfaf50065ae (1.1.1)
    const char* game_version;  // 1.1.1
    uint16_t protocol_version; // coop::protocol::kProtocolVersion of the server's release
};

enum class JoinState : uint8_t {
    idle, connecting, hello_sent, proof_sent, welcomed, refused, failed, closed,
};
const char* to_string(JoinState s);

struct JoinStatus {
    JoinState state = JoinState::idle;
    uint8_t player_number = 0;   // from WELCOME
    uint8_t refuse_reason = 0;   // from REFUSED
    uint8_t players_seen = 0;    // others named in WELCOME or PLAYER_JOINED since
    uint32_t pings_answered = 0;
};

// The bytes of HELLO for this config and client key share (type, length, body).
size_t encode_hello(const JoinConfig& c, const uint8_t key_share[32], uint8_t* out, size_t cap);

// The two traffic keys from X25519 (own secret, server share), the password and
// the exact HELLO and JOIN_CHALLENGE packets. False when the shared secret is 0.
bool derive_keys(const uint8_t own_secret[32], const uint8_t server_share[32], const char* password,
                 const uint8_t* hello, size_t hello_len, const uint8_t* challenge, size_t challenge_len,
                 uint8_t client_to_server[32], uint8_t server_to_client[32]);

// HMAC-SHA256(password, nonce || client share || server share).
void join_proof(const char* password, const uint8_t nonce[32], const uint8_t client_share[32],
                const uint8_t server_share[32], uint8_t out[32]);

// SHA-256 (join_proof's hash), exposed for the test.
void sha256(const uint8_t* data, size_t len, uint8_t out[32]);

bool join_start(const JoinConfig& c);   // makes the ENet host and connects
void join_tick();                       // non-blocking; call once a frame
JoinStatus join_status();
void join_stop();                       // disconnects

}  // namespace coopnet
