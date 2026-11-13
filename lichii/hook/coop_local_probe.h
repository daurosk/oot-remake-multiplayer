// SPDX-License-Identifier: GPL-3.0-or-later
// Local co-op's research probe (docs/local-coop.md, "The research run"):
// LICHII_COOP_LOCAL_PROBE=1 logs what split screen's views and input stand on,
// read from the running game, so step 3 is code and not guesses. Reads only.
//
// What the game's code says (build e0f3ccfa, main+ offsets):
// - The renderer's layers: the pointer at main+0x4e81cf0 is an object with a
//   count at +0x20 and an array at +0x28 indexed by layer id (the layer
//   creator main+0x1174640 checks that a new id is free there). Hookshot's
//   host/oot_fov.c reads the same object as "CameraMgr": element 0 is the
//   gameplay one, and its +120 (0x78) is the gameplay sead::LookAtCamera. So a
//   layer likely holds its own camera at +0x78.
// - The controller manager: the pointer at main+0x4e92f10; getController
//   (main+0x3b25e00) picks its controllers at +0xb0, +0x110..+0x120, +0xb8.
#pragma once

#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace CoopLocalProbe {

namespace addr {
inline constexpr std::uint64_t kLayers = 0x4e81cf0;        // main+: pointer to the layers' owner
inline constexpr std::uint64_t kLayerCount = 0x20;
inline constexpr std::uint64_t kLayerArray = 0x28;
inline constexpr std::uint64_t kLayerCamera = 0x78;
inline constexpr std::uint64_t kLayerFlags = 0x88;
inline constexpr std::uint64_t kLayerName = 0x8b0;
inline constexpr std::uint64_t kLayerSize = 0x8f0;         // what the creator allocates
inline constexpr std::uint64_t kLookAtCamera = 0x4bf8a88;  // main+: sead::LookAtCamera's vtable (0x84c18a88)
inline constexpr std::uint64_t kControllers = 0x4e92f10;   // main+: pointer to the controller manager
}  // namespace addr

inline constexpr std::uint32_t kMostLayers = 24;   // (15 ids; room for a bigger count than seen)
inline constexpr std::uint64_t kMainSize = 0x6000000;
inline constexpr std::size_t kMostViewports = 6;   // per layer

using ReadFn = std::function<bool(std::uint64_t address, void* out, std::size_t size)>;

inline std::string Hex(std::uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "%#llx", static_cast<unsigned long long>(v));
    return b;
}

// "main+0x..." for an address in main, else the address.
inline std::string Where(std::uint64_t v, std::uint64_t main_base) {
    if (v >= main_base && v < main_base + kMainSize) return "main+" + Hex(v - main_base);
    return Hex(v);
}

inline std::uint64_t Word(const ReadFn& read, std::uint64_t address) {
    std::uint64_t v = 0;
    return address != 0 && read(address, &v, sizeof v) ? v : 0;
}

// The text at `address` (up to 40 printable characters), "" when it is not one of at least 3.
inline std::string Text(const ReadFn& read, std::uint64_t address) {
    char b[41] = {};
    if (address == 0 || !read(address, b, 40)) return "";
    std::size_t n = 0;
    while (n < 40 && b[n] != 0 && std::isprint(static_cast<unsigned char>(b[n]))) n++;
    return n >= 3 && (n == 40 || b[n] == 0) ? std::string(b, n) : "";
}

// A layer's name: inline at +0x8b0, or a string object there (sead::SafeString: vtable, then the text's pointer).
inline std::string LayerName(const ReadFn& read, std::uint64_t layer) {
    if (std::string t = Text(read, layer + addr::kLayerName); !t.empty()) return t;
    for (std::uint64_t at : {layer + addr::kLayerName, layer + addr::kLayerName + 8})
        if (std::string t = Text(read, Word(read, at)); !t.empty()) return t;
    return "?";
}

