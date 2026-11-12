#include "coop/net/client.hpp"

// ENet pulls in windows.h on Windows; keep its min and max macros out.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <enet/enet.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include "coop/net/enet_path.hpp"
#include "coop/net/enet_waiting.hpp"
#include "coop/protocol/codec.hpp"

namespace coop::net {

namespace proto = coop::protocol;

namespace {

bool is_delta(const proto::FlagEntry& flag) { return flag.kind == proto::FlagKind::count_delta; }

// Two count deltas as one, held to the s32 range.
std::uint32_t add_delta(std::uint32_t a, std::uint32_t b) {
  const std::int64_t sum = std::int64_t{static_cast<std::int32_t>(a)} + static_cast<std::int32_t>(b);
  const auto held = std::clamp<std::int64_t>(sum, std::numeric_limits<std::int32_t>::min(),
                                             std::numeric_limits<std::int32_t>::max());
  return static_cast<std::uint32_t>(static_cast<std::int32_t>(held));
}

std::uint64_t steady_now_us() {
  const auto since = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(since).count());
}

constexpr std::uint64_t ms_to_us(std::uint32_t ms) {
  return static_cast<std::uint64_t>(ms) * 1000u;
}

// A REFUSED name taken is asked again no faster than this.
constexpr std::uint32_t kNameTakenRetryFloorMs = 100;

// The longest packet a message can fill: the 4-byte header and a body of
// at most 65535 bytes, and what sealing adds (traffic_crypto.hpp). ENet
// refuses anything longer before assembling it.
constexpr std::size_t kMaxPacketBytes = 4 + 0xffff + proto::kSealOverheadBytes;

// PONGs answer at most this many PINGs at once, then one per interval.
constexpr std::uint64_t kPongBurst = 4;
constexpr std::uint64_t kPongIntervalUs = 125'000;

// Time passed since `then`; 0 when an injected clock stepped back.
constexpr std::uint64_t elapsed(std::uint64_t now, std::uint64_t then) {
  return now >= then ? now - then : 0;
}

ClientConfig sanitised(ClientConfig config) {
  config.retry_first_ms = std::max<std::uint32_t>(config.retry_first_ms, 1);
  config.retry_max_ms = std::max(config.retry_max_ms, config.retry_first_ms);
  config.max_events_per_service =
      std::max<std::uint32_t>(config.max_events_per_service, 1);
  return config;
}

// Characters (code points) in UTF-8 text: the bytes that do not continue one.
std::size_t utf8_chars(std::string_view text) {
  std::size_t n = 0;
  for (const char c : text) {
    n += (static_cast<unsigned char>(c) & 0xc0u) != 0x80u ? 1 : 0;
  }
  return n;
}

using proto::valid_flag;

FlagKey key_of(const proto::FlagEntry& flag) { return flag.key; }

}  // namespace

