// SPDX-License-Identifier: GPL-3.0-or-later
// See coop_join.h. Message numbers and layouts: coop/protocol/include/coop/protocol/messages.hpp
// (MessageType, Hello, JoinChallenge, JoinProof, Welcome, Ping, Pong, PlayerJoined) and
// codec.cpp (little-endian u16/u32/u64, strings as u16 length + bytes, each message
// u16 type + u16 body length + body). Seal: traffic_crypto.cpp.
#include "coop_join.h"

#include "net_plat.h"

extern "C" {
#include <enet/enet.h>
#include <monocypher.h>
}

namespace coopnet {
namespace {

constexpr uint16_t kHello = 1, kWelcome = 2, kRefused = 3, kPlayerJoined = 4, kPlayerLeft = 5,
                   kPing = 6, kPong = 7, kJoinChallenge = 191, kJoinProof = 192;
constexpr uint8_t kChannelSession = 0;
constexpr size_t kChannelCount = 6;    // messages.hpp kChannelCount
constexpr size_t kSealChannels = 8;    // traffic_crypto.hpp kSealChannels
constexpr size_t kCounterBytes = 8, kTagBytes = 16;
constexpr char kContext[] = "oot-coop traffic v1";
constexpr char kClientToServer[] = "client to server";
constexpr char kServerToClient[] = "server to client";

// ---- little-endian writer / reader over a fixed buffer ----
struct W {
    uint8_t* p;
    size_t cap, n = 0;
    bool ok = true;
    void u8(uint8_t v) { n < cap ? (void)(p[n++] = v) : (void)(ok = false); }
    void u16(uint16_t v) { u8(uint8_t(v)); u8(uint8_t(v >> 8)); }
    void u64(uint64_t v) { for (int i = 0; i < 8; ++i) u8(uint8_t(v >> (8 * i))); }
    void bytes(const void* d, size_t len) { for (size_t i = 0; i < len; ++i) u8(static_cast<const uint8_t*>(d)[i]); }
    void str(const char* s) { const size_t len = strlen(s); u16(uint16_t(len)); bytes(s, len); }
};
struct R {
    const uint8_t* p;
    size_t len, at = 0;
    bool u8(uint8_t& v) { if (at + 1 > len) return false; v = p[at++]; return true; }
    bool u16(uint16_t& v) { if (at + 2 > len) return false; v = uint16_t(p[at] | p[at + 1] << 8); at += 2; return true; }
    bool u64(uint64_t& v) {
        if (at + 8 > len) return false;
        v = 0;
        for (int i = 7; i >= 0; --i) v = v << 8 | p[at + i];
        at += 8;
        return true;
    }
    bool take(const uint8_t*& out, size_t n) { if (at + n > len) return false; out = p + at; at += n; return true; }
    size_t left() const { return len - at; }
};

// ---- SHA-256 and HMAC (join_password.cpp's join_proof) ----
constexpr uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};
inline uint32_t rotr(uint32_t x, int n) { return x >> n | x << (32 - n); }

struct Sha256 {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t block[64];
    size_t fill = 0;
    uint64_t total = 0;
    void compress() {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = uint32_t(block[4 * i]) << 24 | uint32_t(block[4 * i + 1]) << 16 | uint32_t(block[4 * i + 2]) << 8 | block[4 * i + 3];
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t t1 = k + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + kK[i] + w[i];
            const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
    }
    void add(const uint8_t* d, size_t len) {
        total += len;
        for (size_t i = 0; i < len; ++i) {
            block[fill++] = d[i];
            if (fill == 64) { compress(); fill = 0; }
        }
    }
    void finish(uint8_t out[32]) {
        const uint64_t bits = total * 8;
        const uint8_t one = 0x80, zero = 0;
        add(&one, 1);
        while (fill != 56) add(&zero, 1);
        for (int i = 7; i >= 0; --i) { const uint8_t b = uint8_t(bits >> (8 * i)); add(&b, 1); }
        for (int i = 0; i < 8; ++i)
            for (int j = 0; j < 4; ++j) out[4 * i + j] = uint8_t(h[i] >> (24 - 8 * j));
    }
};

void hmac_sha256(const uint8_t* key, size_t key_len, const uint8_t* msg, size_t msg_len, uint8_t out[32]) {
    uint8_t k[64] = {};
    if (key_len > 64) sha256(key, key_len, k);
    else memcpy(k, key, key_len);
    uint8_t pad[64];
    for (int i = 0; i < 64; ++i) pad[i] = k[i] ^ 0x36;
    Sha256 inner;
    inner.add(pad, 64);
    inner.add(msg, msg_len);
    uint8_t inner_digest[32];
    inner.finish(inner_digest);
    for (int i = 0; i < 64; ++i) pad[i] = k[i] ^ 0x5c;
    Sha256 outer;
    outer.add(pad, 64);
    outer.add(inner_digest, 32);
    outer.finish(out);
}

void hash_part(crypto_blake2b_ctx& ctx, const uint8_t* d, size_t len) {
    uint8_t n[4];
    for (int i = 0; i < 4; ++i) n[i] = uint8_t(uint32_t(len) >> (8 * i));
    crypto_blake2b_update(&ctx, n, 4);
    crypto_blake2b_update(&ctx, d, len);
}

void nonce_for(uint8_t channel, uint64_t counter, uint8_t nonce[24]) {
    memset(nonce, 0, 24);
    for (int i = 0; i < 8; ++i) nonce[i] = uint8_t(counter >> (8 * i));
    nonce[8] = channel;
}

// ---- the one connection ----
struct State {
    JoinConfig cfg{};
    JoinStatus st{};
    ENetHost* host = nullptr;
    ENetPeer* peer = nullptr;
    uint8_t secret[32] = {}, share[32] = {};
    uint8_t hello[256] = {};
    size_t hello_len = 0;
    uint8_t send_key[32] = {}, recv_key[32] = {};
    uint64_t send_counter[kSealChannels] = {};
    uint64_t recv_highest[kSealChannels] = {};
    bool sealed = false;
};
State g;

void fail(const char* why) {
    Line().s("coop-join: FAIL ").s(why);
    g.st.state = JoinState::failed;
}

bool send_plain(uint8_t channel, const uint8_t* d, size_t len) {
    ENetPacket* p = enet_packet_create(d, len, ENET_PACKET_FLAG_RELIABLE);
    if (!p || enet_peer_send(g.peer, channel, p) != 0) return false;
    enet_host_flush(g.host);
    return true;
}

bool send_sealed(uint8_t channel, const uint8_t* plain, size_t len) {
    uint8_t out[kCounterBytes + 64 + kTagBytes];
    if (len > 64) return false;
    const uint64_t counter = ++g.send_counter[channel];
    for (size_t i = 0; i < kCounterBytes; ++i) out[i] = uint8_t(counter >> (8 * i));
    uint8_t nonce[24];
    nonce_for(channel, counter, nonce);
    crypto_aead_lock(out + kCounterBytes, out + kCounterBytes + len, g.send_key, nonce, nullptr, 0, plain, len);
    return send_plain(channel, out, kCounterBytes + len + kTagBytes);
}

// Opens a sealed packet into out; false when it does not open or is a replay.
bool open_sealed(uint8_t channel, const uint8_t* d, size_t len, uint8_t* out, size_t& out_len) {
    if (channel >= kSealChannels || len < kCounterBytes + kTagBytes) return false;
    uint64_t counter = 0;
    for (int i = 7; i >= 0; --i) counter = counter << 8 | d[i];
    const size_t text = len - kCounterBytes - kTagBytes;
    uint8_t nonce[24];
    nonce_for(channel, counter, nonce);
    if (crypto_aead_unlock(out, d + kCounterBytes + text, g.recv_key, nonce, nullptr, 0, d + kCounterBytes, text) != 0)
        return false;
    // Reliable channels arrive in order: only a newer counter is taken (the full
    // 64-packet window of traffic_crypto.cpp is for unreliable ones, unused here).
    if (counter <= g.recv_highest[channel]) return false;
    g.recv_highest[channel] = counter;
    out_len = text;
    return true;
}

void on_challenge(const uint8_t* packet, size_t len, R& r) {
    const uint8_t *nonce = nullptr, *server_share = nullptr;
    if (!r.take(nonce, 32) || r.left() != 32 || !r.take(server_share, 32)) return fail("JOIN_CHALLENGE without a key share");
    uint8_t c2s[32], s2c[32];
    if (!derive_keys(g.secret, server_share, g.cfg.password, g.hello, g.hello_len, packet, len, c2s, s2c))
        return fail("unusable server key share");
    uint8_t proof[32];
    join_proof(g.cfg.password, nonce, g.share, server_share, proof);
    crypto_wipe(g.secret, 32);
    uint8_t msg[4 + 32];
    W w{msg, sizeof msg};
    w.u16(kJoinProof);
    w.u16(32);
    w.bytes(proof, 32);
    if (!send_plain(kChannelSession, msg, w.n)) return fail("JOIN_PROOF not sent");
    memcpy(g.send_key, c2s, 32);
    memcpy(g.recv_key, s2c, 32);
    crypto_wipe(c2s, 32);
    crypto_wipe(s2c, 32);
    g.sealed = true;
    g.st.state = JoinState::proof_sent;
    Line().s("coop-join: JOIN_CHALLENGE answered, JOIN_PROOF sent; traffic sealed from here");
}

void on_message(uint8_t channel, const uint8_t* d, size_t len, bool was_sealed) {
    R r{d, len};
    uint16_t type = 0, body = 0;
    if (!r.u16(type) || !r.u16(body) || r.left() != body) {
        Line().s("coop-join: bad message on channel ").dec(channel).s(", ").dec(len).s(" bytes");
        return;
    }
    R b{d + 4, body};
    if (type == kJoinChallenge && !was_sealed && g.st.state == JoinState::hello_sent) return on_challenge(d, len, b);
    if (type == kRefused) {
        b.u8(g.st.refuse_reason);
        g.st.state = JoinState::refused;
        Line().s("coop-join: REFUSED reason ").dec(g.st.refuse_reason).s(was_sealed ? " (sealed)" : " (plain)");
        return;
    }
    if (!was_sealed) {
        Line().s("coop-join: plain message type ").dec(type).s(" ignored");
        return;
    }
    if (type == kWelcome) {
        uint64_t server_time = 0;
        uint16_t tick = 0;
        uint8_t count = 0;
        if (!b.u8(g.st.player_number) || !b.u64(server_time) || !b.u16(tick) || !b.u8(count)) return fail("short WELCOME");
        g.st.state = JoinState::welcomed;
        Line().s("coop-join: WELCOME: I am player ").dec(g.st.player_number).s(", tick rate ").dec(tick)
            .s(", others in: ").dec(count);
        for (uint8_t i = 0; i < count; ++i) {
            uint8_t num = 0;
            uint16_t n = 0;
            const uint8_t* name = nullptr;
            if (!b.u8(num) || !b.u16(n) || !b.take(name, n)) break;
            ++g.st.players_seen;
            Line().s("coop-join: in the session: player ").dec(num).s(" '").s(reinterpret_cast<const char*>(name), n).s("'");
        }
    } else if (type == kPlayerJoined) {
        uint8_t num = 0;
        uint16_t n = 0;
        const uint8_t* name = nullptr;
        if (b.u8(num) && b.u16(n) && b.take(name, n)) {
            ++g.st.players_seen;
            Line().s("coop-join: PLAYER_JOINED player ").dec(num).s(" '").s(reinterpret_cast<const char*>(name), n).s("'");
        }
    } else if (type == kPlayerLeft) {
        uint8_t num = 0;
        b.u8(num);
        Line().s("coop-join: PLAYER_LEFT player ").dec(num);
    } else if (type == kPing) {
        uint64_t t = 0;
        if (!b.u64(t)) return;
        uint8_t msg[4 + 16];
        W w{msg, sizeof msg};
        w.u16(kPong);
        w.u16(16);
        w.u64(t);
        w.u64(coop_plat_ms() * 1000);
        if (send_sealed(kChannelSession, msg, w.n)) ++g.st.pings_answered;
    }
    // Everything else (game traffic) is not for this proof.
}

}  // namespace

