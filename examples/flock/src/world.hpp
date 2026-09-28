#pragma once

#include <cstdint>

// The persistent contract between the page and every behavior generation.
// All state lives here, in the main module; a generation only receives a
// pointer to it. The build hashes this file into the group's ABI identity,
// so editing the layout while the page runs produces a new generation that
// the page rejects as incompatible instead of misreading memory.

inline constexpr float world_width = 960.0f;
inline constexpr float world_height = 600.0f;
inline constexpr std::uint32_t max_boids = 640;
inline constexpr std::uint32_t max_walls = 512;

struct boid {
  float x;
  float y;
  float vx;
  float vy;
  float flash; // owned by the page: fades after a wall breach
};

struct wall {
  float x0;
  float y0;
  float x1;
  float y1;
};

struct world_state {
  std::uint32_t tick;     // frames simulated since the page opened
  std::uint32_t breaches; // boids the page saw pass through a wall
  std::uint32_t boid_count;
  std::uint32_t wall_count;
  float pointer_x;
  float pointer_y;
  std::uint32_t pointer_inside;
  std::uint32_t rng;
  boid boids[max_boids];
  wall walls[max_walls];
};