std::uint32_t packet_flags(std::uint8_t channel) {
  return channel < proto::kChannelPlayerSamples
             ? static_cast<std::uint32_t>(ENET_PACKET_FLAG_RELIABLE)
             : static_cast<std::uint32_t>(ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
}

std::string_view to_string(State state) {
  switch (state) {
    case State::idle: return "idle";
    case State::connecting: return "connecting";
    case State::handshaking: return "handshaking";
    case State::connected: return "connected";
    case State::waiting_retry: return "waiting to retry";
    case State::refused: return "refused";
    case State::stopped: return "stopped";
  }
  return "unknown";
}

std::string_view to_string(EventKind kind) {
  switch (kind) {
    case EventKind::welcomed: return "welcomed";
    case EventKind::refused: return "refused";
    case EventKind::connect_failed: return "connect failed";
    case EventKind::disconnected: return "disconnected";
    case EventKind::reconnected: return "reconnected";
    case EventKind::message: return "message";
    case EventKind::chat: return "chat";
    case EventKind::player_joined: return "player joined";
    case EventKind::player_left: return "player left";
    case EventKind::player_status: return "player status";
    case EventKind::flag_state: return "flag state";
    case EventKind::flag_snapshot: return "flag snapshot";
    case EventKind::flag_reset: return "flag reset";
    case EventKind::join_request: return "join request";
    case EventKind::join_accepted: return "join accepted";
    case EventKind::join_refused: return "join refused";
    case EventKind::join_timed_out: return "join timed out";
    case EventKind::host_changed: return "host changed";
    case EventKind::player_equip: return "player equip";
  }
  return "unknown";
}

std::string_view refuse_reason_text(proto::RefuseReason reason) {
  switch (reason) {
    case proto::RefuseReason::wrong_protocol_version:
      return "wrong protocol version: this game and the server are different releases; "
             "run Update.bat in every kit, the host's too";
    case proto::RefuseReason::wrong_game_build: return "wrong game build";
    case proto::RefuseReason::server_full: return "server full";
    case proto::RefuseReason::name_taken: return "name taken";
    case proto::RefuseReason::bad_name: return "bad name";
    case proto::RefuseReason::bad_password:
      return "wrong join password: ask the host for it, then start Join.bat again and type it";
  }
  return "unknown reason";
}

Client::Client(ClientConfig config) : config_(sanitised(std::move(config))) {}

Client::~Client() {
  stop();
  if (host_ != nullptr) enet_host_destroy(host_);
  if (enet_started_) enet_deinitialize();
}

std::uint64_t Client::now_us() const {
  return config_.now_us ? config_.now_us() : steady_now_us();
}

std::uint64_t Client::server_now_us() const { return to_server_time(now_us()); }

std::uint64_t Client::to_server_time(std::uint64_t client_time_us) const {
  return apply_offset(client_time_us, clock_.offset_us().value_or(0));
}

std::uint64_t Client::to_client_time(std::uint64_t server_time_us) const {
  const std::int64_t offset = clock_.offset_us().value_or(0);
  // Negating the most negative i64 does not fit; it is never a real offset.
  if (offset == std::numeric_limits<std::int64_t>::min()) return server_time_us;
  return apply_offset(server_time_us, -offset);
}

bool Client::start() {
  if (state_ != State::idle) return false;
  if (enet_initialize() != 0) return false;
  enet_started_ = true;
  host_ = enet_host_create(nullptr, 1, proto::kChannelCount, 0, 0);
  if (host_ == nullptr) return false;
  host_->maximumPacketSize = kMaxPacketBytes;
  host_->maximumWaitingData = config_.max_waiting_bytes;
  host_->mtu = std::clamp<std::uint32_t>(config_.mtu, ENET_PROTOCOL_MINIMUM_MTU,
                                         ENET_PROTOCOL_MAXIMUM_MTU);  // issue 143
  begin_attempt();
  return true;
}

void Client::stop() {
  if (peer_ != nullptr) {
    // Sends the goodbye at once and forgets the peer; no reply is awaited.
    enet_peer_disconnect_now(peer_, 0);
    peer_ = nullptr;
  }
  if (state_ != State::idle) state_ = State::stopped;
  players_.clear();
  clear_shared();
  early_.clear();
}

void Client::begin_attempt() {
  attempt_started_us_ = now_us();
  cipher_.reset();
  key_pair_.reset();
  hello_packet_.clear();
  path_mark_ = PathMark{};
  last_receive_us_ = 0;
  link_ = LinkInfo{};
  ENetAddress address{};
  address.port = config_.port;
  if (!address_known_) {
    // May block on a DNS name; an IP is read at once. Kept once it works.
    if (enet_address_set_host(&address, config_.host.c_str()) != 0) {
      state_ = State::connecting;
      end_attempt(DropReason::no_route);
      return;
    }
    address_host_ = address.host;
    address_known_ = true;
  }
  address.host = address_host_;
  peer_ = enet_host_connect(host_, &address, proto::kChannelCount, 0);
  if (peer_ == nullptr) {
    state_ = State::connecting;
    end_attempt(DropReason::no_route);
    return;
  }
  enet_peer_timeout(peer_, 0, config_.timeout_ms, config_.timeout_ms);
  state_ = State::connecting;
}

void Client::end_attempt(DropReason reason) {
  if (peer_ != nullptr) {
    enet_peer_disconnect_now(peer_, 0);
    peer_ = nullptr;
  }
  const bool was_connected = state_ == State::connected;
  // Only a REFUSED name taken keeps the retry window going; any other end
  // of an attempt closes it, so a later name taken gets its full window.
  name_taken_since_us_ = 0;
  players_.clear();
  clear_shared();
  early_.clear();
  Event event;
  event.kind = was_connected ? EventKind::disconnected : EventKind::connect_failed;
  event.drop_reason = reason;
  event.link = link();  // state_ is still the one that ended
  push(std::move(event));

  ++failed_attempts_;
  // Only a connection that lasted starts the delays over; one that a server
  // drops right after WELCOME counts as one more failure.
  const bool stable = was_connected &&
      elapsed(now_us(), welcomed_us_) >= ms_to_us(config_.stable_after_ms);
  if (stable || retry_delay_ms_ == 0) {
    retry_delay_ms_ = config_.retry_first_ms;
  } else {
    retry_delay_ms_ = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        static_cast<std::uint64_t>(retry_delay_ms_) * 2, config_.retry_max_ms));
  }
  // Not a failure of the server: try again at once with small datagrams.
  if (reason == DropReason::small_path) retry_delay_ms_ = config_.retry_first_ms;
  retry_at_us_ = now_us() + ms_to_us(retry_delay_ms_);
  state_ = State::waiting_retry;
}