const char* to_string(JoinState s) {
    switch (s) {
        case JoinState::idle: return "idle";
        case JoinState::connecting: return "connecting";
        case JoinState::hello_sent: return "hello_sent";
        case JoinState::proof_sent: return "proof_sent";
        case JoinState::welcomed: return "welcomed";
        case JoinState::refused: return "refused";
        case JoinState::failed: return "failed";
        case JoinState::closed: return "closed";
    }
    return "?";
}

void sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
    Sha256 s;
    s.add(data, len);
    s.finish(out);
}

size_t encode_hello(const JoinConfig& c, const uint8_t key_share[32], uint8_t* out, size_t cap) {
    W w{out, cap};
    w.u16(kHello);
    w.u16(0);  // body length, filled below
    w.u16(c.protocol_version);
    w.str(c.build_id);
    w.str(c.game_build_id);
    w.str(c.game_version);
    w.str(c.nickname);
    w.bytes(key_share, 32);
    if (!w.ok || w.n - 4 > 0xffff) return 0;
    const size_t body = w.n - 4;
    out[2] = uint8_t(body);
    out[3] = uint8_t(body >> 8);
    return w.n;
}

bool derive_keys(const uint8_t own_secret[32], const uint8_t server_share[32], const char* password,
                 const uint8_t* hello, size_t hello_len, const uint8_t* challenge, size_t challenge_len,
                 uint8_t client_to_server[32], uint8_t server_to_client[32]) {
    uint8_t shared[32];
    crypto_x25519(shared, own_secret, server_share);
    const uint8_t zero[32] = {};
    if (crypto_verify32(shared, zero) == 0) return false;
    uint8_t prk[32];
    crypto_blake2b_ctx ctx;
    crypto_blake2b_keyed_init(&ctx, 32, shared, 32);
    hash_part(ctx, reinterpret_cast<const uint8_t*>(kContext), sizeof kContext - 1);
    hash_part(ctx, reinterpret_cast<const uint8_t*>(password), strlen(password));
    hash_part(ctx, hello, hello_len);
    hash_part(ctx, challenge, challenge_len);
    crypto_blake2b_final(&ctx, prk);
    crypto_wipe(shared, 32);
    crypto_blake2b_keyed(client_to_server, 32, prk, 32, reinterpret_cast<const uint8_t*>(kClientToServer),
                         sizeof kClientToServer - 1);
    crypto_blake2b_keyed(server_to_client, 32, prk, 32, reinterpret_cast<const uint8_t*>(kServerToClient),
                         sizeof kServerToClient - 1);
    crypto_wipe(prk, 32);
    return true;
}

