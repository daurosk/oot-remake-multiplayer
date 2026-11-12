// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop/voice/spatial.hpp"

#include <algorithm>
#include <cmath>

namespace coop::voice {

namespace {

constexpr float kPi = 3.14159265358979f;

float dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
float length(const Vec3& v) { return std::sqrt(dot(v, v)); }
bool finite(const Vec3& v) {
  return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}
std::optional<Vec3> unit(const Vec3& v) {
  const float l = length(v);
  if (!(l > 1e-6f) || !std::isfinite(l)) return std::nullopt;
  return Vec3{v[0] / l, v[1] / l, v[2] / l};
}

}  // namespace

Placement place(const Listener& listener, const Vec3& source, const SpatialConfig& config) {
  Placement p;
  if (!finite(source) || !finite(listener.position) || !(config.range_m > 0.0f)) return p;
  const Vec3 to = sub(source, listener.position);
  const float d = length(to);
  p.distance = d;
  if (!(d < config.range_m)) return p;
  // Loudness: full up close, then 1/distance, faded to 0 over the last stretch.
  const float full = std::max(config.full_m, 0.1f);
  float g = d <= full ? 1.0f : full / d;
  const float fade = std::clamp(config.fade_m, 0.0f, config.range_m);
  if (fade > 0.0f && d > config.range_m - fade) {
    const float t = (config.range_m - d) / fade;  // 1 at the fade's start, 0 at range
    g *= t * t * (3.0f - 2.0f * t);
  }
  p.gain = std::clamp(g, 0.0f, 1.0f);
  p.audible = p.gain > 0.0f;
  // Direction on the camera's horizontal plane.
  const float x = dot(to, listener.right);
  const float z = dot(to, listener.forward);
  const float flat = std::sqrt(x * x + z * z);
  float pan = 0.0f;    // -1 left, +1 right
  float front = 1.0f;  // 1 ahead, -1 behind
  if (flat > 1e-4f) {
    pan = x / flat;
    front = z / flat;
  }
  // Up close the voice is in the middle (a Link one step away is not "left").
  const float spread = std::clamp(d / 1.5f, 0.0f, 1.0f);
  pan *= spread;
  front = front * spread + (1.0f - spread);
  const float angle = (pan + 1.0f) * kPi / 4.0f;  // equal power
  const float l = std::cos(angle), r = std::sin(angle);
  const float f = std::sqrt((1.0f + front) / 2.0f), b = std::sqrt((1.0f - front) / 2.0f);
  p.front_left = p.gain * l * f;
  p.front_right = p.gain * r * f;
  p.rear_left = p.gain * l * b;
  p.rear_right = p.gain * r * b;
  // Stereo: the rear folded in, a little quieter (behind sounds further off).
  const float behind = 0.8f;
  p.left = p.front_left + p.rear_left * behind;
  p.right = p.front_right + p.rear_right * behind;
  // Duller with distance: the cutoff falls from 20 kHz at full_m to
  // min_cutoff_hz at range_m (log scale), and further behind.
  const float t = std::clamp((d - full) / std::max(config.range_m - full, 0.1f), 0.0f, 1.0f);
  float cutoff = std::exp(std::log(20000.0f) * (1.0f - t) +
                          std::log(std::max(config.min_cutoff_hz, 100.0f)) * t);
  if (front < 0.0f) cutoff *= 1.0f + 0.3f * front;  // up to 30% lower straight behind
  p.lowpass = std::clamp(1.0f - std::exp(-2.0f * kPi * cutoff / 48000.0f), 0.0f, 1.0f);
  return p;
}

std::optional<Listener> listener_from_clip(const std::array<float, 16>& m) {
  for (float v : m) {
    if (!std::isfinite(v)) return std::nullopt;
  }
  const Vec3 rx{m[0], m[1], m[2]};
  const Vec3 ry{m[4], m[5], m[6]};
  const Vec3 rw{m[12], m[13], m[14]};
  // Solve rx.c = -m[3], ry.c = -m[7], rw.c = -m[15] (Cramer's rule).
  const auto det3 = [](const Vec3& a, const Vec3& b, const Vec3& c) {
    return a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) +
           a[2] * (b[0] * c[1] - b[1] * c[0]);
  };
  const float det = det3(rx, ry, rw);
  if (!(std::abs(det) > 1e-12f)) return std::nullopt;
  const Vec3 k{-m[3], -m[7], -m[15]};
  // Columns replaced one at a time: the rows here are the equations.
  const Vec3 c0{rx[0], ry[0], rw[0]}, c1{rx[1], ry[1], rw[1]}, c2{rx[2], ry[2], rw[2]};
  const auto det_cols = [&](const Vec3& a, const Vec3& b, const Vec3& c) {
    return det3(Vec3{a[0], b[0], c[0]}, Vec3{a[1], b[1], c[1]}, Vec3{a[2], b[2], c[2]});
  };
  Listener l;
  l.position = {det_cols(k, c1, c2) / det, det_cols(c0, k, c2) / det, det_cols(c0, c1, k) / det};
  auto forward = unit(rw);
  if (!forward) return std::nullopt;
  // Right: the x row with its share of the view direction taken out (an
  // off-centre projection leans it).
  const float along = dot(rx, *forward);
  auto right = unit(Vec3{rx[0] - along * (*forward)[0], rx[1] - along * (*forward)[1],
                         rx[2] - along * (*forward)[2]});
  if (!right || !finite(l.position)) return std::nullopt;
  l.forward = *forward;
  l.right = *right;
  return l;
}

Listener listener_at(const Vec3& position, float yaw) {
  Listener l;
  l.position = position;
  l.forward = {-std::sin(yaw), 0.0f, -std::cos(yaw)};
  l.right = {std::cos(yaw), 0.0f, -std::sin(yaw)};
  return l;
}

}  // namespace coop::voice