void Client::service(std::uint32_t wait_ms) {
  if (host_ == nullptr || state_ == State::idle || state_ == State::stopped ||
      state_ == State::refused) {
    return;
  }

  ENetEvent event;
  std::uint32_t wait = wait_ms;
  // Bounded, so a flood from the server cannot keep service() from returning.
  for (std::uint32_t handled = 0; handled < config_.max_events_per_service &&
                                  enet_host_service(host_, &event, wait) > 0;
       ++handled) {
    wait = 0;
    switch (event.type) {
      case ENET_EVENT_TYPE_CONNECT:
        if (event.peer == peer_ && state_ == State::connecting) {
          proto::Hello hello = config_.hello;
          last_receive_us_ = now_us();  // the server answered the connect
          // The token of our last place, shown before HELLO: with it the
          // server lets this connection take over our own quiet place.
          bool claimed = true;
          if (rejoin_token_) {
            claimed = send_raw(proto::RejoinClaim{*rejoin_token_}, proto::kChannelSession);
          }
          // Our key share for this connection (docs/protocol.md "Encrypted
          // traffic"); a server without a password ignores it.
          key_pair_.emplace(proto::KeyPair::generate());
          hello.key_share = key_pair_->share();
          hello_packet_.clear();
          if (claimed && proto::encode(hello, hello_packet_) == proto::Error::none &&
              send_packet(hello_packet_, proto::channel_of(hello))) {
            hello_sent_us_ = now_us();
            challenge_answered_ = false;
            state_ = State::handshaking;
          } else {
            end_attempt(DropReason::connection_lost);
          }
        }
        break;
      case ENET_EVENT_TYPE_RECEIVE:
        if (event.peer == peer_) {
          last_receive_us_ = now_us();
          if (cipher_ != nullptr) {
            // Encrypted: only what opens with the key counts. The one plain
            // answer left is REFUSED for a wrong password, which the server
            // sends before it has keys (anyone on the path could also just
            // drop our packets, so it gives nothing away).
            std::vector<std::uint8_t> plain;
            const std::span<const std::uint8_t> raw(event.packet->data,
                                                    event.packet->dataLength);
            proto::Message refused;
            if (cipher_->open(event.channelID, raw, plain) == proto::OpenError::none) {
              on_receive(plain.data(), plain.size(), event.channelID);
            } else if (state_ == State::handshaking &&
                       event.channelID == proto::kChannelSession &&
                       proto::decode(raw, refused) == proto::Error::none &&
                       std::holds_alternative<proto::Refused>(refused)) {
              on_receive(raw.data(), raw.size(), event.channelID);
            } else {
              ++rejected_;
            }
          } else {
            on_receive(event.packet->data, event.packet->dataLength,
                       event.channelID);
          }
        }
        enet_packet_destroy(event.packet);
        break;
      case ENET_EVENT_TYPE_DISCONNECT:
        if (event.peer == peer_) {
          peer_ = nullptr;  // ENet has already reset it
          const bool to_small = event.data == kDisconnectSmallDatagrams;
          if (to_small) use_small_datagrams();
          if (state_ == State::connecting || state_ == State::handshaking ||
              state_ == State::connected) {
            end_attempt(to_small ? DropReason::small_path : DropReason::connection_lost);
          }
        }
        break;
      case ENET_EVENT_TYPE_NONE:
        break;
    }
    if (state_ == State::refused || state_ == State::stopped) return;
  }

  const std::uint64_t now = now_us();
  switch (state_) {
    case State::connecting:
    case State::handshaking:
      if (elapsed(now, attempt_started_us_) >= ms_to_us(config_.handshake_timeout_ms)) {
        end_attempt(DropReason::handshake_timeout);
      }
      break;
    case State::connected:
      if (now >= next_ping_us_) {
        send_raw(proto::Ping{now}, proto::kChannelSession);
        next_ping_us_ = now + ms_to_us(config_.ping_interval_ms);
      }
      check_join_timeouts(now);
      break;
    case State::waiting_retry:
      if (now >= retry_at_us_) begin_attempt();
      break;
    case State::idle:
    case State::refused:
    case State::stopped:
      break;
  }
  if (peer_ != nullptr) {
    // ENet forgets to take thrown-away fragments off its count; past the cap
    // it would refuse (and never acknowledge) the server's packets.
    link_.mended_bytes += mend_waiting_data(*peer_);
    link_.waiting_bytes = static_cast<std::uint32_t>(
        std::min<std::size_t>(peer_->totalWaitingData, 0xffffffffu));
    link_.round_trip_ms = peer_->roundTripTime;
    link_.loss_percent = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(peer_->packetLoss) * 100 / ENET_PEER_PACKET_LOSS_SCALE);
    link_.mtu = peer_->mtu;
    if ((state_ == State::handshaking || state_ == State::connected) &&
        full_datagrams_lost(path_mark_, *peer_, host_->serviceTime)) {
      use_small_datagrams();
      end_attempt(DropReason::small_path);
    }
  }
  enet_host_flush(host_);
}

void Client::use_small_datagrams() {
  small_datagrams_ = true;
  if (host_ != nullptr && host_->mtu > kSmallDatagramBytes) host_->mtu = kSmallDatagramBytes;
}

LinkInfo Client::link() const {
  LinkInfo out = link_;
  if (state_ == State::connecting || state_ == State::handshaking ||
      state_ == State::connected) {
    const std::uint64_t since = last_receive_us_ != 0 ? last_receive_us_ : attempt_started_us_;
    out.quiet_ms = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(elapsed(now_us(), since) / 1000, 0xffffffffu));
  }
  return out;
}

