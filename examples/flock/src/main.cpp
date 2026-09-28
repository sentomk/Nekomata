// The persistent page. It owns the world, handles input, watches for wall
// breaches and draws every frame. Behavior comes from the reloadable flock
// group: `flock::step(&world)` below is an ordinary call that reaches whichever
// generation the reload session activated last.

#include "flock.hpp"
#include "world.hpp"

#include <neko/session.hpp>
#include <neko/wasm.hpp>

#include <emscripten.h>
#include <emscripten/html5.h>

#include <cmath>
#include <memory>
#include <string>

namespace {

world_state world{};
std::unique_ptr<neko::reload_session> session;

// Pointer drag state: dragging draws walls, a short click releases a flock.
bool dragging = false;
float drag_anchor_x = 0.0f;
float drag_anchor_y = 0.0f;
float drag_travel = 0.0f;

constexpr double step_ms = 1000.0 / 60.0;
double previous_ms = 0.0;
double pending_ms = 0.0;

float previous_x[max_boids];
float previous_y[max_boids];

std::string last_generation = "none";

EM_JS(void, render, (const float* boids, int count, int stride, const float* walls, int wall_count),
      {
        if (window.flock_render) {
          window.flock_render(HEAPF32, boids >> 2, count, stride, walls >> 2, wall_count);
        }
      });

EM_JS(void, show_hud,
      (unsigned tick, unsigned boids, unsigned walls, unsigned breaches, unsigned applied,
       unsigned rejected, const char* behavior, const char* generation),
      {
        if (window.flock_hud) {
          window.flock_hud({
            tick : tick,
            boids : boids,
            walls : walls,
            breaches : breaches,
            applied : applied,
            rejected : rejected,
            behavior : UTF8ToString(behavior),
            generation : UTF8ToString(generation)
          });
        }
      });

EM_JS(void, log_event, (int ok, const char* text), {
  if (window.flock_log) {
    window.flock_log(!!ok, UTF8ToString(text));
  }
});

std::uint32_t next_random() {
  std::uint32_t x = world.rng;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  world.rng = x;
  return x;
}

float random_unit() {
  return static_cast<float>(next_random() >> 8) / static_cast<float>(1u << 24);
}

void spawn(float x, float y, std::uint32_t count, float spread) {
  for (std::uint32_t i = 0; i < count && world.boid_count < max_boids; ++i) {
    const float angle = random_unit() * 6.2831853f;
    const float radius = random_unit() * spread;
    const float heading = random_unit() * 6.2831853f;
    boid& b = world.boids[world.boid_count++];
    b.x = std::fmod(x + std::cos(angle) * radius + world_width, world_width);
    b.y = std::fmod(y + std::sin(angle) * radius + world_height, world_height);
    b.vx = std::cos(heading) * 4.0f;
    b.vy = std::sin(heading) * 4.0f;
    b.flash = 0.0f;
  }
}

void add_wall(float x0, float y0, float x1, float y1) {
  if (world.wall_count < max_walls) {
    world.walls[world.wall_count++] = {x0, y0, x1, y1};
  }
}

void seed_world() {
  world.rng = 0x2545f491u;
  // A pen with a doorway, and two long diagonals the flocks keep running into.
  add_wall(330.0f, 190.0f, 630.0f, 190.0f);
  add_wall(630.0f, 190.0f, 630.0f, 410.0f);
  add_wall(630.0f, 410.0f, 330.0f, 410.0f);
  add_wall(330.0f, 410.0f, 330.0f, 330.0f);
  add_wall(330.0f, 270.0f, 330.0f, 190.0f);
  add_wall(90.0f, 80.0f, 250.0f, 520.0f);
  add_wall(870.0f, 80.0f, 710.0f, 520.0f);
  spawn(480.0f, 300.0f, 60, 80.0f);
  spawn(160.0f, 300.0f, 60, 60.0f);
  spawn(800.0f, 300.0f, 60, 60.0f);
}

// True when the move from (ax, ay) to (bx, by) passes through the wall.
bool crosses(float ax, float ay, float bx, float by, const wall& w) {
  const float rx = bx - ax;
  const float ry = by - ay;
  const float sx = w.x1 - w.x0;
  const float sy = w.y1 - w.y0;
  const float denominator = rx * sy - ry * sx;
  if (denominator == 0.0f) {
    return false;
  }
  const float qx = w.x0 - ax;
  const float qy = w.y0 - ay;
  const float t = (qx * sy - qy * sx) / denominator;
  const float u = (qx * ry - qy * rx) / denominator;
  return t > 0.0f && t <= 1.0f && u >= 0.0f && u <= 1.0f;
}

// The page's own referee: independent of any generation, it counts boids whose
// movement this frame passed through a wall.
void detect_breaches() {
  for (std::uint32_t i = 0; i < world.boid_count; ++i) {
    boid& b = world.boids[i];
    b.flash = b.flash > 0.02f ? b.flash - 0.02f : 0.0f;
    const float dx = b.x - previous_x[i];
    const float dy = b.y - previous_y[i];
    if (std::fabs(dx) > world_width / 2.0f || std::fabs(dy) > world_height / 2.0f) {
      continue; // wrapped around an edge
    }
    for (std::uint32_t j = 0; j < world.wall_count; ++j) {
      if (crosses(previous_x[i], previous_y[i], b.x, b.y, world.walls[j])) {
        ++world.breaches;
        b.flash = 1.0f;
        break;
      }
    }
  }
}

void simulate() {
  for (std::uint32_t i = 0; i < world.boid_count; ++i) {
    previous_x[i] = world.boids[i].x;
    previous_y[i] = world.boids[i].y;
  }
  flock::step(&world);
  detect_breaches();
  ++world.tick;
}

const char* code_name(neko::reload_error_code code) {
  switch (code) {
  case neko::reload_error_code::none:
    return "none";
  case neko::reload_error_code::invalid_artifact:
    return "invalid_artifact";
  case neko::reload_error_code::incompatible:
    return "incompatible";
  case neko::reload_error_code::integrity:
    return "integrity";
  case neko::reload_error_code::object_rejected:
    return "object_rejected";
  case neko::reload_error_code::commit_failed:
    return "commit_failed";
  }
  return "unknown";
}

bool frame(double now_ms, void*) {
  // Safe point: no flock code is running between frames.
  for (const auto& event : session->update().events) {
    if (event.status == neko::update_status::applied) {
      last_generation = event.generation_id;
      log_event(1,
                ("applied generation " + event.generation_id + " — " + flock::describe()).c_str());
    } else {
      log_event(0, ("rejected generation " + event.generation_id + " (" + code_name(event.code) +
                    "): " + event.message + " — the previous generation keeps running")
                       .c_str());
    }
  }

  // Fixed 60 Hz simulation regardless of the display's refresh rate.
  pending_ms += previous_ms == 0.0 ? step_ms : now_ms - previous_ms;
  previous_ms = now_ms;
  for (int steps = 0; pending_ms >= step_ms && steps < 4; ++steps) {
    simulate();
    pending_ms -= step_ms;
  }
  if (pending_ms > step_ms) {
    pending_ms = 0.0;
  }

  render(&world.boids[0].x, static_cast<int>(world.boid_count),
         static_cast<int>(sizeof(boid) / sizeof(float)), &world.walls[0].x0,
         static_cast<int>(world.wall_count));
  if (world.tick % 6 == 0) {
    const auto observed = session->snapshot();
    show_hud(world.tick, world.boid_count, world.wall_count, world.breaches,
             static_cast<unsigned>(observed.applied), static_cast<unsigned>(observed.rejected),
             flock::describe(), last_generation.c_str());
  }
  return true;
}

// Mouse coordinates arrive in CSS pixels; the canvas may be scaled.
void to_world(const EmscriptenMouseEvent& event, float& x, float& y) {
  double css_width = world_width;
  double css_height = world_height;
  emscripten_get_element_css_size("#world", &css_width, &css_height);
  x = static_cast<float>(event.targetX * world_width / css_width);
  y = static_cast<float>(event.targetY * world_height / css_height);
}

bool on_mouse_down(int, const EmscriptenMouseEvent* event, void*) {
  to_world(*event, drag_anchor_x, drag_anchor_y);
  world.pointer_x = drag_anchor_x;
  world.pointer_y = drag_anchor_y;
  world.pointer_inside = 1;
  dragging = true;
  drag_travel = 0.0f;
  return true;
}

bool on_mouse_move(int, const EmscriptenMouseEvent* event, void*) {
  to_world(*event, world.pointer_x, world.pointer_y);
  world.pointer_inside = 1;
  if (dragging) {
    const float dx = world.pointer_x - drag_anchor_x;
    const float dy = world.pointer_y - drag_anchor_y;
    const float distance = std::sqrt(dx * dx + dy * dy);
    if (distance >= 18.0f) {
      add_wall(drag_anchor_x, drag_anchor_y, world.pointer_x, world.pointer_y);
      drag_anchor_x = world.pointer_x;
      drag_anchor_y = world.pointer_y;
      drag_travel += distance;
    }
  }
  return true;
}

bool on_mouse_up(int, const EmscriptenMouseEvent*, void*) {
  if (dragging && drag_travel == 0.0f) {
    spawn(world.pointer_x, world.pointer_y, 24, 24.0f);
  }
  dragging = false;
  return true;
}

bool on_mouse_leave(int, const EmscriptenMouseEvent*, void*) {
  world.pointer_inside = 0;
  return true;
}

bool on_key(int, const EmscriptenKeyboardEvent* event, void*) {
  if (std::string{event->key} == "c") {
    world.wall_count = 0;
    return true;
  }
  return false;
}

} // namespace

int main() {
  seed_world();
  session = std::make_unique<neko::reload_session>(neko::wasm::create_backend());
  session->watch();
  emscripten_set_mousedown_callback("#world", nullptr, true, on_mouse_down);
  emscripten_set_mousemove_callback("#world", nullptr, true, on_mouse_move);
  emscripten_set_mouseup_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, nullptr, true, on_mouse_up);
  emscripten_set_mouseleave_callback("#world", nullptr, true, on_mouse_leave);
  emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, true, on_key);
  emscripten_request_animation_frame_loop(frame, nullptr);
  return 0;
}
