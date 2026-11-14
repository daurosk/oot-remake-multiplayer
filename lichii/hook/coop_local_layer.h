// SPDX-License-Identifier: GPL-3.0-or-later
//
// Split screen, step 3b research (LICHII_COOP_LOCAL_LAYER=1, docs/local-coop.md
// "The other players' views"): a second world layer, made the way the game
// makes its own at its set-up (main+0xe35a50), once, at a frame in play.
//
// - The maker main+0x1174620 (layer owner, descriptor; it runs
//   main+0x1174640 and clears the new layer's +0x92): what the creator
//   main+0x1174420 calls, with a 0x28-byte descriptor (heap, a 0x100-byte part
//   that starts with the scene name, 1, the layer name, the id). The creator
//   itself cannot be used: it finds layers by name and its table only names
//   Main_3D_0. The new layer is named "3D_0" (the tail of the game's own
//   "Main_3D_0" string, so the name lives as long as the game) with id 1.
// - Then main+0x11745c0 (layer, 0) as the creator does, and main+0x336f5a0,
//   which ties the layer to the render pipeline "GameScene" with the setup
//   "Master_Field", with the same 0x180-byte descriptor the set-up builds.
// - Each frame after: the layer's rect is the right half and its camera
//   (+0x78, a sead::LookAtCamera) looks at the first Link from in front of
//   it, so the two views differ (this run has no second player). The camera
//   gets its matrix at +0x8 (rows: right, up, back; as Hookshot's free view
//   writes it) and its eye and target as doubles at +96 and +120.
//
// It logs every step and every pointer. Research only: nothing here runs
// unless the switch is set.
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "coop_local_view.h"

namespace CoopLocalLayer {

// main+ offsets (static reading of main, build e0f3ccfa).
inline constexpr std::uint64_t kMaker = 0x1174620;
inline constexpr std::uint64_t kSetShown = 0x11745c0;
inline constexpr std::uint64_t kBind = 0x336f5a0;
inline constexpr std::uint64_t kOwnerSlot = 0x4c7e880;   // [[[main+slot]] + 0x38]: the layers' owner
inline constexpr std::uint64_t kHeapSlot = 0x4c7e740;    // [[[main+slot]] + 0x200]: the heap
inline constexpr std::uint64_t kSceneMain = 0x3dc7946;   // "Main"
inline constexpr std::uint64_t kGameScene = 0x3d00948;   // "GameScene"
inline constexpr std::uint64_t kMasterField = 0x3caa19d; // "Master_Field"
inline constexpr std::uint64_t kName = 0x3f085b5;        // "3D_0" (inside "Main_3D_0" at main+0x3f085b0)
inline constexpr std::uint8_t kId = 1;
inline constexpr std::uint64_t kCamera = 0x78;
inline constexpr std::uint64_t kCameraMatrix = 0x8;
inline constexpr std::uint64_t kCameraEye = 96;
inline constexpr std::uint64_t kCameraTarget = 120;

inline constexpr std::size_t kMakerSize = 0x28 + 0x100;
inline constexpr std::size_t kBindSize = 0x180;

template <class T> void Put(std::vector<std::uint8_t>& b, std::size_t at, T v) { std::memcpy(b.data() + at, &v, sizeof v); }

// The maker's descriptor at guest address `at` (its 0x100-byte part right after it), as main+0x11744a8.. builds it.
inline std::vector<std::uint8_t> MakerDescriptor(std::uint64_t at, std::uint64_t main, std::uint64_t heap) {
    std::vector<std::uint8_t> b(kMakerSize, 0);
    Put<std::uint64_t>(b, 0x00, heap);
    Put<std::uint64_t>(b, 0x08, at + 0x28);
    Put<std::uint32_t>(b, 0x10, 1);
    Put<std::uint64_t>(b, 0x18, main + kName);
    Put<std::uint8_t>(b, 0x20, kId);
    Put<std::uint64_t>(b, 0x28, main + kSceneMain);   // (the part: the scene name, then zeros)
    return b;
}

// The binder's descriptor at guest address `at`, as main+0xe35aa0..0xe35b14 builds it: four small strings (each a
// pointer to its own buffer, room 0x20, empty) and zeros.
inline std::vector<std::uint8_t> BindDescriptor(std::uint64_t at) {
    std::vector<std::uint8_t> b(kBindSize, 0);
    Put<std::uint64_t>(b, 0x08, 1);
    Put<std::uint8_t>(b, 0x10, 1);
    for (const std::size_t s : {0x18u, 0x48u, 0x78u, 0xa8u}) {
        Put<std::uint64_t>(b, s, at + s + 0xc);   // (its buffer)
        Put<std::uint32_t>(b, s + 8, 0x20);
    }
    return b;
}

struct Vec3 {
    double x = 0, y = 0, z = 0;
};

// A view matrix (3 rows of 4 floats: right, up, back; translation -axis.eye) for a camera at `eye` looking at
// `target`, as Hookshot's free view builds it.
inline std::array<float, 12> LookAt(const Vec3& eye, const Vec3& target) {
    const double dx = eye.x - target.x, dy = eye.y - target.y, dz = eye.z - target.z;
    const double yaw = std::atan2(dx, dz), pitch = std::atan2(dy, std::hypot(dx, dz));
    const double sy = std::sin(yaw), cy = std::cos(yaw), sp = std::sin(pitch), cp = std::cos(pitch);
    const double axes[3][3] = {{cy, 0, -sy}, {-sp * sy, cp, -sp * cy}, {cp * sy, sp, cp * cy}};
    std::array<float, 12> m{};
    for (int r = 0; r < 3; r++) {
        const double* a = axes[r];
        m[r * 4 + 0] = static_cast<float>(a[0]);
        m[r * 4 + 1] = static_cast<float>(a[1]);
        m[r * 4 + 2] = static_cast<float>(a[2]);
        m[r * 4 + 3] = static_cast<float>(-(a[0] * eye.x + a[1] * eye.y + a[2] * eye.z));
    }
    return m;
}

struct Game {
    std::function<std::uint64_t()> main_base;
    std::function<bool(std::uint64_t, void*, std::size_t)> read;
    std::function<bool(std::uint64_t, const void*, std::size_t)> write;
    // Bytes in the game's stack for the next calls of this frame hook (patch 0004's scratch, at most 256 bytes):
    // their guest address, 0 when there is none.
    std::function<std::uint64_t(const std::vector<std::uint8_t>&)> scratch;
    // A game function (guest address) with x0-x7; its x0, nothing when it could not be called.
    std::function<std::optional<std::uint64_t>(std::uint64_t, const std::array<std::uint64_t, 8>&)> call;
    std::function<void(const std::string&)> log;
};

class Second {
public:
    explicit Second(Game game) : g_(std::move(game)) {}

