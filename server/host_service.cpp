// HostService: coop_server's hosting loop as an object (host_service.hpp).
// The lines it says are the ones coop_server always printed.
#include "host_service.hpp"

#include <chrono>
#include <exception>
#include <utility>

#include "coop/protocol/messages.hpp"
#include "join_helper.hpp"
#include "nat_net.hpp"
#include "punch.hpp"
#include "relay_link.hpp"
#include "relay_wire.hpp"
#include "upnp.hpp"

namespace coop::server {

namespace {

std::uint64_t steady_ms() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
}

std::string count(std::uint64_t v) { return std::to_string(static_cast<unsigned long long>(v)); }

}  // namespace

std::string show_ipv4(std::uint32_t ip) {
  return std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 255u) + "." +
         std::to_string((ip >> 8) & 255u) + "." + std::to_string(ip & 255u);
}

HostService::HostService(HostServiceConfig config) : config_(std::move(config)) {}

HostService::~HostService() { stop(); }

std::uint64_t HostService::now_ms() const { return config_.now_ms ? config_.now_ms() : steady_ms(); }

void HostService::say(const std::string& text) const {
  if (config_.log) config_.log(text);
}

void HostService::set_message(const std::string& text, HostProblem problem) {
  std::lock_guard lock(status_mutex_);
  status_.message = text;
  status_.problem = problem;
}

HostStatus HostService::status() const {
  std::lock_guard lock(status_mutex_);
  HostStatus out = status_;
  return out;
}

std::uint16_t HostService::port() const { return server_ ? server_->port() : 0; }

const Session* HostService::session() const { return server_ ? &server_->session() : nullptr; }

bool HostService::start(std::string& error) {
  auto fail = [&](const std::string& why, HostProblem problem) {
    error = why;
    std::lock_guard lock(status_mutex_);
    status_.state = HostState::kFailed;
    status_.message = why;
    status_.problem = problem;
    return false;
  };
  if (server_) return fail("the session is already running", HostProblem::kNone);
  if (config_.session.join_password.empty()) {
    return fail("a join password is needed: every session has one, and it also encrypts the traffic",
                HostProblem::kNoPassword);
  }
  if (!config_.session.log) config_.session.log = config_.log;

  const std::uint16_t tries = config_.port_tries == 0 ? 1 : config_.port_tries;
  for (std::uint16_t i = 0; i < tries && !server_; ++i) {
    ServerOptions options;
    // Port 0 (any free one) is tried once.
    options.port = config_.port == 0 ? 0 : static_cast<std::uint16_t>(config_.port + i);
    options.max_connections = config_.max_connections;
    options.bind = config_.bind;
    options.session = config_.session;
    auto server = std::make_unique<EnetServer>(std::move(options));
    if (server->start()) server_ = std::move(server);
    if (config_.port == 0) break;
  }
  if (!server_) {
    std::string why = "cannot listen on port " + std::to_string(config_.port);
    if (tries > 1) why += " or the " + std::to_string(tries - 1) + " after it";
    why += ": another program has it (is a co-op server window open already?)";
    port_failed_ = true;
    return fail(why, HostProblem::kPortTaken);
  }
  {
    std::lock_guard lock(status_mutex_);
    status_.state = HostState::kHosting;
    status_.port = server_->port();
    status_.message.clear();
  }

  // On every network adapter (a VPN's too) unless bind, with VPN-safe
  // datagrams (issue 143).
  const std::string where = config_.bind ? "only " + show_ipv4(*config_.bind) : "all network adapters";
  say("listening on port " + std::to_string(server_->port()) + " (" + where + ", datagrams of " +
      std::to_string(coop::protocol::kDatagramBytes) + " bytes)");
  say("players must give the join password to join; the traffic is encrypted");
  if (config_.session.pvp) say("PvP is on: players can hurt each other");
  if (config_.session.nearest_owner) say("enemies belong to the nearest player's game (--owner nearest)");
  if (!config_.session.allowed.empty()) {
    say("only the " + std::to_string(config_.session.allowed.size()) +
        " address range(s) given with --allow may connect");
  }

  if (config_.upnp) {
    upnp::Config c;
    c.internal_port = server_->port();
    c.external_port = server_->port();
    c.log = config_.log;
    mapping_ = std::make_unique<upnp::Mapping>(c);
    if (!mapping_->open()) {
      mapping_.reset();
      set_message("the router did not open the port (UPnP is off on it, or it said no)", HostProblem::kUpnp);
    } else {
      std::lock_guard lock(status_mutex_);
      status_.upnp_open = true;
      status_.external = mapping_->external_address();
    }
  }

  if (!config_.relay_via.empty()) {
    relay::RelayLinkConfig c;
    if (!relay::parse_relay_address(config_.relay_via, c.relay_host, c.relay_port)) {
      say("relay: '" + config_.relay_via + "' is not a relay address (name or IPv4, optional :port)");
      set_message("'" + config_.relay_via + "' is not a relay address", HostProblem::kRelay);
    } else {
      c.server_port = server_->port();
      c.key = config_.relay_key;
      c.log = [this](const std::string& text) { say("relay: " + text); };
      const std::string relay_text = config_.relay_via;
      c.on_code = [this, relay_text](const std::string& code) {
        const std::string shown = relay::show_code(code);
        say("relay: join code " + shown + ". Give your friends " + shown + "@" + relay_text +
            " (Join.bat takes it as the server address)");
        std::lock_guard lock(status_mutex_);
        status_.relay_code = shown + "@" + relay_text;
      };
      link_ = std::make_unique<relay::RelayLink>(c);
      std::string why;
      if (!link_->start(why)) {
        say("relay: " + why);
        set_message("relay: " + why, HostProblem::kRelay);
        link_.reset();
      } else {
        say("relay: asking " + config_.relay_via + " for a join code");
      }
    }
  }

  // Invite code (punch.hpp): STUN from the game port, then the code, then
  // answer codes.
  if (config_.invite) {
    nat::HostConfig c;
    c.password = config_.session.join_password;
    c.stun_servers = config_.stun_servers;
    c.allow_private = config_.stun_allow_private;
    c.remember_friends = config_.remember_friends;
    EnetServer* server = server_.get();
    c.send = [server](nat::Addr to, std::span<const std::uint8_t> data) { server->send_raw(nat::to_enet(to), data); };
    c.log = config_.log;
    rendezvous_ = std::make_unique<nat::HostRendezvous>(c);
    nat::HostRendezvous* r = rendezvous_.get();
    server_->set_raw_handler([this, r](const ENetAddress& from, std::span<const std::uint8_t> data) {
      return r->on_datagram(nat::from_enet(from), data, now_ms());
    });
    say("invite: asking a public STUN server for this PC's internet address (it sees the address, no game data)");
    rendezvous_->start(now_ms());
  }
  enemies_at_ = now_ms();
  return true;
}

