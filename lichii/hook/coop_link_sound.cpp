// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_link_sound.h"

#include <array>
#include <atomic>
#include <mutex>
#include <set>
#include <utility>

#include "coop/protocol/messages.hpp"
#include "coop_puppet.h"

namespace CoopLinkSound {

static_assert(kNameMax == coop::protocol::kMaxLinkSoundChars);
static_assert(CoopPuppetSound::block::kText + CoopPuppetSound::block::kTextCapacity == block::kHandle);
static_assert(kNameMax + 1 == CoopPuppetSound::block::kTextCapacity);
static_assert(block::kHandle + 8 <= CoopPuppet::block::kSize);

namespace {

std::atomic<std::uint64_t> g_our_user{0};
std::atomic<bool> g_probe{false};

std::mutex& CaughtMutex() {
    static std::mutex m;
    return m;
}
std::vector<std::string>& Caught() {  // under CaughtMutex
    static std::vector<std::string> v;
    return v;
}
std::set<std::pair<std::uint64_t, std::string>>& ProbeSeen() {  // under CaughtMutex
    static std::set<std::pair<std::uint64_t, std::string>> s;
    return s;
}
std::vector<std::string>& ProbeLines() {  // under CaughtMutex
    static std::vector<std::string> v;
    return v;
}
inline constexpr std::size_t kProbePairsMax = 512;

std::uint64_t Pointer(CoopPuppet::Guest& guest, std::uint64_t address) {
    std::uint64_t v = 0;
    return address != 0 && guest.Read(address, &v, sizeof v) ? v : 0;
}

std::string Hex(std::uint64_t v) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    do {
        out.insert(out.begin(), digits[v & 0xf]);
        v >>= 4;
    } while (v != 0);
    return "0x" + out;
}

void Drop(std::deque<std::uint64_t>& times, std::uint64_t now_ms) {
    while (!times.empty() && now_ms - times.front() >= 1000) {
        times.pop_front();
    }
}

// The emit call for player.name with its asset.
std::optional<CoopPuppet::Call> Emit(Player& player, CoopPuppet::Guest& guest, std::uint64_t main_base,
                                     std::uint64_t user, std::uint64_t block, std::uint64_t asset,
                                     const std::string& who, const Say& say, bool debug) {
    // The handle as the game's own node starts one: u8 0xff, u16 0xffff, u32 0.
    const std::array<std::uint8_t, 8> handle{0xff, 0, 0xff, 0xff, 0, 0, 0, 0};
    if (asset == 0 || !guest.Write(block + CoopPuppetSound::block::kAsset, &asset, sizeof asset) ||
        !guest.Write(block + block::kHandle, handle.data(), handle.size())) {
        return std::nullopt;
    }
    CoopPuppet::Call c;
    c.function = main_base + addr::kEmit;
    c.x[0] = user;
    c.x[1] = block + CoopPuppetSound::block::kAsset;
    c.x[2] = block + block::kHandle;
    c.x[3] = 0;
    player.pending = Player::Step::emit;
    if (debug) {
        say(who + " sound '" + player.name + "'");
    }
    return c;
}

} // namespace

void SetOurUser(std::uint64_t user) {
    g_our_user.store(user, std::memory_order_relaxed);
}

std::uint64_t OurUser() {
    return g_our_user.load(std::memory_order_relaxed);
}

std::string NameAt(std::uint64_t asset_cell, const Read& read) {
    std::uint64_t asset = 0;
    std::uint64_t text = 0;
    if (asset_cell == 0 || !read(asset_cell, &asset, sizeof asset) || asset == 0 ||
        !read(asset, &text, sizeof text) || text == 0) {
        return {};
    }
    std::array<char, kNameMax + 1> buffer{};
    if (!read(text, buffer.data(), buffer.size())) {
        return {};
    }
    std::string name;
    for (const char ch : buffer) {
        if (ch == '\0') {
            return coop::protocol::valid_link_sound_name(name) ? name : std::string();
        }
        name += ch;
    }
    return {};  // longer than the protocol takes
}

