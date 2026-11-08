#include "session.hpp"

#include <algorithm>
#include <utility>

namespace coop::server {

using namespace coop::protocol;

namespace {

char ascii_lower(char ch) {
  return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
}

// Nicknames are compared without ASCII case, so "Link" and "link" clash.
bool same_name(const std::string& a, const std::string& b) {
  return a.size() == b.size() &&
         std::equal(a.begin(), a.end(), b.begin(),
                    [](char x, char y) { return ascii_lower(x) == ascii_lower(y); });
}

// The codec already refuses empty names and control characters; a name of
// nothing but spaces is refused here.
bool blank_name(const std::string& name) {
  return std::all_of(name.begin(), name.end(), [](char ch) { return ch == ' '; });
}

}  // namespace

std::uint64_t Stats::dropped() const {
  std::uint64_t total = before_welcome + repeated_hello + after_refused + rate_limited;
  for (std::uint64_t n : by_error) total += n;
  return total;
}

Session::Session(SessionConfig config) : config_(std::move(config)) {}

void Session::connected(ConnectionId id, std::uint64_t now_us) {
  Connection c;
  c.connected_at = now_us;
  c.last_heard = now_us;
  connections_[id] = std::move(c);
}

void Session::received(ConnectionId id, std::uint8_t channel,
                       std::span<const std::uint8_t> bytes, std::uint64_t now_us) {
  auto it = connections_.find(id);
  if (it == connections_.end()) return;  // the transport's mistake, not the peer's
  Connection& c = it->second;
  c.last_heard = now_us;

  if (c.state == State::closing) {
    ++stats_.after_refused;
    return;
  }

  // A HELLO of another protocol version may not decode at all; its first
  // two body bytes are the version in every version, so refuse it properly.
  if (c.state == State::awaiting_hello) {
    if (auto version = hello_protocol_version(bytes);
        version && *version != kProtocolVersion) {
      refuse(id, c, RefuseReason::wrong_protocol_version, now_us);
      return;
    }
  }

  Message message;
  Error error = decode(bytes, message);
  if (error == Error::none) error = check_received(message, Sender::client, channel);
  if (error != Error::none) {
    ++stats_.by_error[static_cast<std::size_t>(error)];
    return;
  }

  if (c.state == State::awaiting_hello) {
    if (const auto* hello = std::get_if<Hello>(&message)) {
      on_hello(id, c, *hello, now_us);
    } else {
      ++stats_.before_welcome;
    }
    return;
  }
  on_player_message(id, c, channel, message, now_us);
}

void Session::on_hello(ConnectionId id, Connection& c, const Hello& hello,
                       std::uint64_t now_us) {
  std::string expected = config_.game_build_id;
  if (expected.empty()) {
    expected = player_count() == 0 ? hello.game_build_id : session_game_build_;
  }
  if (auto reason = check_hello(hello, expected)) {
    refuse(id, c, *reason, now_us);
    return;
  }
  if (player_count() >= kMaxPlayers) {
    refuse(id, c, RefuseReason::server_full, now_us);
    return;
  }
  if (blank_name(hello.nickname)) {
    refuse(id, c, RefuseReason::bad_name, now_us);
    return;
  }
  if (name_in_use(hello.nickname)) {
    refuse(id, c, RefuseReason::name_taken, now_us);
    return;
  }

  Welcome welcome;
  welcome.player_number = free_number();
  welcome.server_time_us = now_us;
  welcome.tick_rate = config_.tick_rate;
  for (const auto& entry : connections_) {
    const Connection& other = entry.second;
    if (other.state == State::joined) welcome.players.push_back({other.number, other.nickname});
  }
  std::sort(welcome.players.begin(), welcome.players.end(),
            [](const PlayerInfo& a, const PlayerInfo& b) { return a.number < b.number; });

  c.state = State::joined;
  c.number = welcome.player_number;
  c.nickname = hello.nickname;
  c.last_ping = now_us;
  c.reliable_budget = 2.0 * config_.reliable_per_second;
  c.unreliable_budget = 2.0 * config_.unreliable_per_second;
  c.budget_time = now_us;
  if (player_count() == 1) session_game_build_ = hello.game_build_id;

  send(id, kChannelSession, welcome);
  send_to_others(id, kChannelSession, PlayerJoined{c.number, c.nickname});
  log("player " + std::to_string(c.number) + " joined: " + c.nickname);
}

void Session::on_player_message(ConnectionId id, Connection& c, std::uint8_t channel,
                                Message& message, std::uint64_t now_us) {
  if (std::holds_alternative<Pong>(message)) return;  // our PING answered
  if (!within_rate(c, channel, now_us)) {
    ++stats_.rate_limited;
    return;
  }
  if (const auto* ping = std::get_if<Ping>(&message)) {
    send(id, kChannelSession, Pong{ping->sender_time_us, now_us});
    return;
  }
  if (std::holds_alternative<Hello>(message)) {
    ++stats_.repeated_hello;
    return;
  }
  // PLAYER_STATUS, CHAT and PLAYER_SAMPLE carry the sender: the server writes
  // it from the connection, never trusting the message. TEST_PAYLOAD has no
  // player field and goes on as it came. check_received has already refused
  // every server-only type, so nothing else reaches here.
  if (sender_of(message)) set_sender(message, c.number);
  send_to_others(id, channel, message);
  ++stats_.relayed;
}

bool Session::within_rate(Connection& c, std::uint8_t channel, std::uint64_t now_us) {
  double seconds = static_cast<double>(now_us - c.budget_time) / 1e6;
  c.budget_time = now_us;
  auto refill = [seconds](double& budget, std::uint32_t per_second) {
    budget = std::min(budget + seconds * per_second, 2.0 * per_second);
  };
  refill(c.reliable_budget, config_.reliable_per_second);
  refill(c.unreliable_budget, config_.unreliable_per_second);
  double& budget = channel <= kChannelOwnership ? c.reliable_budget : c.unreliable_budget;
  if (budget < 1.0) return false;
  budget -= 1.0;
  return true;
}

void Session::refuse(ConnectionId id, Connection& c, RefuseReason reason,
                     std::uint64_t now_us) {
  send(id, kChannelSession, Refused{reason});
  actions_.push_back(DisconnectAction{id});
  c.state = State::closing;
  c.refused_at = now_us;
  log("refused a connection, reason " + std::to_string(static_cast<int>(reason)));
}

void Session::disconnected(ConnectionId id, std::uint64_t now_us) {
  auto it = connections_.find(id);
  if (it == connections_.end()) return;
  bool silent = now_us - it->second.last_heard >= config_.silence_timeout_us / 2;
  remove_player(id, silent ? LeaveReason::timed_out : LeaveReason::left);
}

void Session::remove_player(ConnectionId id, LeaveReason reason) {
  auto it = connections_.find(id);
  if (it == connections_.end()) return;
  Connection c = std::move(it->second);
  connections_.erase(it);
  if (c.state != State::joined) return;
  send_to_others(id, kChannelSession, PlayerLeft{c.number, reason});
  if (player_count() == 0) session_game_build_.clear();
  log("player " + std::to_string(c.number) + " left: " + c.nickname +
      (reason == LeaveReason::timed_out ? " (timed out)" : ""));
}

void Session::tick(std::uint64_t now_us) {
  std::vector<ConnectionId> late_hello;  // not a player: dropped without PLAYER_LEFT
  std::vector<ConnectionId> silent;
  for (auto& [id, c] : connections_) {
    if (c.state == State::awaiting_hello &&
        now_us - c.connected_at >= config_.hello_timeout_us) {
      late_hello.push_back(id);
    } else if (c.state == State::closing &&
               now_us - c.refused_at >= config_.refused_grace_us) {
      late_hello.push_back(id);  // refused, but the transport never closed it
    } else if (c.state == State::joined) {
      if (now_us - c.last_heard >= config_.silence_timeout_us) {
        silent.push_back(id);
      } else if (now_us - c.last_ping >= config_.ping_interval_us) {
        c.last_ping = now_us;
        send(id, kChannelSession, Ping{now_us});
      }
    }
  }
  for (ConnectionId id : late_hello) {
    actions_.push_back(DisconnectAction{id});
    remove_player(id, LeaveReason::timed_out);
  }
  for (ConnectionId id : silent) {
    actions_.push_back(DisconnectAction{id});
    remove_player(id, LeaveReason::timed_out);
  }
}

std::vector<Action> Session::take_actions() { return std::exchange(actions_, {}); }

std::size_t Session::player_count() const {
  return static_cast<std::size_t>(std::count_if(
      connections_.begin(), connections_.end(),
      [](const auto& entry) { return entry.second.state == State::joined; }));
}

std::optional<std::uint8_t> Session::player_number(ConnectionId id) const {
  auto it = connections_.find(id);
  if (it == connections_.end() || it->second.state != State::joined) return std::nullopt;
  return it->second.number;
}

bool Session::name_in_use(const std::string& nickname) const {
  return std::any_of(connections_.begin(), connections_.end(), [&](const auto& entry) {
    return entry.second.state == State::joined && same_name(entry.second.nickname, nickname);
  });
}

std::uint8_t Session::free_number() const {
  for (std::uint8_t n = 1; n <= kMaxPlayers; ++n) {
    bool used = std::any_of(connections_.begin(), connections_.end(), [&](const auto& entry) {
      return entry.second.state == State::joined && entry.second.number == n;
    });
    if (!used) return n;
  }
  return 0;  // unreachable: on_hello refuses a full session first
}

void Session::send(ConnectionId to, std::uint8_t channel, const Message& m) {
  SendAction action{to, channel, {}};
  if (encode(m, action.bytes) != Error::none) return;  // our own message; cannot happen
  actions_.push_back(std::move(action));
}

void Session::send_to_others(ConnectionId except, std::uint8_t channel, const Message& m) {
  std::vector<std::uint8_t> bytes;
  if (encode(m, bytes) != Error::none) return;
  for (const auto& [id, c] : connections_) {
    if (id != except && c.state == State::joined) {
      actions_.push_back(SendAction{id, channel, bytes});
    }
  }
}

void Session::log(const std::string& line) const {
  if (config_.log) config_.log(line);
}

}  // namespace coop::server