    // The calc thread, a frame hook that may call (ctx); `first` = the first Link's place when in play.
    void Step(const std::optional<std::array<float, 3>>& first) {
        if (failed_ || !first || !g_.scratch || !g_.call) return;
        if (layer_ == 0) {
            if (++frames_in_play_ < 300) return;   // (5 s in play first)
            Make();
            return;
        }
        Follow(*first);
    }
    std::uint64_t Layer() const { return layer_; }
    // The calls that need the frame hook's context (set once; they read the current one).
    void Set(std::function<std::uint64_t(const std::vector<std::uint8_t>&)> scratch,
             std::function<std::optional<std::uint64_t>(std::uint64_t, const std::array<std::uint64_t, 8>&)> call) {
        g_.scratch = std::move(scratch);
        g_.call = std::move(call);
    }

private:
    std::uint64_t R64(std::uint64_t a) const {
        std::uint64_t v = 0;
        return g_.read(a, &v, 8) ? v : 0;
    }
    void Say(const std::string& s) const {
        if (g_.log) g_.log("local co-op: layer research: " + s);
    }
    static std::string Hex(std::uint64_t v) {
        char b[24];
        std::snprintf(b, sizeof b, "%#llx", static_cast<unsigned long long>(v));
        return b;
    }
    // [[[slot]] + field]: a global's object's field (slot: the global's pointer, as main's code loads it).
    std::uint64_t Field(std::uint64_t slot, std::uint64_t field) const {
        const std::uint64_t global = R64(slot);
        const std::uint64_t object = global ? R64(global) : 0;
        return object ? R64(object + field) : 0;
    }
    std::uint32_t Count() const {
        const std::uint64_t r = R64(g_.main_base() + CoopLocalView::kRenderer);
        std::uint32_t n = 0;
        if (r) g_.read(r + CoopLocalView::kLayerCount, &n, 4);
        return n;
    }
    std::uint64_t LayerAt(std::uint32_t id) const {
        const std::uint64_t r = R64(g_.main_base() + CoopLocalView::kRenderer);
        const std::uint64_t array = r ? R64(r + CoopLocalView::kLayerArray) : 0;
        return array ? R64(array + 8ull * id) : 0;
    }