void Client::on_receive(const std::uint8_t* data, std::size_t size,
                        std::uint8_t channel) {
  proto::Message message;
  if (proto::decode(std::span<const std::uint8_t>(data, size), message) !=
          proto::Error::none ||
      proto::check_received(message, proto::Sender::server, channel) !=
          proto::Error::none) {
    ++rejected_;
    return;
  }

  // The server pings from its side too; answer whenever there is a peer.
  // At most kPongBurst at once, then one per kPongIntervalUs: a PING flood
  // cannot fill the reliable send queue.
  if (const auto* ping = std::get_if<proto::Ping>(&message)) {
    const std::uint64_t now = now_us();
    const std::uint64_t due = std::max(pong_due_us_, now);
    if (due - now > (kPongBurst - 1) * kPongIntervalUs) {
      ++rejected_;
      return;
    }
    pong_due_us_ = due + kPongIntervalUs;
    send_raw(proto::Pong{ping->sender_time_us, now}, proto::kChannelSession);
    return;
  }

  if (state_ == State::handshaking) {
    if (const auto* welcome = std::get_if<proto::Welcome>(&message)) {
      on_welcome(*welcome);
    } else if (const auto* challenge = std::get_if<proto::JoinChallenge>(&message)) {
      // One answer per connection: the proof is for this nonce only.
      if (challenge_answered_) {
        ++rejected_;
        return;
      }
      challenge_answered_ = true;
      if (!answer_challenge(*challenge, std::span<const std::uint8_t>(data, size))) {
        // A server with a password that does not encrypt: never fall back to
        // plain traffic (someone on the path may have taken the key out).
        if (peer_ != nullptr) {
          enet_peer_disconnect_now(peer_, 0);
          peer_ = nullptr;
        }
        state_ = State::refused;
        Event event;
        event.kind = EventKind::refused;
        event.refuse_reason = proto::RefuseReason::wrong_protocol_version;
        push(std::move(event));
      }
    } else if (const auto* refused = std::get_if<proto::Refused>(&message)) {
      if (peer_ != nullptr) {
        enet_peer_disconnect_now(peer_, 0);
        peer_ = nullptr;
      }
      // Our own crashed build may still hold the name for a moment (see
      // name_taken_retry_ms): ask again before giving up.
      if (refused->reason == proto::RefuseReason::name_taken &&
          config_.name_taken_retry_for_ms != 0 && config_.name_taken_retry_ms != 0) {
        const std::uint64_t now = now_us();
        if (name_taken_since_us_ == 0) name_taken_since_us_ = now;
        if (elapsed(now, name_taken_since_us_) < ms_to_us(config_.name_taken_retry_for_ms)) {
          ++name_taken_retries_;
          retry_at_us_ = now + ms_to_us(std::max<std::uint32_t>(config_.name_taken_retry_ms,
                                                                kNameTakenRetryFloorMs));
          state_ = State::waiting_retry;
          return;
        }
      }
      name_taken_since_us_ = 0;
      state_ = State::refused;
      Event event;
      event.kind = EventKind::refused;
      event.refuse_reason = refused->reason;
      push(std::move(event));
    } else if (channel == proto::kChannelProgress &&
               early_.size() < config_.max_early_messages) {
      // HOST and the snapshot may beat WELCOME (channels are not ordered
      // against each other): kept for right after it.
      early_.push_back(EarlyMessage{std::move(message), channel});
    } else {
      ++rejected_;  // nothing else is expected before the answer to HELLO
    }
    return;
  }

  if (state_ != State::connected) {
    ++rejected_;
    return;
  }
  on_connected_message(std::move(message), channel);
}

void Client::on_connected_message(proto::Message message, std::uint8_t channel) {
  if (const auto* pong = std::get_if<proto::Pong>(&message)) {
    // Only an echo of a PING sent on this connection counts.
    const std::uint64_t now = now_us();
    if (pong->ping_sender_time_us < hello_sent_us_ || pong->ping_sender_time_us > now) {
      ++rejected_;
      return;
    }
    if (auto sample = clock_sample(pong->ping_sender_time_us,
                                   pong->receiver_time_us, now)) {
      clock_.add(*sample);
    } else {
      ++rejected_;
    }
    return;
  }

  if (const auto* token = std::get_if<proto::RejoinToken>(&message)) {
    rejoin_token_ = token->token;  // kept for the next connection
    return;
  }

  if (std::holds_alternative<proto::Welcome>(message) ||
      std::holds_alternative<proto::Refused>(message) ||
      std::holds_alternative<proto::JoinChallenge>(message)) {
    ++rejected_;  // the handshake is over
    return;
  }

  std::optional<Event> room;
  if (!apply_room(message, channel, room)) {
    ++rejected_;
    return;
  }
  if (!room) {
    if (const auto* join = std::get_if<proto::JoinPartner>(&message);
        join != nullptr && join->kind != proto::JoinKind::request &&
        (!our_join_ || join->request_id != our_join_->request_id ||
         join->player_number != our_join_->player)) {
      ++stale_join_;  // an answer to a request that is no longer open
      return;
    }
    if (!apply_shared(message, channel, room)) {
      ++rejected_;
      return;
    }
  }
  Event event;
  event.kind = EventKind::message;
  event.message = std::move(message);
  event.channel = channel;
  push(std::move(event));
  if (room) push(std::move(*room));
}

const Player* Client::find_player(std::uint8_t number) const {
  for (const Player& p : players_) {
    if (p.number == number) return &p;
  }
  return nullptr;
}

Player* Client::player_entry(std::uint8_t number) {
  for (Player& p : players_) {
    if (p.number == number) return &p;
  }
  return nullptr;
}

