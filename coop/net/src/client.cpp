#include "coop/net/client.hpp"

// ENet pulls in windows.h on Windows; keep its min and max macros out.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <enet/enet.h>

#include <algorithm>
#include <chrono>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include "coop/protocol/codec.hpp"

namespace coop::net {

namespace proto = coop::protocol;

namespace {

std::uint64_t steady_now_us() {
  const auto since = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(since).count());
}

constexpr std::uint64_t ms_to_us(std::uint32_t ms) {
  return static_cast<std::uint64_t>(ms) * 1000u;
}

// The longest packet a message can fill: the 4-byte header and a body of
// at most 65535 bytes. ENet refuses anything longer before assembling it.
constexpr std::size_t kMaxPacketBytes = 4 + 0xffff;

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

// Channels 0 to 2 are reliable and ordered, 3 and 4 unreliable and sequenced
// (ENet's default for a packet without flags).
enet_uint32 packet_flags(std::uint8_t channel) {
  return channel < proto::kChannelPlayerSamples ? static_cast<enet_uint32>(ENET_PACKET_FLAG_RELIABLE) : 0u;
}

}  // namespace

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
  }
  return "unknown";
}

std::string_view refuse_reason_text(proto::RefuseReason reason) {
  switch (reason) {
    case proto::RefuseReason::wrong_protocol_version: return "wrong protocol version";
    case proto::RefuseReason::wrong_game_build: return "wrong game build";
    case proto::RefuseReason::server_full: return "server full";
    case proto::RefuseReason::name_taken: return "name taken";
    case proto::RefuseReason::bad_name: return "bad name";
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
}

void Client::begin_attempt() {
  attempt_started_us_ = now_us();
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
  players_.clear();
  Event event;
  event.kind = was_connected ? EventKind::disconnected : EventKind::connect_failed;
  event.drop_reason = reason;
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
          if (send_raw(hello, proto::channel_of(hello))) {
            hello_sent_us_ = now_us();
            state_ = State::handshaking;
          } else {
            end_attempt(DropReason::connection_lost);
          }
        }
        break;
      case ENET_EVENT_TYPE_RECEIVE:
        if (event.peer == peer_) {
          on_receive(event.packet->data, event.packet->dataLength,
                     event.channelID);
        }
        enet_packet_destroy(event.packet);
        break;
      case ENET_EVENT_TYPE_DISCONNECT:
        if (event.peer == peer_) {
          peer_ = nullptr;  // ENet has already reset it
          if (state_ == State::connecting || state_ == State::handshaking ||
              state_ == State::connected) {
            end_attempt(DropReason::connection_lost);
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
      break;
    case State::waiting_retry:
      if (now >= retry_at_us_) begin_attempt();
      break;
    case State::idle:
    case State::refused:
    case State::stopped:
      break;
  }
  enet_host_flush(host_);
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
    } else if (const auto* refused = std::get_if<proto::Refused>(&message)) {
      if (peer_ != nullptr) {
        enet_peer_disconnect_now(peer_, 0);
        peer_ = nullptr;
      }
      state_ = State::refused;
      Event event;
      event.kind = EventKind::refused;
      event.refuse_reason = refused->reason;
      push(std::move(event));
    } else {
      ++rejected_;  // nothing else is expected before the answer to HELLO
    }
    return;
  }

  if (state_ != State::connected) {
    ++rejected_;
    return;
  }

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

  if (std::holds_alternative<proto::Welcome>(message) ||
      std::holds_alternative<proto::Refused>(message)) {
    ++rejected_;  // the handshake is over
    return;
  }

  std::optional<Event> room;
  if (!apply_room(message, channel, room)) {
    ++rejected_;
    return;
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
  } else if (const auto* left = std::get_if<proto::PlayerLeft>(&message)) {
    const Player* known = find_player(left->player_number);
    if (known == nullptr || known->self) return false;
    event.kind = EventKind::player_left;
    event.player = left->player_number;
    event.nickname = known->nickname;
    event.leave_reason = left->reason;
    std::erase_if(players_, [&](const Player& p) { return p.number == left->player_number; });
  } else if (const auto* status = std::get_if<proto::PlayerStatus>(&message)) {
    Player* known = player_entry(status->player_number);
    if (known == nullptr || known->self) return false;
    known->status = status->flags;
    event.kind = EventKind::player_status;
    event.player = status->player_number;
    event.status_flags = status->flags;
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

  players_.clear();
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
  return true;
}

bool Client::send_chat(std::string_view text) {
  const std::size_t chars = utf8_chars(text);
  if (chars == 0 || chars > kMaxChatChars) return false;
  return send(proto::Chat{proto::kPlayerSelf, std::string(text)});
}

bool Client::send_raw(const proto::Message& message, std::uint8_t channel) {
  if (peer_ == nullptr) return false;
  std::vector<std::uint8_t> bytes;
  if (proto::encode(message, bytes) != proto::Error::none) return false;
  ENetPacket* packet =
      enet_packet_create(bytes.data(), bytes.size(), packet_flags(channel));
  if (packet == nullptr) return false;
  if (enet_peer_send(peer_, channel, packet) != 0) {
    enet_packet_destroy(packet);
    return false;
  }
  return true;
}

void Client::push(Event event) { events_.push_back(std::move(event)); }

}  // namespace coop::net
