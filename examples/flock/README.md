# nekomata · flock

Edit C++ while it runs, and keep the world you built.

A flock of boids runs in the browser. You draw walls and release flocks, so
the world on screen is one only you have. Then you change the behavior code,
save, and the page switches to the new code at its next frame. The boids keep
their positions, and the walls, world age and breach counter carry on.

The page is an ordinary Nekomata consumer. It links the installed package,
declares one reload group in CMake, and calls `flock::step(&world)` like any
other function.

**Try it without installing anything:** <https://sentomk.github.io/Nekomata/>
serves a static build of this demo. Its generations were compiled ahead of
time; see [The static site](#the-static-site).

## Run it

You need Emscripten, CMake 3.24+, Ninja, Python 3 and a browser. From the
repository root:

```sh
bash examples/flock/run.sh
```

The first run builds a native publisher and the Emscripten library from this
checkout (about a minute), builds the page and publishes the first behavior
generation. It then serves <http://127.0.0.1:8940/> and watches `src/` for
changes. Set `PORT` to pick another port.

## The tour

**1. Build a world.** Drag on the canvas to draw walls and click to release
flocks. The world age counts up from page load and never resets.

**2. Watch the bug.** Some boids pass straight through walls, each one marked
with a red ring, and the breach count under the canvas climbs. The page counts breaches
itself by checking every boid's move against every wall. Behavior code doesn't
report them.

**3. Tune it.** In `src/flock.cpp`, change a constant and the label, then save:

```cpp
constexpr float view_radius = 90.0f;     // was 48
constexpr float cohesion_weight = 0.010f; // was 0.004
...
return "v2 · tighter flocks";
```

The terminal prints `published`, and the page applies the new generation
within a frame or two. Nothing on screen resets.

**4. Fix the bug.** `collide()` only notices a boid that *ends* its move
inside a wall. A fast boid moves up to 9 px per frame, but walls are only 6 px
thick, so it can jump from one side to the other. Add a swept test that also
catches the move that crosses the wall. Put it next to `collide()`:

```cpp
bool crossed(float from_x, float from_y, const boid& self, const wall& w) {
  const float rx = self.x - from_x, ry = self.y - from_y;
  const float sx = w.x1 - w.x0, sy = w.y1 - w.y0;
  const float denominator = rx * sy - ry * sx;
  if (denominator == 0.0f) {
    return false;
  }
  const float qx = w.x0 - from_x, qy = w.y0 - from_y;
  const float t = (qx * sy - qy * sx) / denominator;
  const float u = (qx * ry - qy * rx) / denominator;
  return t >= 0.0f && t <= 1.0f && u >= 0.0f && u <= 1.0f;
}

void bounce(boid& self, const wall& w) {
  const float wx = w.x1 - w.x0, wy = w.y1 - w.y0;
  const float length = std::sqrt(wx * wx + wy * wy);
  const float nx = -wy / length, ny = wx / length;
  const float along_normal = self.vx * nx + self.vy * ny;
  self.vx -= 2.0f * along_normal * nx;
  self.vy -= 2.0f * along_normal * ny;
}
```

Then use it in `step()`:

```cpp
    const float from_x = self.x;
    const float from_y = self.y;
    self.x += self.vx;
    self.y += self.vy;
    for (std::uint32_t j = 0; j < world->wall_count; ++j) {
      if (crossed(from_x, from_y, self, world->walls[j])) {
        self.x = from_x;
        self.y = from_y;
        bounce(self, world->walls[j]);
      }
      collide(self, world->walls[j]);
    }
```

Save. The breach counter stops climbing, and the breaches it already counted
remain, in the same world where the bug happened.

**5. Add a behavior.** Make the flock circle the mouse pointer. In `step()`'s
first loop, add this right after the flocking force and before `limit_speed`:

```cpp
    if (world->pointer_inside) {
      const float dx = self.x - world->pointer_x;
      const float dy = self.y - world->pointer_y;
      const float distance = std::sqrt(dx * dx + dy * dy);
      if (distance > 1.0f && distance < 180.0f) {
        self.vx += (-dy / distance) * 0.6f - (dx / distance) * 0.15f;
        self.vy += (dx / distance) * 0.6f - (dy / distance) * 0.15f;
      }
    }
```

**6. Try something that cannot work.** Add a field to `struct boid` in
`src/world.hpp` and save. The build publishes the generation, but its ABI
identity no longer matches the page's. The line under the canvas reports it
*rejected (incompatible)*, and the flock keeps flying under the previous
generation.
Revert the field and save again, and the page accepts the next generation.

Two more things to try: a compile error in `flock.cpp` never reaches the page
(the terminal shows the error, the page keeps running), and reloading the tab
starts a fresh world with whatever generation is current.

## The static site

```sh
bash examples/flock/scripts/build_site.sh
python3 -m http.server -d examples/flock/build/site   # preview
```

This builds a version of the page for GitHub Pages or any other static host
into `build/site`. A static host can't compile C++ or publish generations, so
`scripts/generations.py` builds each generation of the tour ahead of time. It
applies the same edits as above to a scratch copy of the sources and publishes
each result through `flock_reload`. The site lists the generations beside
the canvas, so each switch is visible as it happens. Choosing one hands the
running page its manifest under a new sequence number, so generations can be
chosen in any order. Nekomata still fetches the module, checks its ABI
identity and SHA-256, and switches at the next frame. The last two must be
rejected: one generation is built against a changed `boid` layout, and one
manifest lists a SHA-256 that doesn't match its module's bytes.

The live site is the repository's `gh-pages` branch, whose tree is exactly
the contents of `build/site`. To update it, rebuild the site and commit that
directory's contents to `gh-pages`.

## How it works

| File | Role |
| --- | --- |
| `src/world.hpp` | The persistent contract. All state lives here, in the page. |
| `src/flock.hpp` | The two reloadable entries, `flock::step` and `flock::describe`. |
| `src/flock.cpp` | The behavior generation. Everything in it may change. |
| `src/plt.hpp` | One `neko::wasm::plt_slot` per entry, typed from `flock.hpp`. |
| `src/plt.cpp` | The page's definitions of the entries, forwarding through the slots. |
| `src/main.cpp` | The page: world, input, breach referee, reload session, rendering bridge. |
| `web/index.html` | Canvas renderer; it reads the boid array directly from wasm memory. |
| `CMakeLists.txt` | The page, the `flock` reload group and its ABI identity. |
| `web/pages.js`, `web/pages.css` | The static site's list of generations, added by `scripts/build_site.sh`. |
| `scripts/dev.py` | Serves the page and runs `flock_reload` on every save. |
| `scripts/generations.py` | Builds the tour's generations and assembles the static site. |

Each frame begins at a safe point: `session.update()` activates a ready
generation by rewriting the PLT slots, and then the page calls `flock::step`.
The page never refers to a generation or a function pointer.

The ABI identity is a SHA-256 of `world.hpp` and `flock.hpp`, computed when
CMake configures. Editing either file reconfigures on the next build, so a
generation built against a different contract carries a different identity.
The running page still expects the old one and rejects it before downloading
it. Any edit to those two files counts, comments included. That is the
conservative side to err on.

## Limits

- Behavior code must keep all state in `world_state`. Globals and statics in
  `flock.cpp` belong to one generation and start fresh in the next.
- Every committed generation stays loaded until the page closes, so previously
  captured function pointers keep working. A long session accumulates modules.
- Only code changes are hot. Changing the contract means reloading the page
  and starting a new world.
