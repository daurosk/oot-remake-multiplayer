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
    e.name = seat.name.empty() ? "Player " + std::to_string(player + 1) : seat.name;
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

std::vector<SeatNow> Game::Seats() const {
    std::vector<SeatNow> out;
    for (int k = 1; k <= CoopLocalInput::kMostExtra; k++) {
        const Extra& e = extra_[Index(k)];
        if (e.wanted) out.push_back({k, ActorOf(k), e.name, e.color});
    }
    return out;
}

void Game::See(const Facts& facts) {
    facts_ = facts;
    // A pad that reconnected (or woke) into its slot is a new opening, which Hookshot gives back to player 1: the
    // seat takes it again.
    for (int k = 1; k <= CoopLocalInput::kMostExtra; k++) {
        const Extra& e = extra_[Index(k)];
        if (!e.wanted || e.device < 0 || e.device == kKeyboardSlot) continue;
        const std::optional<Pad> pad = FindPad(e.device);
        if (pad && (pad->serial != e.serial || pad->npad != NpadFor(k))) {
            Say("player " + std::to_string(k + 1) + "'s controller came back: taken again");
            TakePad(k, e.device);
        }
    }
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

std::uint64_t Game::ReadingActor(std::uint64_t node) const {
    // An AI node's Player: [[[node+8]+8]+0x1f0] (the node in the caller's x19).
    namespace a = CoopPlayerInput::addr;
    std::uint64_t owner = 0, holder = 0, actor = 0;
    if (deps_.read(node + a::kNodeOwner, &owner, 8) && owner && deps_.read(owner + a::kOwnerHolder, &holder, 8) &&
        holder && deps_.read(holder + a::kHolderActor, &actor, 8))
        return actor;
    return 0;
}

bool Game::HasController(std::uint64_t manager, std::uint32_t index) const {
    std::uint64_t controller = 0;
    return manager != 0 && deps_.read(manager + CoopLocalInput::ControllerSlot(index), &controller, 8) &&
           controller != 0;
}

void Game::SayNoController(int player) const {
    if (!no_controller_said_[Index(player)].exchange(true))
        Say("player " + std::to_string(player + 1) + ": the game has no controller " +
            std::to_string(CoopLocalInput::ControllerFor(player)) + ", so that Link gets no input");
}

int Game::Wrap(std::uint32_t id, int phase, std::uint64_t* x) const {
    thread_local CoopLocalInput::Thread thread;
    thread_local bool held_off = false;   // (wrap 18: this call was answered "input off")
    if (id == CoopPlayerInput::kWrapInputOff) {
        // Whether the Player's input is off; both callers skip their read when it is. An extra player's Player
        // whose controller the game does not have is off: it never reads player 1's pad.
        if (phase == 1) {
            if (std::exchange(held_off, false)) x[0] = 1;
            return 0;
        }
        const int player = players_.Any() ? players_.Of(ReadingActor(x[19])) : 0;
        if (player == 0 || !deps_.main_base) return 0;
        std::uint64_t manager = 0;
        deps_.read(deps_.main_base() + CoopLocalInput::addr::kControllerManager, &manager, 8);
        if (HasController(manager, CoopLocalInput::ControllerFor(player))) return 0;
        SayNoController(player);
        held_off = true;
        return 1;   // (the game's check is not run; phase 1 answers 1)
    }
    if (id == CoopPlayerInput::kWrapButtons || id == CoopPlayerInput::kWrapSticks) {
        thread.Read(phase, phase == 0 && players_.Any() ? players_.Of(ReadingActor(x[19])) : 0);
        return 0;
    }
    if (id != CoopPlayerInput::kWrapControllerGet || phase != 0 || thread.Reading() == 0) return 0;
    const std::uint64_t manager = x[0];
    const auto has = [&](std::uint32_t index) { return HasController(manager, index); };
    if (const auto index = thread.ControllerGet(static_cast<std::uint32_t>(x[1]), has)) {
        x[1] = (x[1] & ~0xffffffffull) | *index;
    } else if (static_cast<std::uint32_t>(x[1]) == 0) {
        SayNoController(thread.Reading());   // (wrap 18 keeps such a Player's reads from running)
    }
    return 0;
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

int Wrap(std::uint32_t id, int phase, std::uint64_t* x) {
    Game* game = Installed();
    return game ? game->Wrap(id, phase, x) : 0;
}

void AutoStart(int slot) { g_auto_slot = slot; }

std::vector<SeatNow> Seats() {
    Game* game = Installed();
    return game ? game->Seats() : std::vector<SeatNow>{};
}

}  // namespace CoopLocalGame
