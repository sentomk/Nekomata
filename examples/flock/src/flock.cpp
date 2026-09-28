#include "flock.hpp"

#include <cmath>

// The behavior generation: edit anything in this file and save while the page
// runs. Tuning constants, steering rules and helpers may all change; the world
// keeps its boids, walls, age and breach count. Only world.hpp is fixed.

namespace flock {
namespace {

constexpr float view_radius = 48.0f;
constexpr float separation_radius = 16.0f;
constexpr float cohesion_weight = 0.004f;
constexpr float alignment_weight = 0.06f;
constexpr float separation_weight = 0.10f;
constexpr float min_speed = 3.0f;
constexpr float max_speed = 9.0f;
constexpr float wall_thickness = 3.0f;

struct steering {
  float x = 0.0f;
  float y = 0.0f;
};

steering flocking(const world_state& world, const boid& self) {
  float center_x = 0.0f;
  float center_y = 0.0f;
  float heading_x = 0.0f;
  float heading_y = 0.0f;
  steering away;
  int neighbors = 0;
  for (std::uint32_t i = 0; i < world.boid_count; ++i) {
    const boid& other = world.boids[i];
    const float dx = other.x - self.x;
    const float dy = other.y - self.y;
    const float distance_squared = dx * dx + dy * dy;
    if (&other == &self || distance_squared > view_radius * view_radius) {
      continue;
    }
    center_x += other.x;
    center_y += other.y;
    heading_x += other.vx;
    heading_y += other.vy;
    if (distance_squared < separation_radius * separation_radius) {
      away.x -= dx;
      away.y -= dy;
    }
    ++neighbors;
  }
  steering result;
  if (neighbors > 0) {
    const float n = static_cast<float>(neighbors);
    result.x =
        (center_x / n - self.x) * cohesion_weight + (heading_x / n - self.vx) * alignment_weight;
    result.y =
        (center_y / n - self.y) * cohesion_weight + (heading_y / n - self.vy) * alignment_weight;
  }
  result.x += away.x * separation_weight;
  result.y += away.y * separation_weight;
  return result;
}

void limit_speed(boid& self) {
  const float speed = std::sqrt(self.vx * self.vx + self.vy * self.vy);
  if (speed > max_speed) {
    self.vx *= max_speed / speed;
    self.vy *= max_speed / speed;
  } else if (speed < min_speed && speed > 0.0f) {
    self.vx *= min_speed / speed;
    self.vy *= min_speed / speed;
  }
}

void wrap(boid& self) {
  if (self.x < 0.0f) {
    self.x += world_width;
  } else if (self.x >= world_width) {
    self.x -= world_width;
  }
  if (self.y < 0.0f) {
    self.y += world_height;
  } else if (self.y >= world_height) {
    self.y -= world_height;
  }
}

// Push a boid that ended its move inside a wall back out, and reflect its
// velocity off the wall.
void collide(boid& self, const wall& w) {
  const float wx = w.x1 - w.x0;
  const float wy = w.y1 - w.y0;
  const float length_squared = wx * wx + wy * wy;
  if (length_squared == 0.0f) {
    return;
  }
  float t = ((self.x - w.x0) * wx + (self.y - w.y0) * wy) / length_squared;
  t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
  const float nearest_x = w.x0 + wx * t;
  const float nearest_y = w.y0 + wy * t;
  float nx = self.x - nearest_x;
  float ny = self.y - nearest_y;
  const float distance = std::sqrt(nx * nx + ny * ny);
  if (distance >= wall_thickness) {
    return;
  }
  if (distance > 0.0f) {
    nx /= distance;
    ny /= distance;
  } else {
    const float length = std::sqrt(length_squared);
    nx = -wy / length;
    ny = wx / length;
  }
  self.x = nearest_x + nx * wall_thickness;
  self.y = nearest_y + ny * wall_thickness;
  const float along_normal = self.vx * nx + self.vy * ny;
  if (along_normal < 0.0f) {
    self.vx -= 2.0f * along_normal * nx;
    self.vy -= 2.0f * along_normal * ny;
  }
}

} // namespace

const char* describe() {
  return "v1 · classic flocking";
}

void step(world_state* world) {
  for (std::uint32_t i = 0; i < world->boid_count; ++i) {
    boid& self = world->boids[i];
    const steering force = flocking(*world, self);
    self.vx += force.x;
    self.vy += force.y;
    limit_speed(self);
  }
  for (std::uint32_t i = 0; i < world->boid_count; ++i) {
    boid& self = world->boids[i];
    self.x += self.vx;
    self.y += self.vy;
    for (std::uint32_t j = 0; j < world->wall_count; ++j) {
      collide(self, world->walls[j]);
    }
    wrap(self);
  }
}

} // namespace flock