bool OnEmit(std::uint64_t user, std::uint64_t asset_cell, const Read& read) {
    const std::uint64_t ours = OurUser();
    const bool probe = g_probe.load(std::memory_order_relaxed);
    if (user == 0 || (!probe && user != ours)) {
        return false;  // nearly every call: not our Link
    }
    std::string name = NameAt(asset_cell, read);
    std::lock_guard lock(CaughtMutex());
    if (probe) {
        auto& seen = ProbeSeen();
        if (seen.size() < kProbePairsMax && seen.emplace(user, name).second) {
            ProbeLines().push_back("coop: sound probe: user " + Hex(user) + (user == ours ? " (our Link)" : "") +
                                   " emits '" + (name.empty() ? std::string("?") : name) + "'");
        }
    }
    if (user != ours || name.empty()) {
        return false;
    }
    auto& caught = Caught();
    if (caught.size() >= kCaughtMax) {
        return false;
    }
    caught.push_back(std::move(name));
    return true;
}

std::vector<std::string> TakeCaught() {
    std::lock_guard lock(CaughtMutex());
    return std::exchange(Caught(), {});
}

void SetProbe(bool on) {
    g_probe.store(on, std::memory_order_relaxed);
}

std::vector<std::string> TakeProbeLines() {
    std::lock_guard lock(CaughtMutex());
    return std::exchange(ProbeLines(), {});
}

bool Limiter::Allow(std::string_view name, std::uint64_t now_ms) {
    Drop(sent_ms_, now_ms);
    const auto last = last_ms_.find(name);
    if (last != last_ms_.end() && now_ms - last->second < kRepeatMs) {
        return false;
    }
    if (sent_ms_.size() >= kSendPerSecond) {
        return false;
    }
    if (last_ms_.size() > 256) {
        last_ms_.clear();  // a bound; a repeat after this is only sent once more
    }
    last_ms_[std::string(name)] = now_ms;
    sent_ms_.push_back(now_ms);
    return true;
}

bool InHearing(const std::optional<std::array<float, 3>>& at, const std::array<float, 3>& from) {
    if (!at) {
        return false;
    }
    float d2 = 0.0f;
    for (std::size_t i = 0; i < 3; ++i) {
        const float d = (*at)[i] - from[i];
        d2 += d * d;
    }
    return d2 <= kHearDistance * kHearDistance;
}

void Heard::Push(std::string name, std::uint64_t now_ms) {
    if (queue_.size() >= kHeardMax) {
        queue_.pop_front();
    }
    queue_.push_back(Sound{std::move(name), now_ms});
}

void Heard::PushFront(Sound sound) {
    if (queue_.size() >= kHeardMax) {
        queue_.pop_back();
    }
    queue_.push_front(std::move(sound));
}

std::optional<Sound> Heard::Pop(std::uint64_t now_ms) {
    while (!queue_.empty()) {
        Sound s = std::move(queue_.front());
        queue_.pop_front();
        if (now_ms < s.at_ms || now_ms - s.at_ms <= kHeardStaleMs) {
            return s;
        }
    }
    return std::nullopt;
}

bool Budget::Take(std::uint64_t now_ms) {
    Drop(taken_ms_, now_ms);
    if (taken_ms_.size() >= kPlayPerSecond) {
        return false;
    }
    taken_ms_.push_back(now_ms);
    return true;
}

