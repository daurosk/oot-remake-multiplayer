#include "coop/host/session.hpp"

#include <chrono>
#include <utility>

#include "coop/protocol/codec.hpp"

namespace coop::host {

Session::Session(net::Client& client, SessionSettings settings)
    : sampler_(settings.sampler),
      client_(client),
      on_event_(std::move(settings.on_event)),
      presence_(settings.presence) {
  sharing_ = settings.shared.has_value();
  if (settings.shared) {
    watcher_.emplace(std::move(*settings.shared), FlagRole::player);
    watcher_->set_learn_pickups(settings.learn_pickups);
  }
}

FrameReport Session::frame(const std::optional<PlayerReading>& reading, FlagStore* flags) {
  FrameReport report;
  std::vector<Inbound> inbox;
  {
    std::lock_guard lock(net_mutex_);
    report.connected = client_.state() == net::State::connected;
    if (report.connected) report.server_now_us = client_.server_now_us();
    inbox.swap(inbox_);
  }

  // What the network thread handed over, in the order it came. receive()
  // only queues; the game is touched below.
  for (auto& in : inbox) {
    if (const auto* control = std::get_if<Control>(&in)) {
      switch (*control) {
        case Control::reset:
          reset_sampler_ = true;
          if (watcher_) watcher_->reset();
          break;
        case Control::become_host:
          if (watcher_) {
            if (watcher_->role() != FlagRole::host) watcher_->set_role(FlagRole::host);
            // Also when we were host already: after a reconnect the server
            // needs our table again.
            watcher_->request_snapshot();
          }
          break;
        case Control::become_player:
          // Only on a real change: set_role drops a snapshot being held.
          if (watcher_ && watcher_->role() != FlagRole::player) {
            watcher_->set_role(FlagRole::player);
          }
          break;
      }
    } else if (watcher_) {
      watcher_->receive(std::get<protocol::Message>(in));
    }
  }

  std::vector<Outbound> out;
  if (report.connected) {
    if (std::exchange(reset_sampler_, false)) sampler_.reset();
    for (const Outgoing& m : sampler_.frame(reading, report.server_now_us)) {
      protocol::Message message;
      if (protocol::decode(m.bytes, message) == protocol::Error::none) {
        out.emplace_back(std::move(message));
      }
    }
    if (watcher_ && flags != nullptr) {
      // No Link to read: the game is loading (FlagWatcher::set_loading).
      watcher_->set_loading(!reading.has_value());
      watcher_->set_scene(scene_readable_);
      queue_flags(watcher_->frame(*flags), out);
    }
  }
  if (watcher_) {
    report.hosting = watcher_->role() == FlagRole::host;
    report.holding = watcher_->holding();
    report.host_reset = watcher_->take_reset();
    if (const auto seen = watcher_->take_save_load()) {
      report.save_load_flags = seen->flags;
      report.save_load_frames = seen->frames;
    }
  }
  report.queued = out.size();

  if (!out.empty()) {
    {
      std::lock_guard lock(net_mutex_);
      for (auto& o : out) outbox_.push_back(std::move(o));
    }
    wake_.notify_one();
  }
  return report;
}

void Session::queue_flags(const std::vector<Outgoing>& out, std::vector<Outbound>& to) {
  FlagChanges changes;
  FlagTable table;
  bool has_table = false;
  for (const Outgoing& m : out) {
    protocol::Message message;
    if (protocol::decode(m.bytes, message) != protocol::Error::none) continue;
    if (auto* set = std::get_if<protocol::FlagSet>(&message)) {
      changes.flags.insert(changes.flags.end(), set->flags.begin(), set->flags.end());
    } else if (auto* state = std::get_if<protocol::FlagState>(&message)) {
      changes.flags.insert(changes.flags.end(), state->flags.begin(), state->flags.end());
    } else if (auto* reset = std::get_if<protocol::FlagReset>(&message)) {
      // The host's save load: sent as FLAG_RESET, in its own message.
      to.emplace_back(protocol::Message{std::move(*reset)});
    } else if (auto* part = std::get_if<protocol::FlagSnapshot>(&message)) {
      // The client splits the table again; it wants it whole.
      table.flags.insert(table.flags.end(), part->flags.begin(), part->flags.end());
      has_table = true;
    }
  }
  if (!changes.flags.empty()) to.emplace_back(std::move(changes));
  if (has_table) to.emplace_back(std::move(table));
}

void Session::service() {
  std::lock_guard lock(net_mutex_);
  for (auto& out : outbox_) send(out);
  outbox_.clear();
  client_.service(0);
  while (auto event = client_.next_event()) {
    handle(*event);
    if (on_event_) on_event_(*event, client_);
  }
}

void Session::send(Outbound& out) {
  if (auto* message = std::get_if<protocol::Message>(&out)) {
    if (const auto* reset = std::get_if<protocol::FlagReset>(message)) {
      client_.send_flag_reset(reset->flags);  // false when we no longer host
    } else {
      client_.send(*message);
    }
  } else if (auto* changes = std::get_if<FlagChanges>(&out)) {
    // FLAG_SET, or FLAG_STATE when we host. When it cannot go (a drop), a
    // player's watcher still holds the changes as unanswered and sends them
    // after the reconnect; a host sends its whole table then.
    client_.set_flags(changes->flags);
  } else {
    // False when we no longer host; the new host sends its own.
    client_.send_flag_snapshot(std::get<FlagTable>(out).flags);
  }
}

void Session::handle(const net::Event& event) {
  using net::EventKind;
  const bool sharing = sharing_;
  switch (event.kind) {
    case EventKind::welcomed:
    case EventKind::reconnected: {
      inbox_.emplace_back(Control::reset);
      std::lock_guard lock(presence_mutex_);
      presence_.on_welcome(event.welcome);
      break;
    }
    case EventKind::disconnected: {
      std::lock_guard lock(presence_mutex_);
      presence_.clear();
      break;
    }
    case EventKind::message: {
      {
        std::lock_guard lock(presence_mutex_);
        presence_.apply(event.message);
      }
      // FLAG_STATE comes to the watcher as its flag_state event, which the
      // client also gives for its own answers as host.
      if (sharing && std::holds_alternative<protocol::FlagSnapshot>(event.message)) {
        inbox_.emplace_back(event.message);
      }
      break;
    }
    case EventKind::flag_state:
      if (sharing) inbox_.emplace_back(protocol::Message{protocol::FlagState{event.player, event.flags}});
      break;
    case EventKind::flag_reset:
      if (sharing) inbox_.emplace_back(protocol::Message{protocol::FlagReset{event.player, event.flags}});
      break;
    case EventKind::host_changed:
      inbox_.emplace_back(client_.is_host() ? Control::become_host : Control::become_player);
      break;
    default:
      break;
  }
}

void Session::wait(std::uint32_t max_ms) {
  std::unique_lock lock(net_mutex_);
  wake_.wait_for(lock, std::chrono::milliseconds(max_ms),
                 [this] { return woken_ || !outbox_.empty(); });
  woken_ = false;
}

void Session::wake() {
  {
    std::lock_guard lock(net_mutex_);
    woken_ = true;
  }
  wake_.notify_all();
}

}  // namespace coop::host
