// The co-op HUD's data: PLAYER_LIFE both ways, the game look's folder and
// place names. See coop_hud.h and docs/overlay.md, "The look".

#include "coop_hud.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <system_error>
#include <variant>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "coop/host/session.hpp"
#include "coop/net/client.hpp"
#include "coop/protocol/messages.hpp"
#include "coop_every.h"
#include "coop_hud_places.h"
#include "coop_objects.h"

namespace CoopHud {
namespace {

constexpr std::uint64_t kMinGapUs = 100'000;    // at most ten PLAYER_LIFE a second
constexpr std::uint64_t kRepeatUs = 5'000'000;  // and once every 5 s anyway

std::mutex g_mutex;
std::array<std::optional<Life>, 5> g_lives;  // by player number, 1 to 4
std::atomic<bool> g_resend{false};

// Present thread only.
std::optional<Life> g_sent;
std::uint64_t g_sent_at = 0;

void ForgetAll() {
    std::lock_guard lock(g_mutex);
    g_lives.fill(std::nullopt);
}

std::string Env(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

bool HasLook(const std::filesystem::path& dir) {
    std::error_code ec;
    return std::filesystem::is_regular_file(dir / "hud.json", ec);
}

std::optional<std::filesystem::path> FindFolder(std::string& why) {
    if (Env("LICHII_COOP_HUD") == "0") {
        why = "LICHII_COOP_HUD=0";
        return std::nullopt;
    }
    if (const std::string dir = Env("LICHII_COOP_HUD_DIR"); !dir.empty()) {
        if (HasLook(dir)) {
            return std::filesystem::path(dir);
        }
        why = "LICHII_COOP_HUD_DIR " + dir + " has no hud.json";
        return std::nullopt;
    }
    std::error_code ec;
    const auto title = Common::FS::GetEdenPath(Common::FS::EdenPath::LoadDir) / "0400486024DEC000";
    // The default mod name first, then any other mod folder.
    if (HasLook(title / "coop-puppet" / "coop_hud")) {
        return title / "coop-puppet" / "coop_hud";
    }
    for (const auto& mod : std::filesystem::directory_iterator(title, ec)) {
        if (HasLook(mod.path() / "coop_hud")) {
            return mod.path() / "coop_hud";
        }
    }
    why = "no coop_hud folder in the puppet pack (run make-puppet-pack.cmd again)";
    return std::nullopt;
}

const CoopHudPlaces::Places& Places() {
    static const CoopHudPlaces::Places places = [] {
        const auto dir = Folder();
        if (!dir) {
            return CoopHudPlaces::Places{};
        }
        std::ifstream in(*dir / "places.json", std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto parsed = CoopHudPlaces::Parse(text);
        if (!parsed) {
            LOG_WARNING(Core, "coop: hud: places.json is missing or not readable; scene names shown");
            return CoopHudPlaces::Places{};
        }
        LOG_INFO(Core, "coop: hud: {} place names", parsed->size());
        return std::move(*parsed);
    }();
    return places;
}

} // namespace

void OnEvent(const coop::net::Event& event) {
    using coop::net::EventKind;
    switch (event.kind) {
    case EventKind::message:
        if (const auto* m = std::get_if<coop::protocol::PlayerLife>(&event.message)) {
            if (m->player_number >= 1 && m->player_number <= 4) {
                std::lock_guard lock(g_mutex);
                g_lives[m->player_number] = Life{m->life, m->max_life};
            }
        }
        break;
    case EventKind::player_joined:
        g_resend = true;  // the newcomer has not seen our hearts
        break;
    case EventKind::player_left:
        if (event.player >= 1 && event.player <= 4) {
            std::lock_guard lock(g_mutex);
            g_lives[event.player].reset();
        }
        break;
    case EventKind::welcomed:
    case EventKind::reconnected:
        ForgetAll();
        g_resend = true;
        break;
    case EventKind::disconnected:
        ForgetAll();
        break;
    default:
        break;
    }
}

void Frame(coop::host::Session* session) {
    if (session == nullptr) {
        g_sent.reset();
        return;
    }
    const auto own = CoopObjects::OurLife();
    if (!own) {
        return;
    }
    const auto clamp = [](std::int32_t v) {
        return static_cast<std::uint16_t>(
            std::clamp<std::int32_t>(v, 0, coop::protocol::kMaxLifeQuarters));
    };
    const Life now{clamp(own->life), clamp(own->max)};
    const std::uint64_t t = CoopEvery::NowUs();
    const bool resend = g_resend.load();
    const bool changed = !g_sent || *g_sent != now;
    const bool due = resend || t - g_sent_at >= kRepeatUs || (changed && t - g_sent_at >= kMinGapUs);
    if (!due) {
        return;
    }
    const bool sent = session->with_client([&](coop::net::Client& c) {
        return c.state() == coop::net::State::connected &&
               c.send(coop::protocol::PlayerLife{coop::protocol::kPlayerSelf, now.life, now.max});
    });
    if (sent) {
        g_sent = now;
        g_sent_at = t;
        if (resend) {
            g_resend = false;
        }
    }
}

std::optional<Life> PlayerLife(std::uint8_t player) {
    if (player < 1 || player > 4) {
        return std::nullopt;
    }
    std::lock_guard lock(g_mutex);
    return g_lives[player];
}

std::optional<std::filesystem::path> Folder() {
    static const std::optional<std::filesystem::path> folder = [] {
        std::string why;
        auto dir = FindFolder(why);
        if (dir) {
            LOG_INFO(Core, "coop: hud: the game look from {}", dir->string());
        } else {
            LOG_INFO(Core, "coop: hud: plain look ({})", why);
        }
        return dir;
    }();
    return folder;
}

std::string PlaceName(std::string_view scene) {
    return CoopHudPlaces::Name(Places(), scene);
}

} // namespace CoopHud
