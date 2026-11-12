// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_time_sync.h"

#include <cstdlib>
#include <variant>

namespace CoopTime {

namespace proto = coop::protocol;

std::int32_t Ahead(std::uint32_t a, std::uint32_t b) {
    const auto day = static_cast<std::int32_t>(proto::kSecondsPerDay);
    std::int32_t d = static_cast<std::int32_t>(b % proto::kSecondsPerDay) -
                     static_cast<std::int32_t>(a % proto::kSecondsPerDay);
    if (d >= day / 2) {
        d -= day;
    } else if (d < -day / 2) {
        d += day;
    }
    return d;
}

void Sync::Receive(const proto::Message& message) {
    const auto* t = std::get_if<proto::TimeOfDay>(&message);
    if (t == nullptr || t->seconds >= proto::kSecondsPerDay) {
        return;
    }
    std::lock_guard lock(inbox_mutex_);
    if (inbox_.size() < 256) {
        inbox_.push_back(*t);
    }
}

void Sync::SaveLoaded(std::uint64_t now_us) {
    load_at_ = now_us;
    rejoin_ = true;  // a guest takes the host's next word, once (the host: dropped in Tick)
}

void Sync::Loading(std::uint64_t now_us) {
    load_at_ = now_us;
}

void Sync::Reset() {
    last_.reset();
    last_at_ = 0;
    sent_at_ = 0;
    sent_once_ = false;
    set_at_ = 0;
    set_once_ = false;
    running_since_.reset();
    host_last_.reset();
    host_running_ = false;
    joined_ = false;
    nudge_.reset();
    nudge_at_ = 0;
    nudge_tries_ = 0;
    nudges_off_ = false;
    load_at_.reset();
    rejoin_ = false;
    std::lock_guard lock(inbox_mutex_);
    inbox_.clear();
}

std::optional<std::uint32_t> Sync::Tick(std::optional<std::uint32_t> now, bool host,
                                        std::uint64_t now_us,
                                        std::vector<proto::Message>& send) {
    std::vector<proto::TimeOfDay> inbox;
    {
        std::lock_guard lock(inbox_mutex_);
        inbox.swap(inbox_);
    }
    if (!now) {
        last_.reset();  // a load: the next read is no jump
        running_since_.reset();
        return std::nullopt;
    }
    const bool quiet = set_once_ && now_us - set_at_ < kQuietAfterSetUs;
    // A guest just loaded a save: its clock is the save's now. Not the
    // group's: the host's clock is taken again, as on joining.
    const bool after_load =
        !host && load_at_ && now_us >= *load_at_ && now_us - *load_at_ < kAfterLoadUs;
    if (host) rejoin_ = false;

    // Our own clock jumped (not by our set, not a guest's load): everyone takes it.
    const bool jumped =
        last_ && static_cast<std::uint32_t>(std::abs(Ahead(*last_, *now))) > kJumpSeconds;
    if (jumped && !quiet && after_load) {
        rejoin_ = true;  // a guest's reload: not the group's time; take the host's again
    } else if (jumped && !quiet) {
        proto::TimeOfDay t;
        t.seconds = *now;
        t.flags = proto::time_of_day::jump;
        send.emplace_back(t);
        sent_at_ = now_us;
        sent_once_ = true;
    }
    // Our clock runs: it moved forward since the last tick. A time change
    // (ours or the game's) and indoor scenes stop it.
    if (last_ && Ahead(*last_, *now) > 0) {
        if (!running_since_) running_since_ = now_us;
    } else {
        running_since_.reset();
    }
    const bool running = running_since_ && now_us - *running_since_ >= kRunningForUs;
    last_ = now;
    last_at_ = now_us;

    // What the others say: a jump at once; the host's clock when we drift.
    std::optional<std::uint32_t> target;
    std::optional<std::uint32_t> host_word;  // the host's latest plain word this tick
    for (const auto& t : inbox) {
        const auto off = static_cast<std::uint32_t>(std::abs(Ahead(*now, t.seconds)));
        if ((t.flags & proto::time_of_day::jump) != 0) {
            if (off > kDriftSeconds) {
                target = t.seconds;
            }
            continue;
        }
        if (host) continue;
        // The host's clock runs: its plain messages move forward. One that
        // stands (indoors, a dungeon) is no reason to move ours.
        host_running_ = host_last_ && Ahead(*host_last_, t.seconds) > 0;
        host_last_ = t.seconds;
        host_word = t.seconds;
        if (!joined_ && !set_once_) {
            // Joining: take the host's clock once (only its first word).
            joined_ = true;
            if (off > kDriftSeconds) target = t.seconds;
        } else if (rejoin_ && now_us >= *load_at_ && now_us - *load_at_ >= kRejoinAfterUs) {
            // After our save load, the same: the host's next word, once.
            rejoin_ = false;
            if (off > kDriftSeconds) target = t.seconds;
        } else if (off > kResyncSeconds && running && host_running_ &&
                   now_us - set_at_ >= kSetEveryUs) {
            // Playing: only a large drift, never while our clock stands
            // (the last set's time change still runs, or we are indoors).
            target = t.seconds;
        }
    }

    // Issue 192: a smaller drift is nudged, not set (no time change).
    if (!host && !target && host_word && joined_) {
        const std::int32_t ahead = Ahead(*now, *host_word);
        const auto off = static_cast<std::uint32_t>(std::abs(ahead));
        if (off <= kNudgeSeconds) {
            nudge_tries_ = 0;
        } else if (!nudges_off_ && !in_event_ && off <= kResyncSeconds && running &&
                   host_running_ && now_us - nudge_at_ >= kNudgeEveryUs) {
            if (nudge_tries_ >= kNudgeTries) {
                nudges_off_ = true;  // the game does not keep our write: stop
            } else {
                const auto step = static_cast<std::int32_t>(kNudgeStepSeconds);
                const std::int32_t move = ahead > step ? step : (ahead < -step ? -step : ahead);
                const auto day = static_cast<std::int32_t>(proto::kSecondsPerDay);
                const std::int32_t to = static_cast<std::int32_t>(*now) + move;
                // A nudge across midnight would leave the day counter behind
                // (a day skipped or repeated): wait until both clocks are on
                // the same side of it (at most an in-game hour).
                if (to >= 0 && to < day) {
                    nudge_ = static_cast<std::uint32_t>(to);
                    nudge_at_ = now_us;
                    ++nudge_tries_;
                    last_ = nudge_;  // our own move is no jump and does not stop "running"
                }
            }
        }
    }

    // The host says its clock once a second.
    if (host && (!sent_once_ || now_us - sent_at_ >= kSendEveryUs)) {
        proto::TimeOfDay t;
        t.seconds = *now;
        send.emplace_back(t);
        sent_at_ = now_us;
        sent_once_ = true;
    }

    if (target) {
        set_at_ = now_us;
        set_once_ = true;
        last_ = target;
    }
    return target;
}

std::optional<std::uint32_t> Sync::TakeNudge() {
    auto n = nudge_;
    nudge_.reset();
    return n;
}

void Sync::NudgeNotWritten() {
    if (nudge_tries_ > 0) --nudge_tries_;
    nudge_at_ = 0;
    last_.reset();  // re-read our clock as it is
    running_since_.reset();
}

} // namespace CoopTime