bool Client::apply_room(const proto::Message& message, std::uint8_t channel,
                        std::optional<Event>& out) {
  Event event;
  event.channel = channel;
  if (const auto* joined = std::get_if<proto::PlayerJoined>(&message)) {
    if (joined->player_number == player_number_) return false;
    Player player;
    player.number = joined->player_number;
    player.nickname = joined->nickname;
    if (Player* known = player_entry(joined->player_number)) {
      *known = std::move(player);  // a leave we never heard of: a new player
    } else {
      const auto at = std::find_if(players_.begin(), players_.end(),
                                   [&](const Player& p) { return p.number > player.number; });
      players_.insert(at, std::move(player));
    }
    event.kind = EventKind::player_joined;
    event.player = joined->player_number;
    event.nickname = joined->nickname;
    // The newcomer has not seen what we wear.
    if (const Player* self = find_player(player_number_); self && self->equipment) {
      send_raw(proto::PlayerEquip{proto::kPlayerSelf, *self->equipment},
               proto::channel_of(proto::MessageType::player_equip));
    }
  } else if (const auto* left = std::get_if<proto::PlayerLeft>(&message)) {
    const Player* known = find_player(left->player_number);
    if (known == nullptr || known->self) return false;
    event.kind = EventKind::player_left;
    event.player = left->player_number;
    event.nickname = known->nickname;
    event.leave_reason = left->reason;
    std::erase_if(players_, [&](const Player& p) { return p.number == left->player_number; });
    std::erase_if(their_joins_, [&](const OpenJoin& j) { return j.player == left->player_number; });
  } else if (const auto* status = std::get_if<proto::PlayerStatus>(&message)) {
    Player* known = player_entry(status->player_number);
    if (known == nullptr || known->self) return false;
    known->status = status->flags;
    event.kind = EventKind::player_status;
    event.player = status->player_number;
    event.status_flags = status->flags;
  } else if (const auto* equip = std::get_if<proto::PlayerEquip>(&message)) {
    Player* known = player_entry(equip->player_number);
    if (known == nullptr || known->self) return false;
    known->equipment = proto::merge_equipment(known->equipment.value_or(proto::Equipment{}),
                                              equip->equipment);
    event.kind = EventKind::player_equip;
    event.player = equip->player_number;
    event.equipment = *known->equipment;
  } else if (const auto* chat = std::get_if<proto::Chat>(&message)) {
    const Player* known = find_player(chat->player_number);
    if (known == nullptr || known->self) return false;
    event.kind = EventKind::chat;
    event.player = chat->player_number;
    event.text = chat->text;
  } else {
    return true;  // not a room message
  }
  out = std::move(event);
  return true;
}

void Client::on_welcome(const proto::Welcome& welcome) {
  const std::uint64_t now = now_us();
  state_ = State::connected;
  welcomed_us_ = now;
  player_number_ = welcome.player_number;
  failed_attempts_ = 0;
  name_taken_since_us_ = 0;

  players_.clear();
  clear_shared();
  Player self;
  self.number = welcome.player_number;
  self.nickname = config_.hello.nickname;
  self.self = true;
  players_.push_back(std::move(self));
  for (const proto::PlayerInfo& info : welcome.players) {
    if (info.number == welcome.player_number) continue;  // the codec refuses it
    Player player;
    player.number = info.number;
    player.nickname = info.nickname;
    players_.push_back(std::move(player));
  }
  std::sort(players_.begin(), players_.end(),
            [](const Player& a, const Player& b) { return a.number < b.number; });

  // HELLO out and WELCOME back are a first round trip: a rough first sample
  // until the PINGs give better ones.
  clock_.reset();
  if (auto sample = clock_sample(hello_sent_us_, welcome.server_time_us, now)) {
    clock_.add(*sample);
  }
  next_ping_us_ = now;  // the first PING goes out in this service()

  Event event;
  event.kind = ever_welcomed_ ? EventKind::reconnected : EventKind::welcomed;
  event.welcome = welcome;
  ever_welcomed_ = true;
  push(std::move(event));

  std::vector<EarlyMessage> early = std::move(early_);
  early_.clear();
  for (EarlyMessage& e : early) on_connected_message(std::move(e.message), e.channel);
}

std::optional<Event> Client::next_event() {
  if (events_.empty()) return std::nullopt;
  Event event = std::move(events_.front());
  events_.pop_front();
  return event;
}

bool Client::send(const proto::Message& message) {
  return send_on(message, proto::channel_of(message));
}

bool Client::send_on(const proto::Message& message, std::uint8_t channel) {
  if (state_ != State::connected) return false;
  // The same check the server makes on what it receives: a client may send
  // this type, on this channel, with 0 in its player field.
  if (proto::check_received(message, proto::Sender::client, channel) !=
      proto::Error::none) {
    return false;
  }
  if (!send_raw(message, channel)) return false;
  if (const auto* status = std::get_if<proto::PlayerStatus>(&message)) {
    if (Player* self = player_entry(player_number_)) self->status = status->flags;
  }
  if (const auto* equip = std::get_if<proto::PlayerEquip>(&message)) {
    if (Player* self = player_entry(player_number_)) {
      self->equipment = proto::merge_equipment(self->equipment.value_or(proto::Equipment{}),
                                               equip->equipment);
    }
  }
  return true;
}

bool Client::send_chat(std::string_view text) {
  const std::size_t chars = utf8_chars(text);
  if (chars == 0 || chars > kMaxChatChars) return false;
  return send(proto::Chat{proto::kPlayerSelf, std::string(text)});
}