void HostService::finish_invite() {
  invite_shown_ = true;
  std::optional<nat::Addr> where;
  const auto seen = rendezvous_->public_address();
  if (mapping_) {
    const auto ext = upnp::parse_ipv4(mapping_->external_address());
    if (ext && (nat::is_public(*ext) || config_.stun_allow_private)) where = nat::Addr{*ext, server_->port()};
    if (where && seen && seen->ip != where->ip) {
      say("invite: note: the router's internet address is not the one the STUN server saw (two routers in a "
          "row); the invite uses the router's");
    }
  }
  if (!where && seen && (nat::is_public(seen->ip) || config_.stun_allow_private)) where = *seen;
  const auto code = where ? nat::make_invite(nat::Invite{*where, config_.session.join_password}) : std::nullopt;
  if (!code) {
    const std::string why = "no invite code: no STUN server answered (no internet, or a firewall blocks UDP) and "
                            "the router did not forward the port. " + nat::no_way_help();
    say("invite: " + why);
    std::lock_guard lock(status_mutex_);
    status_.invite_done = true;
    status_.message = why;
    status_.problem = HostProblem::kNoInvite;
    return;
  }
  if (config_.log_invite) {
    say("invite code: " + *code);
    say("invite: send the invite code to your friends; they paste it into Join.bat. It holds your internet "
        "address and the join password, so send it only to friends. It stops working when this window closes, "
        "and changes each time.");
    say("invite: if a friend's Join.bat window shows an ANSWER CODE, paste it here (right-click) and press Enter.");
  } else {
    say("invite: the invite code is made (not written here: it holds the join password)");
  }
  const bool symmetric = rendezvous_->symmetric() && !mapping_;
  if (symmetric) {
    say("invite: note: your router gives every address its own port (symmetric NAT), so friends may not get "
        "through; upnp = yes or a VPN helps (docs/play-online.md)");
  }
  std::lock_guard lock(status_mutex_);
  status_.invite = *code;
  status_.invite_done = true;
  if (symmetric) {
    status_.message = "your router gives every address its own port, so friends may not get through";
    status_.problem = HostProblem::kSymmetric;
  }
}

