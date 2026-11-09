// The game's camera, handed from the renderer (GPU thread) to the co-op hook
// (present thread). Built into the emulator's core library; the patched
// vk_rasterizer.cpp fills it through coop_camera_track.h. No emulator code.
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>

namespace CoopCamera {

struct Frame {
    // World to clip, row-major as the game's shader reads it (clip = m * p).
    std::array<float, 16> clip_from_world{};
    float width = 0.0f;   // the scene target the matrix was drawn into, pixels
    float height = 0.0f;
    // Clip y grows down the picture (the opposite of OpenGL), from the
    // viewport and window origin of the draw.
    bool y_down = false;
    std::uint64_t frame = 0;  // the renderer's frame counter
    std::chrono::steady_clock::time_point at{};
};

// Turned on by the hook while Ocarina is running with co-op on, so the
// renderer does nothing otherwise. Any thread.
void SetWanted(bool wanted);
bool Wanted();

// GPU thread. True when a camera from a target of this size would replace
// what `frame` already has (the largest scene target of a frame wins, as in
// the emulator's own DLSS path: reflections and half-size passes are smaller).
bool Better(std::uint64_t frame, float width, float height);
void Publish(std::uint64_t frame, const float* clip_from_world, float width, float height,
             bool y_down);

// The camera of the last finished frame, if any. Any thread.
std::optional<Frame> Latest();

} // namespace CoopCamera