void join_proof(const char* password, const uint8_t nonce[32], const uint8_t client_share[32],
                const uint8_t server_share[32], uint8_t out[32]) {
    uint8_t msg[96];
    memcpy(msg, nonce, 32);
    memcpy(msg + 32, client_share, 32);
    memcpy(msg + 64, server_share, 32);
    hmac_sha256(reinterpret_cast<const uint8_t*>(password), strlen(password), msg, sizeof msg, out);
}

bool join_start(const JoinConfig& c) {
    join_stop();
    g = State{};
    g.cfg = c;
    g.host = enet_host_create(nullptr, 1, kChannelCount, 0, 0);
    if (!g.host) {
        fail("enet_host_create (no socket)");
        return false;
    }
    ENetAddress a{};
    memcpy(&a.host, c.ip, 4);  // network order
    a.port = c.port;
    g.peer = enet_host_connect(g.host, &a, kChannelCount, 0);
    if (!g.peer) {
        fail("enet_host_connect");
        return false;
    }
    coop_plat_random(g.secret, 32);
    crypto_x25519_public_key(g.share, g.secret);
    g.hello_len = encode_hello(c, g.share, g.hello, sizeof g.hello);
    if (g.hello_len == 0) {
        fail("HELLO does not fit");
        return false;
    }
    g.st.state = JoinState::connecting;
    Line().s("coop-join: connecting to ").dec(c.ip[0]).s(".").dec(c.ip[1]).s(".").dec(c.ip[2]).s(".").dec(c.ip[3])
        .s(":").dec(c.port).s(" as '").s(c.nickname).s("', protocol ").dec(c.protocol_version);
    return true;
}