nat::CodeError HostService::add_answer(const std::string& typed) {
  if (!rendezvous_ || stopped_) return nat::CodeError::kNotACode;
  const nat::CodeError e = rendezvous_->add_answer(typed, now_ms());
  if (e != nat::CodeError::kOk) say("answer code not used: " + nat::explain(e));
  return e;
}

void HostService::say_counts() {
  // Counts a window or a test can read while the server runs (it is often
  // closed, not stopped): each host save load, and messages over a player's
  // rate (the first, then every 100th).
  const Stats& now = server_->session().stats();
  if (now.host_resets != shown_resets_) {
    shown_resets_ = now.host_resets;
    say("the host loaded a save: the group follows it (host_resets " + count(shown_resets_) + ")");
  }
  if (now.rate_limited != shown_limited_ &&
      (shown_limited_ == 0 || now.rate_limited / 100 != shown_limited_ / 100)) {
    say("messages over a player's rate dropped so far: " + count(now.rate_limited));
  }
  shown_limited_ = now.rate_limited;
  // Enemy counts (issues 105, 133), at most every 5 s while they change.
  const std::uint64_t enemies =
      now.group_links + now.group_refused + now.damage_relayed + now.damage_dropped + now.dead_told;
  if (config_.counts && enemies != shown_enemies_ && now_ms() - enemies_at_ >= 5000) {
    shown_enemies_ = enemies;
    enemies_at_ = now_ms();
    say("enemies: group entries taken " + count(now.group_links) + ", refused " + count(now.group_refused) +
        "; damage relayed " + count(now.damage_relayed) + ", dropped " + count(now.damage_dropped) +
        "; kills told late " + count(now.dead_told));
  }
}

void HostService::service(std::uint32_t wait_ms) {
  if (!server_ || stopped_) return;
  // Relayed datagrams wait for this round: keep it short with a relay.
  server_->service(link_ && wait_ms > 1 ? 1 : wait_ms);
  if (link_) link_->service();
  if (mapping_) mapping_->renew_if_due();
  if (rendezvous_) {
    rendezvous_->service(now_ms());
    if (!invite_shown_ && rendezvous_->stun_done()) finish_invite();
  }
  say_counts();
  const std::uint64_t now = now_ms();
  const std::uint64_t bytes = server_->session().stats().stream_bytes;
  std::lock_guard lock(status_mutex_);
  if (stream_second_ == 0 || now - stream_second_ >= 1000) {
    if (stream_second_ != 0) {
      const std::uint64_t ms = now - stream_second_;
      status_.stream_kbps = static_cast<std::uint32_t>((bytes - stream_bytes_at_) * 8 / ms);  // bits per ms = kbit/s
    }
    stream_second_ = now;
    stream_bytes_at_ = bytes;
  }
  status_.players = server_->session().player_count();
}

void HostService::stop() {
  if (!server_ || stopped_) return;
  stopped_ = true;
  if (link_) link_->stop();
  if (mapping_) mapping_->close();
  const Stats& stats = server_->session().stats();
  say("stopping; relayed " + count(stats.relayed) + ", dropped " + count(stats.dropped()) + ", half-open refused " +
      count(stats.half_open_limited) + ", not allowed " + count(stats.not_allowed));
  say("host save loads " + count(stats.host_resets) + ", over the rate " + count(stats.rate_limited));
  say("flags relayed " + count(stats.flag_relayed) + ", refused " + count(stats.flag_refused) + ", clashes " +
      count(stats.flag_clashes) + "; join relayed " + count(stats.join_relayed) + ", not there " +
      count(stats.join_not_there) + ", dropped " + count(stats.join_dropped));
  std::lock_guard lock(status_mutex_);
  status_.state = HostState::kStopped;
  status_.players = 0;
}

}  // namespace coop::server