bool AlreadyPlaying(std::string_view playing, std::string_view name) {
    std::size_t start = 0;
    while (start <= playing.size()) {
        const std::size_t end = playing.find(", ", start);
        const std::string_view one =
            playing.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (one == name) {
            return true;
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 2;
    }
    return false;
}

std::optional<CoopPuppet::Call> Next(Player& player, Budget& budget, CoopPuppet::Guest& guest,
                                     std::uint64_t main_base, std::uint64_t actor,
                                     std::uint64_t user, std::uint64_t block, std::uint64_t now_ms,
                                     const std::function<std::string()>& playing,
                                     const std::string& who, const Say& say, bool debug) {
    if (player.pending != Player::Step::none) {
        return std::nullopt;
    }
    if (actor != player.actor || user != player.user) {
        // Made again (or its user changed): the assets found belong to the old user.
        player.actor = actor;
        player.user = user;
        player.assets.clear();
        player.missing_said = false;
        player.found = false;
    }
    if (main_base == 0 || actor == 0 || block == 0 || (player.heard.Empty() && !player.found)) {
        return std::nullopt;
    }
    if (user == 0) {
        player.heard.Clear();
        if (!player.missing_said) {
            player.missing_said = true;
            say(who + " has no sound user: the player's sounds are not played");
        }
        return std::nullopt;
    }
    if (std::exchange(player.found, false)) {
        return Emit(player, guest, main_base, user, block, player.assets[player.name], who, say, debug);
    }
    std::optional<Sound> sound;
    while ((sound = player.heard.Pop(now_ms))) {
        const auto known = player.assets.find(sound->name);
        if (known == player.assets.end() || known->second != 0) {
            break;  // not looked for yet, or found
        }
        // not in its user: dropped (said once when looked for)
    }
    if (!sound) {
        return std::nullopt;
    }
    const auto known = player.assets.find(sound->name);
    if (known != player.assets.end() && AlreadyPlaying(playing(), sound->name)) {
        return std::nullopt;  // its own animation started it: not twice
    }
    if (!budget.Take(now_ms)) {
        player.heard.PushFront(std::move(*sound));  // a later frame, its age kept
        return std::nullopt;
    }
    player.name = std::move(sound->name);
    CoopPuppet::Call c;
    if (known == player.assets.end()) {
        std::uint64_t accessor = 0;
        const std::uint64_t resource = Pointer(guest, user + addr::kUserResource);
        accessor = Pointer(guest, resource != 0 ? resource + addr::kResourceAccessor : 0);
        std::array<char, kNameMax + 1> text{};
        for (std::size_t i = 0; i < player.name.size() && i < kNameMax; ++i) {
            text[i] = player.name[i];
        }
        const std::uint64_t zero = 0;
        if (accessor == 0 || !guest.Write(block + CoopPuppetSound::block::kText, text.data(), text.size()) ||
            !guest.Write(block + CoopPuppetSound::block::kAsset, &zero, sizeof zero)) {
            return std::nullopt;
        }
        c.function = main_base + addr::kSearchAsset;
        c.x[0] = accessor;
        c.x[1] = block + CoopPuppetSound::block::kAsset;
        c.x[2] = block + CoopPuppetSound::block::kText;
        player.pending = Player::Step::search;
        return c;
    }
    return Emit(player, guest, main_base, user, block, known->second, who, say, debug);
}

void Finish(Player& player, std::uint64_t result, CoopPuppet::Guest& guest, std::uint64_t block,
            const std::string& who, const Say& say, bool debug) {
    const auto done = std::exchange(player.pending, Player::Step::none);
    if (done != Player::Step::search) {
        return;
    }
    std::uint64_t asset = 0;
    if ((result & 1) == 0 || !guest.Read(block + CoopPuppetSound::block::kAsset, &asset, sizeof asset) ||
        asset == 0) {
        player.assets[player.name] = 0;
        if (debug) {
            say(who + ": the sound '" + player.name + "' is not in its sound user");
        }
        return;
    }
    player.assets[player.name] = asset;
    player.found = true;  // it plays as the next call
}

} // namespace CoopLinkSound

#ifdef LICHII_COOP

#include "common/logging.h"
#include "coop/host/session.hpp"
#include "coop/net/client.hpp"

namespace CoopLinkSound {

void Frame(coop::host::Session* session, std::uint64_t now_ms) {
    static Limiter limiter;  // the frame's thread only
    static std::uint64_t sent = 0;
    static std::uint64_t dropped = 0;
    for (const auto& line : TakeProbeLines()) {
        LOG_INFO(Core, "{}", line);
    }
    std::vector<std::string> names = TakeCaught();
    if (names.empty() || session == nullptr) {
        return;
    }
    session->with_client([&](coop::net::Client& c) {
        for (auto& name : names) {
            if (!limiter.Allow(name, now_ms)) {
                ++dropped;
                continue;
            }
            // The first ones, then every 500th: enough to see it work, no spam.
            if (++sent <= 5 || sent % 500 == 0) {
                LOG_INFO(Core, "coop: link sound '{}' sent ({} sent, {} held back by the rate limit)", name,
                         sent, dropped);
            }
            c.send(coop::protocol::LinkSound{coop::protocol::kPlayerSelf, std::move(name)});
        }
    });
}

} // namespace CoopLinkSound

#endif // LICHII_COOP