bool Client::answer_challenge(const proto::JoinChallenge& challenge,
                              std::span<const std::uint8_t> packet) {
  if (!challenge.key_share || !key_pair_) return false;
  std::optional<proto::TrafficKeys> keys =
      proto::derive_traffic_keys(*key_pair_, *challenge.key_share, config_.join_password,
                                 hello_packet_, packet, /*is_server=*/false);
  key_pair_.reset();
  hello_packet_.clear();
  if (!keys) return false;
  // The proof goes out plain (the server needs it to make its keys); from
  // the next packet on, everything is sealed.
  const bool sent = send_raw(
      proto::JoinProof{proto::join_proof(config_.join_password, challenge.nonce)},
      proto::kChannelSession);
  cipher_ = std::make_unique<proto::TrafficCipher>(*keys);
  keys->wipe();
  return sent;
}

bool Client::send_raw(const proto::Message& message, std::uint8_t channel) {
  std::vector<std::uint8_t> bytes;
  if (proto::encode(message, bytes) != proto::Error::none) return false;
  return send_packet(bytes, channel);
}

bool Client::send_packet(std::span<const std::uint8_t> plain, std::uint8_t channel) {
  if (peer_ == nullptr) return false;
  std::optional<std::vector<std::uint8_t>> sealed;
  std::span<const std::uint8_t> bytes = plain;
  if (cipher_ != nullptr) {
    sealed = cipher_->seal(channel, plain);
    if (!sealed) return false;
    bytes = *sealed;
  }
  ENetPacket* packet =
      enet_packet_create(bytes.data(), bytes.size(), packet_flags(channel));
  if (packet == nullptr) return false;
  if (enet_peer_send(peer_, channel, packet) != 0) {
    enet_packet_destroy(packet);
    return false;
  }
  link_.largest_sent = std::max<std::uint32_t>(link_.largest_sent,
                                               static_cast<std::uint32_t>(bytes.size()));
  return true;
}

void Client::push(Event event) { events_.push_back(std::move(event)); }

bool Client::is_other_player(std::uint8_t number) const {
  const Player* known = find_player(number);
  return known != nullptr && !known->self;
}

bool Client::is_host() const {
  return state_ == State::connected && host_number_ != 0 &&
         host_number_ == player_number_;
}

std::optional<proto::FlagEntry> Client::find_flag(const FlagKey& key) const {
  const auto it = flags_.find(key);
  if (it == flags_.end()) return std::nullopt;
  return it->second;
}

void Client::store_flags(std::span<const proto::FlagEntry> flags) {
  // The table holds values; a count delta is a change, never a value.
  for (const proto::FlagEntry& flag : flags) {
    if (flag.kind != proto::FlagKind::count_delta) flags_[key_of(flag)] = flag;
  }
}

void Client::clear_shared() {
  flags_.clear();
  synced_ = false;
  snapshot_flags_.clear();
  snapshot_next_ = 0;
  snapshot_count_ = 0;
  our_join_.reset();
  their_joins_.clear();
  pending_.clear();
  host_number_ = 0;
}

void Client::settle_pending(std::span<const proto::FlagEntry> flags) {
  for (const proto::FlagEntry& flag : flags) pending_.erase(key_of(flag));
}

bool Client::on_host(std::uint8_t number) {
  if (number == host_number_) return false;
  host_number_ = number;
  if (pending_.empty()) return true;
  std::vector<proto::FlagEntry> list;
  list.reserve(pending_.size());
  for (const auto& entry : pending_) list.push_back(entry.second);
  if (number == player_number_) {
    // We are the truth now: what our game asked for is what it keeps. Our
    // count deltas are in our game already; our snapshot carries them.
    std::erase_if(list, is_delta);
    if (list.empty()) {
      pending_.clear();
      return true;
    }
    if (send_flags(list, true)) {
      store_flags(list);
      pending_.clear();
    }
  } else {
    send_flags(list, false);  // still pending until the new host answers
  }
  return true;
}

bool Client::send_flags(std::span<const proto::FlagEntry> flags, bool as_state) {
  // Every part is checked first, so a refused one sends nothing at all.
  std::vector<proto::Message> messages;
  for (std::size_t at = 0; at < flags.size(); at += proto::kMaxFlagEntries) {
    const auto part = flags.subspan(at, std::min(proto::kMaxFlagEntries, flags.size() - at));
    std::vector<proto::FlagEntry> list(part.begin(), part.end());
    proto::Message message;
    if (as_state) {
      message = proto::FlagState{proto::kPlayerSelf, std::move(list)};
    } else {
      message = proto::FlagSet{proto::kPlayerSelf, std::move(list)};
    }
    if (proto::check_received(message, proto::Sender::client,
                              proto::kChannelProgress) != proto::Error::none) {
      return false;
    }
    messages.push_back(std::move(message));
  }
  for (const proto::Message& message : messages) {
    if (!send_raw(message, proto::kChannelProgress)) return false;
  }
  return true;
}

