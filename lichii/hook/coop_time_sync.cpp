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

void Sync::Reset() {
    last_.reset();
    last_at_ = 0;
    sent_at_ = 0;
    sent_once_ = false;
    set_at_ = 0;
    set_once_ = false;
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
        return std::nullopt;
    }
    const bool quiet = set_once_ && now_us - set_at_ < kQuietAfterSetUs;

    // Our own clock jumped (not by our set): everyone takes it.
    if (last_ && !quiet &&
        static_cast<std::uint32_t>(std::abs(Ahead(*last_, *now))) > kJumpSeconds) {
        proto::TimeOfDay t;
        t.seconds = *now;
        t.flags = proto::time_of_day::jump;
        send.emplace_back(t);
        sent_at_ = now_us;
        sent_once_ = true;
    }
    last_ = now;
    last_at_ = now_us;

    // What the others say: a jump at once; the host's clock when we drift.
    std::optional<std::uint32_t> target;
    for (const auto& t : inbox) {
        const auto off = static_cast<std::uint32_t>(std::abs(Ahead(*now, t.seconds)));
        if ((t.flags & proto::time_of_day::jump) != 0) {
            if (off > kDriftSeconds) {
                target = t.seconds;
            }
        } else if (!host && off > kDriftSeconds &&
                   (!set_once_ || now_us - set_at_ >= kSetEveryUs)) {
            target = t.seconds;
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

} // namespace CoopTime
