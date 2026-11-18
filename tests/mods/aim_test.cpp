// Native test of the route aim tool's math (host/mods/aim_core.h). Built and run by tests/mods/run_tests.ps1.
#include <cmath>
#include <cstdio>

#include "../../host/mods/aim_core.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); g_fail++; } } while (0)
static bool near(float a, float b, float e = 1e-3f) { return std::fabs(a - b) <= e; }

int main() {
  using namespace aim;
  // Camera at the origin looking down +z, y up, 90 degree vertical field of view (half = 45 degrees), 16:9.
  Cam c;
  c.eye = {0, 0, 0}; c.target = {0, 0, 1}; c.up = {0, 1, 0};
  c.half_y = 3.14159265f / 4; c.aspect = 16.0f / 9.0f;

  Proj p = project(c, {0, 0, 10});                 // dead ahead
  CHECK(p.front && near(p.x, 0) && near(p.y, 0) && near(p.dist, 10));

  p = project(c, {0, 10, 10});                     // 45 degrees up = the top edge
  CHECK(p.front && near(p.x, 0) && near(p.y, 1));
  p = project(c, {0, -5, 10});                     // half way to the bottom edge
  CHECK(near(p.y, -0.5f));

  // Right-handed basis as in the free camera: right = forward x up. Looking down +z with y up, that is -x.
  p = project(c, {-10 * 16.0f / 9.0f, 0, 10});
  CHECK(p.front && near(p.x, 1) && near(p.y, 0));
  p = project(c, {10 * 16.0f / 9.0f, 0, 10});
  CHECK(near(p.x, -1));

  p = project(c, {-1, 0, -10});                    // behind: only the side is given, toward the shorter turn
  CHECK(!p.front && p.x > 1);
  p = project(c, {1, 0, -10});
  CHECK(!p.front && p.x < -1);

  // A camera somewhere else, looking along -x: a point straight ahead stays centered, and distance is the straight line.
  Cam d;
  d.eye = {100, 20, -50}; d.target = {90, 20, -50}; d.up = {0, 1, 0}; d.half_y = 0.4f; d.aspect = 16.0f / 9.0f;
  p = project(d, {70, 20, -50});
  CHECK(p.front && near(p.x, 0) && near(p.y, 0) && near(p.dist, 30));
  p = project(d, {70, 20 + 30 * std::tan(0.4f), -50});
  CHECK(near(p.y, 1));
  // A tilted up vector that is not perpendicular to the view still gives an upright picture.
  d.up = {-0.5f, 1, 0};
  p = project(d, {70, 25, -50});
  CHECK(near(p.x, 0) && p.y > 0);

  // Steering: toward the offset, never below the floor outside the tolerance, capped at 1, at rest inside it.
  Stick s = steer(Proj{0.5f, -0.5f, 10, true});
  CHECK(s.rx > 0 && s.ry < 0 && s.rx <= 1 && s.ry >= -1);
  s = steer(Proj{0.05f, 0, 10, true}, 0.04f, 2.5f, 0.22f);
  CHECK(near(s.rx, 0.22f) && s.ry == 0);
  s = steer(Proj{0.01f, -0.02f, 10, true});
  CHECK(s.rx == 0 && s.ry == 0);
  s = steer(Proj{4, 0, 10, false});
  CHECK(s.rx == 1);
  s = steer(Proj{-4, 0, 10, false});
  CHECK(s.rx == -1);

  // Ready to shoot: in front, inside the tolerance on both axes, inside the range (0 = any).
  CHECK(centered(Proj{0.03f, -0.03f, 12, true}, 0.06f, 0));
  CHECK(!centered(Proj{0.03f, 0.08f, 12, true}, 0.06f, 0));
  CHECK(!centered(Proj{0, 0, 12, false}, 0.06f, 0));
  CHECK(centered(Proj{0, 0, 12, true}, 0.06f, 15));
  CHECK(!centered(Proj{0, 0, 22, true}, 0.06f, 15));

  // Window around the reticle (the act: sweep target and the stand-in actor check): both axes, in front only.
  CHECK(in_window(Proj{0.1f, -0.1f, 5, true}, 0.12f));
  CHECK(!in_window(Proj{0.13f, 0, 5, true}, 0.12f));
  CHECK(!in_window(Proj{0, -0.13f, 5, true}, 0.12f));
  CHECK(!in_window(Proj{0, 0, 5, false}, 0.12f));

  // Zoom: the gain falls with the field of view and never rises above the unzoomed ride camera.
  CHECK(std::fabs(zoom_gain(0.314f) - 1.0f) < 1e-4f);
  CHECK(zoom_gain(0.6f) == 1.0f);
  CHECK(zoom_gain(0.1f) > 0.29f && zoom_gain(0.1f) < 0.33f);
  CHECK(zoom_gain(0.0f) == 0.05f);

  if (g_fail) { printf("aim_test: %d FAILED\n", g_fail); return 1; }
  printf("aim_test: all passed\n");
  return 0;
}