void join_tick() {
    if (!g.host) return;
    ENetEvent ev;
    for (int guard = 0; guard < 64; ++guard) {
        const int r = enet_host_service(g.host, &ev, 0);
        if (r < 0) return fail("enet_host_service error");
        if (r == 0) break;
        if (ev.type == ENET_EVENT_TYPE_CONNECT) {
            Line().s("coop-join: ENet connected; HELLO sent (").dec(g.hello_len).s(" bytes)");
            if (!send_plain(kChannelSession, g.hello, g.hello_len)) return fail("HELLO not sent");
            g.st.state = JoinState::hello_sent;
        } else if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
            uint8_t plain[2048];
            size_t plain_len = 0;
            const uint8_t* d = ev.packet->data;
            const size_t len = ev.packet->dataLength;
            if (g.sealed && len <= sizeof plain + kCounterBytes + kTagBytes && open_sealed(ev.channelID, d, len, plain, plain_len))
                on_message(ev.channelID, plain, plain_len, true);
            else if (g.sealed && len > sizeof plain + kCounterBytes + kTagBytes)
                ;  // a big game message (flags snapshot): not for this proof
            else
                on_message(ev.channelID, d, len, false);
            enet_packet_destroy(ev.packet);
        } else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
            Line().s("coop-join: disconnected (data ").dec(ev.data).s(")");
            if (g.st.state != JoinState::refused) g.st.state = JoinState::closed;
            g.peer = nullptr;
        }
    }
}

JoinStatus join_status() { return g.st; }

void join_stop() {
    if (g.peer) {
        enet_peer_disconnect_now(g.peer, 0);
        g.peer = nullptr;
    }
    if (g.host) {
        enet_host_destroy(g.host);
        g.host = nullptr;
    }
}

}  // namespace coopnet
