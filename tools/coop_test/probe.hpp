#pragma once

// One test client: a coop::net::Client that sends the stage 0 test traffic
// and fake player samples, and measures what it gets from the others.
//
// Sent, once welcomed and the clock has a first estimate:
// - TEST_PAYLOAD at 60 a second on channel 3 (unreliable) and 1 a second on
//   channel 0 (reliable), each with its own sequence number; the first
//   filler byte is the sender's player number, so a receiver can tell whose
//   it is (the message has no player field);
// - PLAYER_SAMPLE at 60 a second along a known circle (path.hpp).
//
// Sample times are the sender's estimate of the server clock, so every
// receiver can place them on one time line (see docs/soak.md).
//
// Measured, for each other player:
// - sequence gaps, out-of-order and repeated arrivals on each kind;
// - 60 times a second, the position shown after interpolation, a fixed delay
//   behind in server time, against where the path really was then.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "coop/net/client.hpp"
#include "path.hpp"

namespace coop::test_tool {

struct Spread {
  double min = std::numeric_limits<double>::infinity();
  double max = -std::numeric_limits<double>::infinity();
  double sum = 0;
  std::uint64_t count = 0;

  void add(double value);
  Spread& operator+=(const Spread& other);
  double avg() const { return count ? sum / static_cast<double>(count) : 0; }
};

struct SequenceCounts {
  std::uint64_t received = 0;  // different sequence numbers
  std::uint64_t lost = 0;      // gaps between the first and the highest seen
  std::uint64_t out_of_order = 0;
  std::uint64_t repeated = 0;

  SequenceCounts& operator+=(const SequenceCounts& other);
};

// The sequence numbers seen from one sender on one kind of message, from the
// first one seen (a receiver may join after the sender started).
class SequenceTracker {
 public:
  void add(std::uint32_t sequence);
  const SequenceCounts& counts() const { return counts_; }
  std::optional<std::uint32_t> highest() const;

 private:
  bool any_ = false;
  std::uint32_t first_ = 0;
  std::uint32_t highest_ = 0;
  std::vector<bool> seen_;  // index: sequence - first_
  SequenceCounts counts_;
};

struct ShownError {
  Spread units;                    // distance shown to true, game units
  std::uint64_t interpolated = 0;  // frames between two samples
  std::uint64_t extrapolated = 0;  // frames past the last sample, within the limit
  std::uint64_t held = 0;          // frames past the limit: the puppet holds

  ShownError& operator+=(const ShownError& other);
};

struct ProbeConfig {
  std::string host = "127.0.0.1";
  std::uint16_t port = protocol::kDefaultPort;
  std::string nickname;
};

class Probe {
 public:
  explicit Probe(ProbeConfig config);

  bool start();
  void stop();

  // Runs the client and handles what arrived. When `sending`, also sends what
  // is due and measures the shown path (a sender that stopped is not shown).
  // Call every millisecond or so.
  void step(bool sending);

  const ProbeConfig& config() const { return config_; }
  const net::Client& client() const { return client_; }
  std::uint8_t number() const { return client_.player_number(); }

  // The last reliable sequence sent, nothing before the first.
  std::optional<std::uint32_t> last_reliable_sent() const;
  // The highest reliable sequence received from that player now in the
  // session, nothing if none.
  std::optional<std::uint32_t> last_reliable_from(std::uint8_t player) const;

  // Totals over every sender, including ones that left.
  SequenceCounts reliable() const;
  SequenceCounts unreliable() const;
  SequenceCounts samples() const;
  ShownError shown() const;

  const Spread& round_trip_ms() const { return round_trip_ms_; }
  const Spread& offset_ms() const { return offset_ms_; }
  std::optional<double> last_offset_ms() const { return last_offset_ms_; }
  std::uint32_t welcomes() const { return welcomes_; }
  std::uint32_t disconnects() const { return disconnects_; }
  std::uint32_t failed_attempts() const { return failed_attempts_; }
  std::optional<protocol::RefuseReason> refused() const { return refused_; }
  std::uint64_t send_failures() const { return send_failures_; }
  std::uint64_t unattributed() const { return unattributed_; }
  // Players this probe has heard anything from since it started.
  const std::vector<std::uint8_t>& heard_from() const { return heard_from_; }

  // `extra_reliable_lost`: reliable messages the caller knows were sent to
  // this probe and never came (main counts them after the drain).
  void print(std::FILE* out, std::uint64_t extra_reliable_lost) const;

 private:
  struct Remote {
    SequenceTracker reliable;
    SequenceTracker unreliable;
    SequenceTracker samples;
    std::deque<PathPoint> buffer;  // by time, at most kSampleBuffer
    ShownError shown;
  };

  void handle(const net::Event& event);
  void on_message(const protocol::Message& message, std::uint8_t channel);
  Remote& remote(std::uint8_t player);
  void archive(std::uint8_t player);
  void archive_all();
  void send_due(std::uint64_t now);
  void render(std::uint64_t now);
  bool send(const protocol::Message& message, std::uint8_t channel);

  ProbeConfig config_;
  net::Client client_;
  std::map<std::uint8_t, Remote> remotes_;
  // Counts from senders that left, or from before this probe was dropped.
  SequenceCounts archived_reliable_;
  SequenceCounts archived_unreliable_;
  SequenceCounts archived_samples_;
  ShownError archived_shown_;

  std::uint32_t reliable_sequence_ = 0;
  std::uint32_t unreliable_sequence_ = 0;
  std::uint32_t sample_sequence_ = 0;
  bool sent_reliable_ = false;
  bool schedule_started_ = false;
  std::uint64_t next_reliable_ = 0;
  std::uint64_t next_unreliable_ = 0;
  std::uint64_t next_sample_ = 0;
  std::uint64_t next_render_ = 0;
  std::uint64_t next_clock_ = 0;

  Spread round_trip_ms_;
  Spread offset_ms_;
  std::optional<double> last_offset_ms_;
  std::uint32_t welcomes_ = 0;
  std::uint32_t disconnects_ = 0;
  std::uint32_t failed_attempts_ = 0;
  std::optional<protocol::RefuseReason> refused_;
  std::uint64_t send_failures_ = 0;
  std::uint64_t unattributed_ = 0;
  std::vector<std::uint8_t> heard_from_;
};

}  // namespace coop::test_tool
