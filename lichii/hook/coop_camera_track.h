// Finds the game's camera in a draw, for co-op markers. Included only by the
// patched src/video_core/renderer_vulkan/vk_rasterizer.cpp, which calls Track
// once per draw after the pipeline is configured, next to the emulator's own
// TrackOcarinaCamera.
//
// The same buffer the emulator's DLSS path reads (TrackOcarinaCamera), but
// without needing DLSS: a draw into a scene-sized B10G11R11 colour target
// with depth, whose vertex-stage uniform buffer 3 holds the target size at
// +0x120 and (-1, 0) at +0xa8, where the emulator checks a projection's w row.
// The world-to-clip matrix is the 16 floats at +0x30, row-major
// (the emulator's own motion shader reads it as row_major).
//
// Reading the buffer is the caller's: since Fritchii v6 a draw can be recorded
// on another thread than the one that filled its buffers (the memory stage,
// FRITCHII_MEMSTAGE_THREAD, and the one-follower split, FRITCHII_ONEF), and the
// camera block then comes with the draw's packet (its tracker record), not from
// game memory as it is now. The patch hands Track a `read_slot3(out, bytes)`
// that answers as TrackOcarinaCamera does on each of those paths: true when
// vertex uniform buffer 3 is bound at least `bytes` large, not GPU-written,
// with its first `bytes` copied to `out`.
#pragma once

#include <array>

#include "coop_camera.h"

namespace CoopCamera {

// The camera block bytes Track reads (through the target size at +0x120).
inline constexpr unsigned kBlockBytes = 0x130;

template <class TextureCache, class Regs, class ReadSlot3>
void Track(TextureCache& texture_cache, const Regs& regs, u64 frame, ReadSlot3&& read_slot3) {
    if (!Wanted()) {
        return;
    }
    const auto& targets = texture_cache.GetRenderTargets();
    if (!targets.color_buffer_ids[0] || !targets.depth_buffer_id) {
        return;
    }
    const auto& color = texture_cache.GetImageView(targets.color_buffer_ids[0]);
    const auto width = static_cast<float>(color.size.width);
    const auto height = static_cast<float>(color.size.height);
    if (width < 640 || height < 360 ||
        color.format != VideoCore::Surface::PixelFormat::B10G11R11_FLOAT ||
        !Better(frame, width, height)) {
        return;
    }
    std::array<f32, kBlockBytes / sizeof(f32)> camera;
    if (!read_slot3(camera.data(), static_cast<u32>(sizeof(camera)))) {
        return;
    }
    constexpr size_t extent = 0x120 / sizeof(f32);
    constexpr size_t projection_wz = 0xa8 / sizeof(f32);
    if (camera[extent] != width || camera[extent + 1] != height ||
        camera[projection_wz] != -1.0f || camera[projection_wz + 1] != 0.0f) {
        return;
    }
    // Which way clip y runs on the picture: Maxwell maps y to
    // translate_y + scale_y * y (negated first by a NegativeY swizzle),
    // counted from the bottom when the window origin is lower-left.
    const auto& viewport = regs.viewport_transform[0];
    float down = viewport.scale_y;
    if (viewport.swizzle.y == Regs::ViewportSwizzle::NegativeY) {
        down = -down;
    }
    if (regs.window_origin.mode != Regs::WindowOrigin::Mode::UpperLeft) {
        down = -down;
    }
    Publish(frame, camera.data() + 0x30 / sizeof(f32), width, height, down > 0.0f);
}

} // namespace CoopCamera
