// SPDX-License-Identifier: GPL-3.0-or-later
// Local co-op's game side (coop_local_game.h).
#include "coop_local_game.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <utility>

#include "coop_native_player.h"
#include "coop_player_input.h"

namespace CoopLocalGame {

bool Game::Supported() const {
    return deps_.pads && deps_.take && deps_.slots && deps_.read && deps_.wraps && deps_.wraps();
}

std::vector<CoopLocal::Device> Game::Devices() const {
    std::vector<CoopLocal::Device> out;
    out.push_back({kKeyboardSlot, "Keyboard and mouse", true});
    if (deps_.pads)
        for (const Pad& pad : deps_.pads()) out.push_back({pad.slot, pad.name.empty() ? "Controller" : pad.name, false});
    return out;
}

bool Game::InPlay() const { return facts_.game && facts_.position.has_value(); }

std::optional<Pad> Game::FindPad(int slot) const {
    if (!deps_.pads) return std::nullopt;
    for (const Pad& pad : deps_.pads())
        if (pad.slot == slot) return pad;
    return std::nullopt;
}

bool Game::TakePad(int player, int slot) {
    Extra& e = extra_[Index(player)];
    const std::optional<Pad> pad = FindPad(slot);
    if (!pad || !deps_.take(pad->slot, pad->serial, NpadFor(player))) {
        Say("player " + std::to_string(player + 1) + ": controller " + std::to_string(slot) + " could not be taken");
        return false;
    }
    e.device = slot;
    e.serial = pad->serial;
    Say("player " + std::to_string(player + 1) + " plays with \"" + pad->name + "\" (Npad No." +
        std::to_string(NpadFor(player) + 1) + ")");
    return true;
}

void Game::GivePad(int player) {
    Extra& e = extra_[Index(player)];
    if (e.device >= 0 && e.device != kKeyboardSlot) deps_.take(e.device, e.serial, 0);
    e.device = -1;
    e.serial = 0;
}

bool Game::Add(int player, const CoopLocal::Seat& seat) {
    if (player < 1 || player > CoopLocalInput::kMostExtra || !Supported()) return false;
    if (seat.device == kKeyboardSlot) {
        Say("player " + std::to_string(player + 1) + " needs a controller: the keyboard stays player 1's");
        return false;
    }
    if (!TakePad(player, seat.device)) return false;
    Extra& e = extra_[Index(player)];
    e.wanted = true;
    e.color = seat.color;
    return true;
}

std::uint64_t Game::ActorOf(int player) const {
    const std::uint8_t number = LocalNumber(player);
    for (const auto& slot : deps_.slots())
        if (slot.number == number && slot.phase == CoopPuppet::Phase::live) return slot.actor;
    return 0;
}

CoopLocal::Game::Made Game::Added(int player) {
    if (player < 1 || player > CoopLocalInput::kMostExtra || !extra_[Index(player)].wanted) return Made::No;
    const std::uint64_t actor = ActorOf(player);
    players_.Set(player, actor);
    if (actor == 0) return Made::Pending;
    Say("player " + std::to_string(player + 1) + " is the game's own Player at " + std::to_string(actor));
    return Made::Yes;
}

bool Game::Remove(int player) {
    if (player < 1 || player > CoopLocalInput::kMostExtra) return true;
    Extra& e = extra_[Index(player)];
    if (e.wanted) {
        e.wanted = false;
        GivePad(player);
        players_.Set(player, 0);
    }
    // Gone once the Director has deleted it (its target is no longer planned).
    const std::uint8_t number = LocalNumber(player);
    for (const auto& slot : deps_.slots())
        if (slot.number == number && (slot.phase == CoopPuppet::Phase::creating || slot.phase == CoopPuppet::Phase::live ||
                                      slot.phase == CoopPuppet::Phase::deleting))
            return false;
    return true;
}

void Game::Apply(const CoopLocal::Status& status) {
    for (int k = 1; k <= CoopLocalInput::kMostExtra; k++) {
        Extra& e = extra_[Index(k)];
        if (k >= status.players || !e.wanted) continue;
        const CoopLocal::Seat& seat = status.seat[static_cast<std::size_t>(k)];
        e.color = seat.color;
        if (seat.device != e.device && seat.device != kKeyboardSlot) {
            GivePad(k);
            TakePad(k, seat.device);
        }
    }
    // (step 3: the views follow status.split and status.scale)
}

void Game::See(const Facts& facts) {
    facts_ = facts;
    // A scene change deletes and makes the Players again: the wraps follow the new actor.
    for (int k = 1; k <= CoopLocalInput::kMostExtra; k++)
        players_.Set(k, extra_[Index(k)].wanted ? ActorOf(k) : 0);
}

void Game::AddTargets(CoopPuppet::Input& input) const {
    if (!facts_.position) return;
    for (int k = 1; k <= CoopLocalInput::kMostExtra; k++) {
        const Extra& e = extra_[Index(k)];
        if (!e.wanted) continue;
        CoopPuppet::Target t;
        t.number = LocalNumber(k);
        bool taken = false;
        for (const auto& other : input.targets) taken |= other.number == t.number;
        if (taken) continue;   // (a session player with that number: step 4 gives local players their own)
        t.actor = CoopNativePlayer::ActorFor(facts_.adult);
        const CoopPuppet::Vec3& beside = kBeside[Index(k)];
        // Only where it is made: after that the game's player code moves it.
        t.position = {(*facts_.position)[0] + beside[0], (*facts_.position)[1] + beside[1],
                      (*facts_.position)[2] + beside[2]};
        t.color = e.color ? e.color : coop::protocol::slot_color(t.number);
        t.why = "local player";
        input.targets.push_back(std::move(t));
    }
}

void Game::Wrap(std::uint32_t id, int phase, std::uint64_t* x) const {
    thread_local CoopLocalInput::Thread thread;
    if (id == CoopPlayerInput::kWrapButtons || id == CoopPlayerInput::kWrapSticks) {
        int player = 0;
        if (phase == 0 && players_.Any()) {
            // The reading Player: [[[node+8]+8]+0x1f0], the node in the caller's x19.
            namespace a = CoopPlayerInput::addr;
            std::uint64_t owner = 0, holder = 0, actor = 0;
            if (deps_.read(x[19] + a::kNodeOwner, &owner, 8) && owner &&
                deps_.read(owner + a::kOwnerHolder, &holder, 8) && holder &&
                deps_.read(holder + a::kHolderActor, &actor, 8))
                player = players_.Of(actor);
        }
        thread.Read(phase, player);
        return;
    }
    if (id != CoopPlayerInput::kWrapControllerGet || phase != 0 || thread.Reading() == 0) return;
    const std::uint64_t manager = x[0];
    const auto has = [&](std::uint32_t index) {
        std::uint64_t controller = 0;
        return deps_.read(manager + CoopLocalInput::ControllerSlot(index), &controller, 8) && controller != 0;
    };
    if (const auto index = thread.ControllerGet(static_cast<std::uint32_t>(x[1]), has)) {
        x[1] = (x[1] & ~0xffffffffull) | *index;
        return;
    }
    const std::size_t k = Index(thread.Reading());
    if (static_cast<std::uint32_t>(x[1]) == 0 && !no_controller_said_[k].exchange(true)) {
        Say("player " + std::to_string(k + 2) + ": the game has no controller " +
            std::to_string(CoopLocalInput::ControllerFor(thread.Reading())) + ", so it reads player 1's pad");
    }
}

namespace {
struct Installation {
    explicit Installation(Deps deps) : game(std::move(deps)), core(game) {}
    Game game;
    CoopLocal::Core core;
};
std::unique_ptr<Installation> g_installation;
std::atomic<Game*> g_game{nullptr};
int g_auto_slot = -1;   // (calc thread, after Install)
}  // namespace

void Install(Deps deps) {
    if (g_installation) return;   // once, at module start
    g_installation = std::make_unique<Installation>(std::move(deps));
    g_game.store(&g_installation->game);
    CoopLocal::Install(&g_installation->core);
}

Game* Installed() { return g_game.load(); }

void Frame(const Facts& facts) {
    if (!g_installation) return;
    g_installation->game.See(facts);
    if (g_auto_slot >= 0 && g_installation->game.InPlay()) {
        CoopLocal::Config config;
        config.seats = {{kKeyboardSlot, "", std::nullopt}, {g_auto_slot, "", std::nullopt}};
        const CoopLocal::Why why = g_installation->core.Start(config);
        g_installation->game.Note("LICHII_COOP_LOCAL: start with controller " + std::to_string(g_auto_slot) + ": " +
                                  std::string(CoopLocal::Text(why)));
        g_auto_slot = -1;
    }
    g_installation->core.Frame();
}

void AddTargets(CoopPuppet::Input& input) {
    if (Game* game = Installed()) game->AddTargets(input);
}

void Wrap(std::uint32_t id, int phase, std::uint64_t* x) {
    if (Game* game = Installed()) game->Wrap(id, phase, x);
}

void AutoStart(int slot) { g_auto_slot = slot; }

}  // namespace CoopLocalGame
