// SPDX-License-Identifier: GPL-3.0-or-later
#include "teleport_auto.hpp"

namespace CoopNx2navi::Teleport {

std::optional<Auto::Want> Auto::Tick(const std::optional<std::string>& scene, bool self_host,
                                     std::span<const Peer> peers, std::uint64_t now_us) {
    if (!scene) {
        // A load, the title screen or a menu: whoever was with us is forgotten
        // (the header's swap rule). A want that waits stays.
        in_game_since_us_ = 0;
        together_ = {};
        if (died_us_ != 0) {
            died_reloaded_ = true;
        }
        return std::nullopt;
    }
    if (in_game_since_us_ == 0) {
        in_game_since_us_ = now_us != 0 ? now_us : 1;
        if (first_in_us_ == 0) {
            first_in_us_ = in_game_since_us_;
        }
    }

    // The join: once, to the host, soon after we first got in.
    if (!joined_) {
        if (!options_.join_at_host || self_host || now_us - first_in_us_ > kJoinWindowUs) {
            joined_ = true;
        } else if (now_us - in_game_since_us_ >= kJoinSettleUs) {
            for (const Peer& p : peers) {
                if (p.host && p.fresh && p.scene && p.number < together_.size()) {
                    joined_ = true;
                    pending_ = Pending{p.number, p.name, Why::join, now_us + kKeepUs, now_us, true};
                    break;
                }
            }
        }
    }

    // Back from a Game Over: to the leader, wherever it is.
    if (died_us_ != 0) {
        if (!died_reloaded_) {
            if (now_us - died_us_ > kDiedForgetUs) {
                died_us_ = 0;
            }
        } else if (now_us - in_game_since_us_ >= kJoinSettleUs) {
            const Peer* leader = nullptr;
            for (const Peer& p : peers) {
                if (!p.fresh || !p.scene || p.number == 0 || p.number >= together_.size()) {
                    continue;
                }
                if (p.host) {
                    leader = &p;
                    break;
                }
                if (options_.follow == FollowMode::anyone && leader == nullptr) {
                    leader = &p;
                }
            }
            if (leader != nullptr) {
                died_us_ = 0;
                pending_ = Pending{leader->number, leader->name, Why::regroup, now_us + kKeepUs, now_us, true};
            } else if (now_us - in_game_since_us_ > kRegroupWindowUs) {
                died_us_ = 0;  // nobody to go to
            }
        }
    }

    // The follow: a leader who was in our scene and now is in another.
    for (const Peer& p : peers) {
        if (p.number == 0 || p.number >= together_.size()) {
            continue;
        }
        const bool leader = options_.follow == FollowMode::anyone ||
                            (options_.follow == FollowMode::host && p.host && !self_host);
        if (!leader) {
            together_[p.number] = false;
            continue;
        }
        if (!p.scene || !p.fresh) {
            continue;  // loading: what it was stays until its new scene shows
        }
        if (*p.scene == *scene) {
            together_[p.number] = true;
            if (pending_ && pending_->player == p.number && pending_->why == Why::follow) {
                pending_.reset();  // we are there (on foot, or the trip arrived)
            }
        } else if (together_[p.number]) {
            together_[p.number] = false;
            pending_ = Pending{p.number, p.name, Why::follow, now_us + kKeepUs, now_us, true};
        }
    }
    // A player who left the session is not waited for.
    if (pending_) {
        bool here = false;
        for (const Peer& p : peers) {
            here = here || p.number == pending_->player;
        }
        if (!here || now_us > pending_->until_us) {
            pending_.reset();
        }
    }
    if (!pending_ || now_us < pending_->next_us) {
        return std::nullopt;
    }
    Want want{pending_->player, pending_->name, pending_->why, pending_->first};
    pending_->first = false;
    pending_->next_us = now_us + kRetryUs;
    return want;
}

void Auto::Done(std::uint8_t player) {
    if (pending_ && pending_->player == player) {
        pending_.reset();
    }
}

void Auto::Died(std::uint64_t now_us) {
    if (options_.follow == FollowMode::off || died_us_ != 0) {
        return;
    }
    died_us_ = now_us != 0 ? now_us : 1;
    died_reloaded_ = false;
}

void Auto::Reset() {
    died_us_ = 0;
    died_reloaded_ = false;
    pending_.reset();
    together_ = {};
    in_game_since_us_ = 0;
    first_in_us_ = 0;
    joined_ = false;
}

}  // namespace CoopNx2navi::Teleport
