// Reloadable translation unit for the playground.
//
// Edit the physics below while the animation runs, then execute ./reload.sh
// in another terminal. The animation changes without restarting the process.
//
// Current limits:
//   * Edit function bodies, constants and strings, or add static helpers.
//   * Keep the g_ globals unchanged: they hold state across reloads.
//   * Do not reference symbols outside this file, except supported libc/libm APIs.

#include <cmath>
#include <cstdio>

// ---- World state (preserved across reloads; keep these globals unchanged) ----
double g_x = 1.0;  // Ball position along the horizontal axis.
double g_y = 18.0; // Ball height; positive points upward.
double g_vx = 5.0; // Horizontal velocity.
double g_vy = 0.0; // Vertical velocity.
int g_frame = 0;   // Frame counter for observing state continuity.

// ---- Physics parameters (edit and reload to see the effect) -----------------
const double kGravity = -9.8;     // Try -1.6 for Moon-like gravity, or -30.
const double kRestitution = 0.85; // A value of 1.01 makes each bounce higher.
const double kDt = 0.05;          // Simulation time step.
// Use an inline array, not const char*: a preserved pointer would still refer
// to the old string, while array bytes are replaced with the new .rodata.
const char kPhysName[] = "v1 Earth gravity, bounce 0.85"; // Edit the HUD label.

// Uncomment this definition to enable air drag in step_world.
// #define AIR_DRAG 1

void step_world() {
#ifdef AIR_DRAG
  g_vx *= 0.995; // Air drag reduces velocity.
  g_vy *= 0.995;
#endif
  g_vy += kGravity * kDt;
  g_x += g_vx * kDt;
  g_y += g_vy * kDt;

  if (g_y < 0.0) { // Bounce off the ground.
    g_y = 0.0;
    g_vy = -g_vy * kRestitution;
  }
  if (g_x > 76.0)
    g_vx = -fabs(g_vx); // Right wall.
  if (g_x < 1.0)
    g_vx = fabs(g_vx); // Left wall.
  ++g_frame;
}

void render_world() {
  // Map world coordinates to terminal cells: rows 2..21 for the world, 23 for the HUD.
  int row = 21 - static_cast<int>(g_y);
  if (row < 2)
    row = 2;
  if (row > 21)
    row = 21;
  int col = static_cast<int>(g_x);
  if (col < 1)
    col = 1;
  if (col > 76)
    col = 76;

  std::printf("\033[%d;%dHo", row, col + 1);
  std::printf("\033[23;1H[%s] frame=%d  pos=(%.1f, %.1f)  vel=(%.1f, %.1f)\033[K", kPhysName,
              g_frame, g_x, g_y, g_vx, g_vy);
  std::fflush(stdout);
}