bool Client::set_flags(std::span<const proto::FlagEntry> flags) {
  if (state_ != State::connected || flags.empty()) return false;
  // One entry per (hash, index) with the last value given, in first-seen order.
  std::vector<proto::FlagEntry> list;
  std::map<FlagKey, std::size_t> seen;
  for (const proto::FlagEntry& flag : flags) {
    if (!valid_flag(flag)) return false;
    const auto [it, fresh] = seen.try_emplace(key_of(flag), list.size());
    if (fresh) {
      list.push_back(flag);
    } else if (is_delta(flag) && is_delta(list[it->second])) {
      list[it->second].value = add_delta(list[it->second].value, flag.value);
    } else {
      list[it->second] = flag;
    }
  }
  const bool host = is_host();
  // As host our game is the truth: a count change in it goes out as its
  // total (the watcher does that once it knows it hosts), never as a delta.
  if (host) std::erase_if(list, is_delta);
  if (list.empty()) return true;
  if (!send_flags(list, host)) return false;
  if (host) {
    store_flags(list);
  } else {
    for (const proto::FlagEntry& flag : list) {
      auto [it, fresh] = pending_.try_emplace(key_of(flag), flag);
      if (fresh) continue;
      if (is_delta(flag) && is_delta(it->second)) {
        // Both unanswered: a resend must carry both.
        it->second.value = add_delta(it->second.value, flag.value);
      } else {
        it->second = flag;
      }
    }
  }
  return true;
}

bool Client::send_flag_reset(std::span<const proto::FlagEntry> flags) {
  if (state_ != State::connected || !is_host()) return false;
  std::vector<proto::Message> messages;
  for (std::size_t at = 0; at < flags.size(); at += proto::kMaxFlagEntries) {
    const auto part = flags.subspan(at, std::min(proto::kMaxFlagEntries, flags.size() - at));
    proto::Message message = proto::FlagReset{proto::kPlayerSelf, {part.begin(), part.end()}};
    if (proto::check_received(message, proto::Sender::client, proto::kChannelProgress) !=
        proto::Error::none) {
      return false;
    }
    messages.push_back(std::move(message));
  }
  for (const proto::Message& message : messages) {
    if (!send_raw(message, proto::kChannelProgress)) return false;
  }
  store_flags(flags);
  return true;
}

bool Client::send_flag_snapshot(std::span<const proto::FlagEntry> flags) {
  if (!is_host()) return false;
  const auto parts = proto::split_snapshot(flags);
  if (!parts) return false;
  for (const proto::FlagSnapshot& part : *parts) {
    if (proto::check_received(part, proto::Sender::client,
                              proto::kChannelProgress) != proto::Error::none) {
      return false;
    }
  }
  for (const proto::FlagSnapshot& part : *parts) {
    if (!send_raw(part, proto::kChannelProgress)) return false;
  }
  return true;
}

std::optional<std::uint16_t> Client::join_request_id() const {
  if (!our_join_) return std::nullopt;
  return our_join_->request_id;
}

std::optional<std::uint16_t> Client::request_join(std::uint8_t player) {
  if (state_ != State::connected || !is_other_player(player)) return std::nullopt;
  const std::uint16_t id = next_request_id_;
  proto::JoinPartner request;
  request.other_player = player;
  request.request_id = id;
  request.kind = proto::JoinKind::request;
  if (!send(request)) return std::nullopt;
  next_request_id_ = static_cast<std::uint16_t>(id + 1);
  if (next_request_id_ == 0) next_request_id_ = 1;
  our_join_ = OpenJoin{player, id, now_us()};
  return id;
}

bool Client::answer_join(proto::JoinPartner answer) {
  const auto open = std::find_if(their_joins_.begin(), their_joins_.end(), [&](const OpenJoin& j) {
    return j.player == answer.other_player && j.request_id == answer.request_id;
  });
  if (open == their_joins_.end()) return false;
  if (elapsed(now_us(), open->at_us) >= ms_to_us(config_.join_timeout_ms)) {
    their_joins_.erase(open);  // the asker has given up by now
    return false;
  }
  if (!send(answer)) return false;
  their_joins_.erase(open);
  return true;
}

bool Client::accept_join(std::uint8_t player, std::uint16_t request_id,
                         const proto::JoinTarget& target) {
  proto::JoinPartner answer;
  answer.other_player = player;
  answer.request_id = request_id;
  answer.kind = proto::JoinKind::accept;
  answer.target = target;
  return answer_join(std::move(answer));
}

bool Client::refuse_join(std::uint8_t player, std::uint16_t request_id,
                         proto::JoinRefuseReason reason) {
  proto::JoinPartner answer;
  answer.other_player = player;
  answer.request_id = request_id;
  answer.kind = proto::JoinKind::refuse;
  answer.reason = reason;
  return answer_join(std::move(answer));
}

void Client::check_join_timeouts(std::uint64_t now) {
  const std::uint64_t timeout = ms_to_us(config_.join_timeout_ms);
  if (our_join_ && elapsed(now, our_join_->at_us) >= timeout) {
    Event event;
    event.kind = EventKind::join_timed_out;
    event.player = our_join_->player;
    event.request_id = our_join_->request_id;
    our_join_.reset();
    push(std::move(event));
  }
  std::erase_if(their_joins_, [&](const OpenJoin& j) {
    return elapsed(now, j.at_us) >= timeout;
  });
}