    void Make() {
        failed_ = true;   // (one try only, whatever happens)
        const std::uint64_t main = g_.main_base();
        const std::uint64_t owner = Field(main + kOwnerSlot, 0x38), heap = Field(main + kHeapSlot, 0x200);
        Say("owner " + Hex(owner) + ", heap " + Hex(heap) + ", layers " + std::to_string(Count()) + ", id 1 holds " +
            Hex(LayerAt(kId)));
        if (owner == 0 || heap == 0 || LayerAt(kId) != 0) return Say("not made: no owner/heap, or id 1 is taken");
        const std::uint64_t at = g_.scratch(std::vector<std::uint8_t>(kMakerSize > 256 ? 256 : kMakerSize, 0));
        if (at == 0) return Say("not made: no scratch");
        // (the part runs past scratch's 256 bytes: the rest of the free stack above it, patch 0004's note)
        const std::vector<std::uint8_t> desc = MakerDescriptor(at, main, heap);
        if (!g_.write(at, desc.data(), desc.size())) return Say("not made: the descriptor was not written");
        const std::optional<std::uint64_t> layer = g_.call(main + kMaker, {owner, at, 0, 0, 0, 0, 0, 0});
        Say("maker: " + (layer ? Hex(*layer) : std::string("not called")) + ", layers " + std::to_string(Count()) +
            ", id 1 holds " + Hex(LayerAt(kId)));
        if (!layer || *layer == 0) return;
        g_.call(main + kSetShown, {*layer, 0, 0, 0, 0, 0, 0, 0});
        const std::uint64_t bind_at = g_.scratch(std::vector<std::uint8_t>(256, 0));
        const std::vector<std::uint8_t> bind = BindDescriptor(bind_at);
        if (bind_at == 0 || !g_.write(bind_at, bind.data(), bind.size())) return Say("not bound: no room");
        const std::uint64_t ptr_at = bind_at + kBindSize;   // (x3: a word holding "Main")
        const std::uint64_t main_name = main + kSceneMain;
        if (!g_.write(ptr_at, &main_name, 8)) return Say("not bound: no room for x3");
        const std::optional<std::uint64_t> bound =
            g_.call(main + kBind, {bind_at, main_name, main + kGameScene, ptr_at, main + kMasterField, 0, *layer, 0});
        layer_ = *layer;
        failed_ = false;
        const std::uint64_t camera = R64(layer_ + kCamera);
        Say(std::string("bound: ") + (bound ? "called" : "not called") + "; the layer's camera " + Hex(camera) +
            " (vtable " + Hex(camera ? R64(camera) - main : 0) + "), projection " +
            Hex(R64(layer_ + CoopLocalView::kLayerProjection)));
    }

    void Follow(const std::array<float, 3>& p) {
        CoopLocalView::Rect whole{};
        const std::uint64_t first = LayerAt(0);
        if (!first || !g_.read(first + 0x58, &whole, sizeof whole) || !CoopLocalView::Usable(whole)) return;
        const float half = whole.min_x + whole.W() / 2.0f;
        const CoopLocalView::Rect right{half, whole.min_y, whole.max_x, whole.max_y};
        g_.write(layer_ + CoopLocalView::kLayerRect, &right, sizeof right);
        const std::uint64_t projection = R64(layer_ + CoopLocalView::kLayerProjection);
        if (projection) {
            const float ratio = right.W() / right.H();
            const std::uint8_t again = 1;
            g_.write(projection + CoopLocalView::kProjectionRatio, &ratio, 4);
            g_.write(projection + CoopLocalView::kProjectionDirty, &again, 1);
        }
        const std::uint64_t camera = R64(layer_ + kCamera);
        if (!camera) return;
        const Vec3 target{p[0], p[1] + 1.0, p[2]};
        const Vec3 eye{p[0], p[1] + 3.0, p[2] + 6.0};   // (in front of Link, a little above)
        const std::array<float, 12> m = LookAt(eye, target);
        g_.write(camera + kCameraMatrix, m.data(), sizeof m);
        g_.write(camera + kCameraEye, &eye, sizeof eye);
        g_.write(camera + kCameraTarget, &target, sizeof target);
        if (++follows_ == 1 || follows_ % 1200 == 0)
            Say("frame " + std::to_string(follows_) + ": layers " + std::to_string(Count()) + ", rect right half");
    }

    Game g_;
    std::uint64_t layer_ = 0;
    bool failed_ = false;
    int frames_in_play_ = 0;
    std::uint64_t follows_ = 0;
};

}  // namespace CoopLocalLayer
