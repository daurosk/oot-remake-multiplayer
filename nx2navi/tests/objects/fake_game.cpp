// SPDX-License-Identifier: GPL-3.0-or-later
// The stand-in game for the shared-enemies port's tests (fake_game.hpp).
#include "fake_game.hpp"

#include <cstdarg>
#include <cstdio>
#include <stdexcept>

namespace FakeGame {
namespace {

Game* g_game = nullptr;

// Main offsets the fake knows (lichii/hook/coop_objects.cpp).
constexpr std::uint64_t kManagerPtr = 0x4e91888;
constexpr std::uint64_t kActorVtable = 0x4b25e18;
constexpr std::uint64_t kSetMatrix = 0x4c6580;
constexpr std::uint64_t kPlayAs = 0x2022a0;
constexpr std::uint64_t kSetAsFrame = 0x37b5d70;
constexpr std::uint64_t kRequestDelete = 0xa6490;
constexpr std::uint64_t kMalloc = 0x14c6c90;
constexpr std::uint64_t kFrameStep = 0x3b4e300;

// One level of a hooked call in progress: which hook runs, and whether it
// already ran the rest (call_original).
struct Level {
    Nx2HookCall* call = nullptr;
    std::uint64_t address = 0;
    std::size_t index = 0;
    bool ran = false;
};
thread_local std::vector<Level> t_levels;

}  // namespace

struct Callbacks {
    static const Nx2GameInfo* GameInfo() { return &g_game->info_; }
    static void Log(const char* format, ...) {
        char line[2048];
        va_list args;
        va_start(args, format);
        std::vsnprintf(line, sizeof(line), format, args);
        va_end(args);
        std::lock_guard lock(g_game->mutex);
        g_game->log.emplace_back(line);
    }
    static int Read(std::uint64_t address, void* out, std::size_t n) {
        return g_game->Read(address, out, n) ? 1 : 0;
    }
    static int Write(std::uint64_t address, const void* in, std::size_t n) {
        return g_game->Write(address, in, n) ? 1 : 0;
    }
    static int OnFrame(Nx2FrameFn, void*) { return 0; }
    static void Remove(int handle) {
        std::erase_if(g_game->hooks, [handle](const Hook& h) { return h.handle == handle; });
    }
    static std::uint64_t GuestCall(std::uint64_t, const std::uint64_t*, int) { return 0; }
    static int GuestCallRegs(std::uint64_t addr, Nx2GuestRegs* regs) {
        regs->x[0] = g_game->Run(*regs, addr);
        return 1;
    }
    static int HookFunction(std::uint64_t addr, Nx2HookFn fn, void* user) {
        if (fn == nullptr || addr == 0) {
            return 0;
        }
        Hook h;
        h.address = addr;
        h.fn = fn;
        h.user = user;
        h.handle = g_game->next_handle_++;
        g_game->hooks.push_back(h);
        return h.handle;
    }
    static std::uint64_t CallOriginal(Nx2HookCall* call) {
        if (t_levels.empty() || t_levels.back().call != call) {
            return 0;
        }
        Level& level = t_levels.back();
        level.ran = true;
        g_game->RunFrom(level.address, *call, level.index + 1);
        return call->x[0];
    }
    static void* MapRange(std::uint64_t addr, std::size_t n, int) { return g_game->Map(addr, n); }
    static int OnExit(Nx2ExitFn, void*) { return 0; }
};

Game::Game() {
    g_game = this;
    // Each stand-in game's heap at another place: the hook's walk keeps actor
    // names by address for the life of the process, as in one game.
    static std::uint64_t games = 0;
    next_ = 0x2000000000ull + 0x100000000ull * games++;
    info_.size = sizeof(info_);
    info_.game_version = 111;
    info_.runtime_version = "test";
    info_.game_dir = ".";
    info_.mods_dir = ".";
    info_.build_id = "E0F3CCFAF50065AE";
    info_.main_base = kMain;
    info_.main_size = 0x5000000;
    info_.profile_dir = ".";
    info_.save_dir = "";

    api_.size = sizeof(api_);
    api_.version = 2;
    api_.game_info = Callbacks::GameInfo;
    api_.log = Callbacks::Log;
    api_.read = Callbacks::Read;
    api_.write = Callbacks::Write;
    api_.on_frame = Callbacks::OnFrame;
    api_.remove = Callbacks::Remove;
    api_.guest_call = Callbacks::GuestCall;
    api_.guest_call_regs = Callbacks::GuestCallRegs;
    api_.hook_function = Callbacks::HookFunction;
    api_.call_original = Callbacks::CallOriginal;
    api_.map_range = Callbacks::MapRange;
    api_.on_exit = Callbacks::OnExit;
    api_v1_ = api_;
    api_v1_.version = 1;
    api_v1_.size = static_cast<std::uint32_t>(offsetof(Nx2PluginApi, hook_function));

    // main's data around the actor manager pointer, then the manager and its list.
    regions_[kMain + 0x4e91000].bytes.resize(0x1000);
    const std::uint64_t manager = Alloc(0x200);
    Put(kMain + kManagerPtr, manager);
    const std::uint64_t lists = Alloc(0x700);
    Put(manager + 0x118, lists);
    head_ = lists + 0x670;
    Put(head_, head_);
    Put(head_ + 8, head_);
    Put<std::uint32_t>(head_ + 0x10, 0);
}

Game::~Game() {
    if (g_game == this) {
        g_game = nullptr;
    }
}

std::uint64_t Game::Alloc(std::size_t size) {
    const std::uint64_t at = next_;
    regions_[at].bytes.assign(size, 0);
    next_ += (size + 0x10f) & ~std::uint64_t{0xf};  // a gap between regions
    return at;
}

const Game::Region* Game::Find(std::uint64_t address, std::size_t size, std::uint64_t& offset) const {
    auto it = regions_.upper_bound(address);
    if (it == regions_.begin()) {
        return nullptr;
    }
    --it;
    offset = address - it->first;
    if (offset > it->second.bytes.size() || size > it->second.bytes.size() - offset) {
        return nullptr;
    }
    return &it->second;
}

bool Game::Read(std::uint64_t address, void* out, std::size_t size) const {
    std::uint64_t offset = 0;
    const Region* r = Find(address, size, offset);
    if (r == nullptr) {
        return false;
    }
    std::memcpy(out, r->bytes.data() + offset, size);
    return true;
}

bool Game::Write(std::uint64_t address, const void* in, std::size_t size) {
    std::uint64_t offset = 0;
    auto* r = const_cast<Region*>(Find(address, size, offset));
    if (r == nullptr) {
        return false;
    }
    std::memcpy(r->bytes.data() + offset, in, size);
    return true;
}

void* Game::Map(std::uint64_t address, std::size_t size) {
    std::uint64_t offset = 0;
    auto* r = const_cast<Region*>(Find(address, size, offset));
    return r == nullptr ? nullptr : r->bytes.data() + offset;
}

std::uint64_t Game::String(const std::string& text) {
    const std::uint64_t at = Alloc(text.size() + 64);
    Write(at, text.data(), text.size());
    return at;
}

Actor Game::Add(const std::string& name, std::uint64_t hash, std::array<float, 3> at,
                std::optional<std::pair<std::int32_t, std::int32_t>> life,
                const std::string& animation) {
    Actor a;
    a.address = Alloc(0x600);
    Put(a.address, kMain + kActorVtable);
    Put<std::uint32_t>(a.address + 0x2c, 4);  // awake
    Put(a.address + 0x260, String(name));
    Put(a.address + 0x31c, at);
    const std::array<float, 9> identity{1, 0, 0, 0, 1, 0, 0, 0, 1};
    Put(a.address + 0x328, identity);
    const std::uint64_t placement = Alloc(0x100);
    Put(placement + 0x88, hash);
    Put(a.address + 0x3d8, placement);
    a.table = Alloc(0x300);
    Put(a.address + 0x270, a.table);
    if (life) {
        a.life = Alloc(0x1d00);
        a.life_value = Alloc(0x10);
        a.life_max = Alloc(0x10);
        Put(a.life + 0x1c10, a.life_value);
        Put(a.life + 0x1c18, a.life_max);
        Put(a.life_value + 8, life->first);
        Put(a.life_max + 8, life->second);
        Put<std::uint32_t>(a.life + 0x1b18, 0xffffffffu);
        Put(a.table + 0x2b8, a.life);
    }
    if (!animation.empty()) {
        const std::uint64_t as = Alloc(0x20), inner = Alloc(0x20), controller = Alloc(0x30),
                            array = Alloc(0x10);
        a.slot = Alloc(0x100);
        Put(a.table + 0x40, as);
        Put(as + 0x18, inner);
        Put(inner + 0x18, controller);
        Put<std::int32_t>(controller + 0x18, 1);
        Put(controller + 0x20, array);
        Put(array, a.slot);
        Put(a.slot + 0x10, String(animation));
        Put(a.slot + 0xd4, 1.0f);
        Put(a.slot + 0xe8, 0.0f);
    }
    a.bsa = Alloc(0x100);
    Put(a.table + 0xb0, a.bsa);
    // Into the list, last.
    const std::uint64_t node = a.address + 0x538;
    const auto prev = Get<std::uint64_t>(head_ + 8);
    Put(node, head_);
    Put(node + 8, prev);
    Put(node + 0x10, a.address);
    Put(prev, node);
    Put(head_ + 8, node);
    Put(head_ + 0x10, Get<std::uint32_t>(head_ + 0x10) + 1);
    return a;
}

void Game::Unlink(std::uint64_t actor) {
    const std::uint64_t node = actor + 0x538;
    const auto next = Get<std::uint64_t>(node);
    const auto prev = Get<std::uint64_t>(node + 8);
    if (next == 0 || prev == 0) {
        return;
    }
    Put(prev, next);
    Put(next + 8, prev);
    Put<std::uint64_t>(node, 0);
    Put<std::uint64_t>(node + 8, 0);
    Put<std::uint64_t>(node + 0x10, 0);
    Put(head_ + 0x10, Get<std::uint32_t>(head_ + 0x10) - 1);
}

std::string Game::SlotName(const Actor& a) const {
    if (a.slot == 0) {
        return {};
    }
    std::array<char, 64> text{};
    Read(Get<std::uint64_t>(a.slot + 0x10), text.data(), text.size() - 1);
    return text.data();
}

std::uint64_t Game::Run(const Nx2GuestRegs& regs, std::uint64_t function) {
    CallRecord record;
    record.function = function;
    for (std::size_t i = 0; i < 8; ++i) {
        record.x[i] = regs.x[i];
    }
    std::uint32_t s0 = static_cast<std::uint32_t>(regs.v[0][0]);
    std::memcpy(&record.s0, &s0, 4);
    if (regs.size >= offsetof(Nx2GuestRegs, nstack) + sizeof(regs.nstack) && regs.stack != nullptr) {
        record.stack.assign(regs.stack, regs.stack + regs.nstack);
    }
    {
        std::lock_guard lock(mutex);
        calls.push_back(record);
    }
    const std::uint64_t offset = function - kMain;
    if (offset == kMalloc) {
        return Alloc(static_cast<std::size_t>(regs.x[0]));
    }
    if (offset == kRequestDelete) {
        Put<std::uint32_t>(regs.x[0] + 0x2c, 7);
        Unlink(regs.x[0]);
        return 1;
    }
    if (offset == kPlayAs) {
        // The request: +0 actor, +8 name. The name goes into the actor's slot 0.
        const auto actor = Get<std::uint64_t>(regs.x[0]);
        const auto name = Get<std::uint64_t>(regs.x[0] + 8);
        const auto table = Get<std::uint64_t>(actor + 0x270);
        const auto as = Get<std::uint64_t>(table + 0x40);
        const auto controller = Get<std::uint64_t>(Get<std::uint64_t>(as + 0x18) + 0x18);
        const auto slot = Get<std::uint64_t>(Get<std::uint64_t>(controller + 0x20));
        if (slot != 0) {
            Put(slot + 0x10, name);
        }
        return 1;
    }
    if (offset == kSetAsFrame) {
        const auto slot = Get<std::uint64_t>(Get<std::uint64_t>(regs.x[0] + 0x20));
        if (slot != 0) {
            Put(slot + 0xe8, record.s0);
        }
        return 1;
    }
    if (offset == kSetMatrix) {
        return 0;  // the place was written before the call
    }
    return 0;
}

void Game::RunFrom(std::uint64_t address, Nx2HookCall& call, std::size_t index) {
    std::vector<Hook> on;
    for (const Hook& h : hooks) {
        if (h.address == address) {
            on.push_back(h);
        }
    }
    if (index >= on.size()) {
        if (original_ != nullptr && *original_) {
            (*original_)(call);
        }
        return;
    }
    t_levels.push_back({&call, address, index, false});
    on[index].fn(&call, on[index].user);
    const Level level = t_levels.back();
    t_levels.pop_back();
    if (!level.ran && call.skip == 0) {
        RunFrom(address, call, index + 1);
    }
}

void Game::CallHooked(std::uint64_t address, Nx2HookCall& call,
                      const std::function<void(Nx2HookCall&)>& original) {
    const auto* before = original_;
    original_ = &original;
    call.size = sizeof(call);
    call.addr = address;
    RunFrom(address, call, 0);
    original_ = before;
}

bool Game::Hooked(std::uint64_t address) const {
    for (const Hook& h : hooks) {
        if (h.address == address) {
            return true;
        }
    }
    return false;
}

void Game::Step() {
    Nx2HookCall call{};
    call.lr = kMain + 0x6e70fc;
    CallHooked(kMain + kFrameStep, call, [](Nx2HookCall&) {});
}

std::vector<CallRecord> Game::CallsTo(std::uint64_t offset) {
    std::lock_guard lock(mutex);
    std::vector<CallRecord> out;
    for (const CallRecord& c : calls) {
        if (c.function == kMain + offset) {
            out.push_back(c);
        }
    }
    return out;
}

}  // namespace FakeGame
