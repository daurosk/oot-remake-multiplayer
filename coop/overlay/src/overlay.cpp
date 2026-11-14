#include "coop/overlay/overlay.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include "coop/host/scene.hpp"

namespace coop::overlay {

namespace {

bool finite(float v) { return std::isfinite(v); }

bool finite(const Mat4& m) {
  return std::all_of(m.begin(), m.end(), [](float v) { return std::isfinite(v); });
}

bool finite(const Vec3& v) { return finite(v[0]) && finite(v[1]) && finite(v[2]); }

using Vec4d = std::array<double, 4>;

// m * (x, y, z, w), in double.
Vec4d transform(const Mat4& m, const Vec4d& v) {
  Vec4d out{};
  for (int row = 0; row < 4; ++row) {
    double sum = 0.0;
    for (int col = 0; col < 4; ++col) sum += double(m[col * 4 + row]) * v[col];
    out[row] = sum;
  }
  return out;
}

}  // namespace

bool valid(const Settings& s) {
  const float all[] = {s.near_distance,     s.far_distance,      s.marker_size, s.reference_distance, s.min_size,
                       s.max_size, s.tag_gap, s.edge_reach,  s.edge_margin,
                       s.fade_start,        s.fade_end,          s.edge_min_alpha};
  for (float v : all) {
    if (!finite(v)) return false;
  }
  return s.near_distance > 0.0f && s.far_distance > s.near_distance && s.marker_size > 0.0f &&
         s.reference_distance > 0.0f && s.min_size > 0.0f && s.min_size <= s.max_size &&
         s.tag_gap >= 0.0f && s.edge_reach >= 0.0f && s.edge_margin >= 0.0f && s.fade_start >= 0.0f &&
         (s.fade_end == 0.0f || s.fade_end > s.fade_start) && s.edge_min_alpha >= 0.0f &&
         s.edge_min_alpha <= 1.0f;
}

std::vector<Shown> collect(presence::Presence& presence, std::uint64_t server_now_us) {
  std::vector<Shown> out;
  for (std::uint8_t n : presence.players()) {
    auto pose = presence.shown(n, server_now_us);
    if (!pose) continue;
    const presence::Player* p = presence.player(n);
    Shown s;
    s.number = n;
    if (p != nullptr) {
      s.name = p->nickname();
      s.scene = p->scene();
    }
    s.position = pose->position;
    s.fade = pose->fade;
    s.rotation = pose->rotation;
    out.push_back(std::move(s));
  }
  return out;
}

Overlay::Overlay(Settings settings) : settings_(settings) {}

Result Overlay::build(const Camera& camera, std::string_view local_scene,
                      const std::vector<Shown>& players) const {
  Result result;
  const Settings& s = settings_;
  if (!valid(s)) {
    result.error = Error::settings;
    return result;
  }
  if (!finite(camera.view) || !finite(camera.projection) || !finite(camera.viewport_width) ||
      !finite(camera.viewport_height) || camera.viewport_width <= 0.0f ||
      camera.viewport_height <= 0.0f) {
    result.error = Error::camera;
    return result;
  }

  const Mat4 view_projection = multiply(camera.projection, camera.view);
  const double width = camera.viewport_width;
  const double height = camera.viewport_height;
  const double cx = width / 2.0;
  const double cy = height / 2.0;

  for (const Shown& p : players) {
    auto skip = [&](Why why) { result.skipped.push_back({p.number, why}); };
    if (!finite(p.position) || !finite(p.fade) || !finite(p.head)) {
      skip(Why::invalid);
      continue;
    }
    if (p.fade <= 0.0f) {
      skip(Why::faded);
      continue;
    }
    const std::optional<std::string_view> theirs =
        p.scene ? std::optional<std::string_view>(*p.scene) : std::nullopt;
    if (!coop::host::same_place(theirs, local_scene)) {  // either era
      skip(Why::other_scene);
      continue;
    }

    const Vec4d clip = transform(view_projection, {p.position[0], p.position[1], p.position[2], 1.0});
    const double w = clip[3];
    if (!std::isfinite(w) || !std::isfinite(clip[0]) || !std::isfinite(clip[1])) {
      skip(Why::invalid);
      continue;
    }
    if (w <= 0.0) {
      skip(Why::behind);
      continue;
    }
    if (w < s.near_distance) {
      skip(Why::too_near);
      continue;
    }
    if (w > s.far_distance) {
      skip(Why::too_far);
      continue;
    }
    double near_fade = 1.0;
    if (s.fade_end > 0.0f && w > s.fade_start) {
      near_fade = std::max(0.0, (s.fade_end - w) / (double(s.fade_end) - s.fade_start));
    }

    const double nx = clip[0] / w;
    const double ny = clip[1] / w;
    const bool off = std::abs(nx) > 1.0 || std::abs(ny) > 1.0;
    if (off && (!s.edge_arrows || std::max(std::abs(nx), std::abs(ny)) > 1.0 + s.edge_reach)) {
      skip(Why::off_screen);
      continue;
    }

    double x = (nx * 0.5 + 0.5) * width;
    double y = (0.5 - ny * 0.5) * height;  // y down

    DrawItem item;
    item.number = p.number;
    if (off) {
      const double dx = x - cx;
      const double dy = y - cy;
      const double hx = std::max(0.0, cx - s.edge_margin);
      const double hy = std::max(0.0, cy - s.edge_margin);
      double t = 1.0;
      if (dx != 0.0) t = std::min(t, hx / std::abs(dx));
      if (dy != 0.0) t = std::min(t, hy / std::abs(dy));
      x = cx + dx * t;
      y = cy + dy * t;
      item.edge = true;
      item.arrow_angle = float(std::atan2(dy, dx));
    }

    const double size =
        std::clamp(double(s.marker_size) * s.reference_distance / w, double(s.min_size), double(s.max_size));
    item.marker = {float(x), float(y), float(w), float(size)};
    const double fade = std::min(p.fade, 1.0f);
    if (off) {
      item.alpha = float(fade * std::max(near_fade, double(s.edge_min_alpha)));
    } else if (near_fade <= 0.0) {
      skip(Why::too_far);
      continue;
    } else {
      item.alpha = float(fade * near_fade);
    }

    item.tag.text = p.name.empty() ? "P" + std::to_string(p.number) : p.name;
    item.tag.x = float(x);
    double above = y - size / 2.0 - s.tag_gap;
    if (!off && p.head > 0.0f) {
      // Over the head: project the point `head` above the feet.
      const Vec4d top =
          transform(view_projection, {p.position[0], double(p.position[1]) + p.head, p.position[2], 1.0});
      if (std::isfinite(top[0]) && std::isfinite(top[1]) && top[3] > 0.0) {
        item.tag.x = float((top[0] / top[3] * 0.5 + 0.5) * width);
        above = (0.5 - top[1] / top[3] * 0.5) * height - s.tag_gap;
        item.tag.head = true;
      }
    }
    if (above < 0.0) {
      item.tag.below = true;
      item.tag.head = false;
      item.tag.y = float(y + size / 2.0 + s.tag_gap);
      item.tag.x = float(x);
    } else {
      item.tag.y = float(above);
    }
    result.items.push_back(std::move(item));
  }

  std::stable_sort(result.items.begin(), result.items.end(), [](const DrawItem& a, const DrawItem& b) {
    if (a.marker.depth != b.marker.depth) return a.marker.depth > b.marker.depth;
    return a.number < b.number;
  });
  return result;
}

Mat4 identity() {
  Mat4 m{};
  m[0] = m[5] = m[10] = m[15] = 1.0f;
  return m;
}

Mat4 multiply(const Mat4& a, const Mat4& b) {
  Mat4 out{};
  for (int col = 0; col < 4; ++col) {
    for (int row = 0; row < 4; ++row) {
      double sum = 0.0;
      for (int k = 0; k < 4; ++k) sum += double(a[k * 4 + row]) * double(b[col * 4 + k]);
      out[col * 4 + row] = float(sum);
    }
  }
  return out;
}

Mat4 look_at(const Vec3& eye, const Vec3& target, const Vec3& up) {
  auto sub = [](const Vec3& a, const Vec3& b) { return Vec3{a[0] - b[0], a[1] - b[1], a[2] - b[2]}; };
  auto cross = [](const Vec3& a, const Vec3& b) {
    return Vec3{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
  };
  auto dot = [](const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
  auto norm = [&](const Vec3& a) {
    const float len = std::sqrt(dot(a, a));
    return len > 0.0f ? Vec3{a[0] / len, a[1] / len, a[2] / len} : a;
  };
  const Vec3 f = norm(sub(target, eye));
  const Vec3 r = norm(cross(f, up));
  const Vec3 u = cross(r, f);
  Mat4 m = identity();
  m[0] = r[0];
  m[4] = r[1];
  m[8] = r[2];
  m[1] = u[0];
  m[5] = u[1];
  m[9] = u[2];
  m[2] = -f[0];
  m[6] = -f[1];
  m[10] = -f[2];
  m[12] = -dot(r, eye);
  m[13] = -dot(u, eye);
  m[14] = dot(f, eye);
  return m;
}

Mat4 perspective(float fov_y, float aspect, float z_near, float z_far) {
  const float f = 1.0f / std::tan(fov_y / 2.0f);
  Mat4 m{};
  m[0] = f / aspect;
  m[5] = f;
  m[10] = (z_far + z_near) / (z_near - z_far);
  m[11] = -1.0f;
  m[14] = 2.0f * z_far * z_near / (z_near - z_far);
  return m;
}

}  // namespace coop::overlay