bool Client::apply_shared(const proto::Message& message, std::uint8_t channel,
                          std::optional<Event>& out) {
  Event event;
  event.channel = channel;
  if (const auto* reset = std::get_if<proto::FlagReset>(&message)) {
    // The host's save load: its values replace ours, 0s included.
    if (host_number_ == 0 || reset->player_number != host_number_) return false;
    store_flags(reset->flags);
    settle_pending(reset->flags);
    event.kind = EventKind::flag_reset;
    event.player = reset->player_number;
    event.flags = reset->flags;
  } else if (const auto* state = std::get_if<proto::FlagState>(&message)) {
    // Only the host's, as HOST named it: that may be a host whose
    // PLAYER_LEFT (channel 0) is already in, or us (the server's value on a
    // clash with our snapshot).
    if (host_number_ == 0 || state->player_number != host_number_) return false;
    store_flags(state->flags);
    settle_pending(state->flags);
    event.kind = EventKind::flag_state;
    event.player = state->player_number;
    event.flags = state->flags;
  } else if (const auto* set = std::get_if<proto::FlagSet>(&message)) {
    // The server relays FLAG_SET to the host only, the host's own included.
    // The asker may have left by now (PLAYER_LEFT is on channel 0); the
    // answer is for everyone, so it is still given.
    if (!is_host()) return false;
    std::vector<proto::FlagEntry> answer;
    std::vector<proto::FlagEntry> deltas;
    if (set->player_number == player_number_) {
      // Our own, sent before we knew we host. We have stated what is in the
      // table since; the rest is our own change, which is the truth.
      // A count delta of ours is in our game already: never added twice.
      for (const proto::FlagEntry& asked : set->flags) {
        if (const auto known = find_flag(asked.key)) {
          answer.push_back(*known);
        } else if (!is_delta(asked)) {
          answer.push_back(asked);
        }
      }
    } else {
      for (const proto::FlagEntry& asked : set->flags) {
        // A count delta goes to our game, which adds it; the watcher sends
        // the total as FLAG_STATE (docs/protocol.md, "Counts").
        if (is_delta(asked)) {
          deltas.push_back(asked);
          continue;
        }
        std::optional<proto::FlagEntry> kept =
            flag_answer_ ? flag_answer_(set->player_number, asked) : asked;
        if (kept) {
          kept->key = asked.key;
        }
        // Refused, or a value the protocol would refuse: the table's value.
        if (!kept || !valid_flag(*kept)) kept = find_flag(asked.key);
        if (kept) answer.push_back(*kept);
      }
    }
    if (!answer.empty()) {
      if (send_flags(answer, true)) {
        store_flags(answer);
        settle_pending(answer);
      } else {
        answer.clear();
      }
    }
    if (answer.empty() && deltas.empty()) return true;
    event.kind = EventKind::flag_state;
    event.player = player_number_;
    event.flags = std::move(answer);
    event.flags.insert(event.flags.end(), deltas.begin(), deltas.end());
  } else if (const auto* part = std::get_if<proto::FlagSnapshot>(&message)) {
    if (part->part == 0) {
      snapshot_flags_.clear();
      snapshot_next_ = 0;
      snapshot_count_ = part->part_count;
    }
    if (snapshot_count_ == 0 || part->part != snapshot_next_ ||
        part->part_count != snapshot_count_) {
      snapshot_flags_.clear();  // out of order: wait for a new part 0
      snapshot_next_ = 0;
      snapshot_count_ = 0;
      return false;
    }
    snapshot_flags_.insert(snapshot_flags_.end(), part->flags.begin(), part->flags.end());
    ++snapshot_next_;
    if (snapshot_next_ < snapshot_count_) return true;
    // The snapshot is the whole table: it replaces what was there.
    flags_.clear();
    store_flags(snapshot_flags_);
    settle_pending(snapshot_flags_);
    synced_ = true;
    event.kind = EventKind::flag_snapshot;
    event.flags = std::move(snapshot_flags_);
    snapshot_flags_.clear();
    snapshot_next_ = 0;
    snapshot_count_ = 0;
  } else if (const auto* join = std::get_if<proto::JoinPartner>(&message)) {
    if (join->kind == proto::JoinKind::request) {
      if (!is_other_player(join->player_number) || join->other_player != player_number_) {
        return false;
      }
      std::erase_if(their_joins_, [&](const OpenJoin& j) { return j.player == join->player_number; });
      their_joins_.push_back(OpenJoin{join->player_number, join->request_id, now_us()});
      event.kind = EventKind::join_request;
    } else {
      // on_receive has matched it to our open request. The server's own
      // not_there answer names the player asked, who is not in the room.
      if (join->kind == proto::JoinKind::accept && !join->target) return false;
      our_join_.reset();
      if (join->kind == proto::JoinKind::accept) {
        event.kind = EventKind::join_accepted;
        event.join_target = *join->target;
      } else {
        event.kind = EventKind::join_refused;
        event.join_reason = join->reason;
      }
    }
    event.player = join->player_number;
    event.request_id = join->request_id;
  } else if (const auto* host = std::get_if<proto::Host>(&message)) {
    if (!on_host(host->player_number)) return true;  // the same host again
    event.kind = EventKind::host_changed;
    event.player = host->player_number;
  } else {
    return true;  // nothing shared
  }
  out = std::move(event);
  return true;
}

}  // namespace coop::net