// Four floats in a row that look like a viewport: x, y >= 0, a width and height of 16 to 8192 in whole
// pixels, or the whole unit square (0, 0, 1, 1).
inline bool LooksLikeViewport(const float v[4]) {
    for (int i = 0; i < 4; i++)
        if (!std::isfinite(v[i])) return false;
    if (v[0] == 0.0f && v[1] == 0.0f && v[2] == 1.0f && v[3] == 1.0f) return true;
    const auto whole = [](float f) { return f == std::floor(f); };
    return v[0] >= 0.0f && v[1] >= 0.0f && v[0] < 8192.0f && v[1] < 8192.0f && v[2] >= 16.0f && v[3] >= 16.0f &&
           v[2] <= 8192.0f && v[3] <= 8192.0f && whole(v[0]) && whole(v[1]) && whole(v[2]) && whole(v[3]);
}

// One line per layer: its id, address, vtable, flags, name, camera (and whether it is a LookAtCamera), and
// where in it four floats look like a viewport.
inline std::vector<std::string> Layers(const ReadFn& read, std::uint64_t main_base) {
    std::vector<std::string> out;
    const std::uint64_t owner = Word(read, main_base + addr::kLayers);
    std::uint32_t count = 0;
    if (owner == 0 || !read(owner + addr::kLayerCount, &count, sizeof count)) {
        out.push_back("layers: no owner at main+" + Hex(addr::kLayers));
        return out;
    }
    const std::uint64_t array = Word(read, owner + addr::kLayerArray);
    out.push_back("layers: owner " + Where(owner, main_base) + " vtable " + Where(Word(read, owner), main_base) +
                  ", count " + std::to_string(count) + ", array " + Hex(array));
    for (std::uint32_t id = 0; id < count && id < kMostLayers && array != 0; id++) {
        const std::uint64_t layer = Word(read, array + 8u * id);
        if (layer == 0) continue;
        std::uint32_t flags = 0;
        read(layer + addr::kLayerFlags, &flags, sizeof flags);
        const std::uint64_t camera = Word(read, layer + addr::kLayerCamera);
        const std::uint64_t camera_vtable = Word(read, camera);
        std::string line = "layer " + std::to_string(id) + " " + Hex(layer) + " vtable " +
                           Where(Word(read, layer), main_base) + " flags " + Hex(flags) + " \"" +
                           LayerName(read, layer) + "\" camera(+0x78) " + Hex(camera) + " vtable " +
                           Where(camera_vtable, main_base) +
                           (camera_vtable == main_base + addr::kLookAtCamera ? " (LookAtCamera)" : "");
        std::vector<std::uint8_t> block(addr::kLayerSize);
        std::size_t found = 0;
        if (read(layer, block.data(), block.size())) {
            for (std::size_t at = 0; at + 16 <= block.size() && found < kMostViewports; at += 4) {
                float v[4];
                std::memcpy(v, block.data() + at, sizeof v);
                if (!LooksLikeViewport(v)) continue;
                char b[96];
                std::snprintf(b, sizeof b, " | +%#zx (%g %g %g %g)", at, v[0], v[1], v[2], v[3]);
                line += b;
                found++;
                at += 12;   // (the next four start after these)
            }
        }
        out.push_back(line);
    }
    return out;
}

// The controller manager's controllers as getController picks them (index: offset).
inline std::vector<std::string> Controllers(const ReadFn& read, std::uint64_t main_base) {
    std::vector<std::string> out;
    const std::uint64_t manager = Word(read, main_base + addr::kControllers);
    if (manager == 0) {
        out.push_back("controllers: no manager at main+" + Hex(addr::kControllers));
        return out;
    }
    std::uint32_t flags = 0;
    read(manager + 0x15c, &flags, sizeof flags);
    std::string line = "controllers: manager " + Hex(manager) + " vtable " + Where(Word(read, manager), main_base) +
                       " flags(+0x15c) " + Hex(flags);
    static constexpr std::array<std::pair<int, std::uint64_t>, 5> kSlots{
        {{0, 0xb0}, {1, 0x110}, {2, 0x118}, {3, 0x120}, {4, 0xb8}}};
    for (const auto& [index, offset] : kSlots) {
        const std::uint64_t c = Word(read, manager + offset);
        line += " | " + std::to_string(index) + ": " + Hex(c);
        if (c != 0) line += " vtable " + Where(Word(read, c), main_base);
    }
    out.push_back(line);
    return out;
}

}  // namespace CoopLocalProbe
