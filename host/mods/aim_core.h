// Pure math of the route aim tool (host/mods/aim.cpp), kept apart so tests/mods/aim_test.cpp can run it without the game.
// No Windows or loader dependency.
#pragma once

#include <cmath>

namespace aim {

struct V3 { float x, y, z; };
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float len(V3 a) { return std::sqrt(dot(a, a)); }
inline V3 norm(V3 a) { const float l = len(a); return l > 1e-12f ? a * (1.0f / l) : V3{0, 0, 1}; }

// The ride camera as the game builds it: eye, look-at target and up vector, the vertical half field of view (radians) and the
// width / height aspect.
struct Cam { V3 eye{}, target{0, 0, 1}, up{0, 1, 0}; float half_y = 0.4f, aspect = 16.0f / 9.0f; };

// Where a world point lands in the picture: x right and y up in half-screens (0,0 = the reticle, +-1 = the picture edges),
// dist = straight-line distance from the eye, front = false when the point is behind the camera (x, y then only give the side).
struct Proj { float x = 0, y = 0, dist = 0; bool front = false; };

inline Proj project(const Cam& c, V3 p) {
  const V3 f = norm(c.target - c.eye);
  const V3 r = norm(cross(f, c.up));          // right-handed, the same basis as the free camera (host/mods/freecam.cpp)
  const V3 u = cross(r, f);
  const V3 d = p - c.eye;
  Proj o;
  o.dist = len(d);
  const float z = dot(d, f), sx = dot(d, r), sy = dot(d, u);
  const float ty = std::tan(c.half_y), tx = ty * c.aspect;
  o.front = z > 1e-3f;
  if (o.front) { o.x = sx / (z * tx); o.y = sy / (z * ty); }
  else { o.x = sx >= 0 ? 4.0f : -4.0f; o.y = 0; }   // behind: turn toward its side
  return o;
}

// Right stick that brings the point to the reticle: proportional to the offset, at least `lo` (below it the game's dead zone eats
// the push) and at most 1. Inside `tol` half-screens the stick rests.
struct Stick { float rx = 0, ry = 0; };

inline float axis(float off, float tol, float gain, float lo) {
  const float a = std::fabs(off);
  if (a <= tol) return 0;
  float v = a * gain;
  if (v < lo) v = lo;
  if (v > 1) v = 1;
  return off < 0 ? -v : v;
}

inline Stick steer(const Proj& p, float tol = 0.04f, float gain = 2.5f, float lo = 0.22f) {
  return {axis(p.x, tol, gain, lo), axis(p.y, tol, gain, lo)};
}

// Zoomed in, one half-screen is a smaller angle, so the same stick push moves the target farther across the picture: the stick gain
// scales with tan(half field of view) against the unzoomed ride camera (0.314 rad). Never above 1.
inline float zoom_gain(float half_y, float base = 0.314f) {
  const float k = std::tan(half_y) / std::tan(base);
  return k < 0.05f ? 0.05f : k > 1 ? 1 : k;
}

// In front and within w half-screens of the reticle on both axes.
inline bool in_window(const Proj& p, float w) { return p.front && std::fabs(p.x) <= w && std::fabs(p.y) <= w; }

// Good to shoot: in front, within `tol` half-screens of the reticle on both axes and no farther than max_dist (0 = any distance).
inline bool centered(const Proj& p, float tol, float max_dist) {
  return p.front && std::fabs(p.x) <= tol && std::fabs(p.y) <= tol && (max_dist <= 0 || p.dist <= max_dist);
}

}  // namespace aim
