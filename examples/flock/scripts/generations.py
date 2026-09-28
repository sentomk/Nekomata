#!/usr/bin/env python3
"""Build every tour generation ahead of time and assemble the static site.

Each generation is the v1 sources plus the edits of every generation before
it, so the site replays the README tour. The page is built once, against the
v1 contract. Each generation is then written into a scratch copy of the
project and published through the ordinary `flock_reload` target, exactly as
saving a file does locally. The last two generations must fail: one is built
against a changed world layout, and one lists an artifact digest that does
not match its bytes.
"""

import argparse
import json
import pathlib
import re
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

TIGHTER = [
    ("constexpr float view_radius = 48.0f;", "constexpr float view_radius = 90.0f;"),
    ("constexpr float cohesion_weight = 0.004f;", "constexpr float cohesion_weight = 0.010f;"),
]

FIXED = [
    ("} // namespace\n\nconst char* describe() {", """\
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

} // namespace

const char* describe() {"""),
    ("""\
    self.x += self.vx;
    self.y += self.vy;
    for (std::uint32_t j = 0; j < world->wall_count; ++j) {
      collide(self, world->walls[j]);
    }""", """\
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
    }"""),
]

VORTEX = [
    ("""\
    self.vx += force.x;
    self.vy += force.y;
    limit_speed(self);""", """\
    self.vx += force.x;
    self.vy += force.y;
    if (world->pointer_inside) {
      const float dx = self.x - world->pointer_x;
      const float dy = self.y - world->pointer_y;
      const float distance = std::sqrt(dx * dx + dy * dy);
      if (distance > 1.0f && distance < 180.0f) {
        self.vx += (-dy / distance) * 0.6f - (dx / distance) * 0.15f;
        self.vy += (dx / distance) * 0.6f - (dy / distance) * 0.15f;
      }
    }
    limit_speed(self);"""),
]

LAYOUT = [
    ("  float flash; // owned by the page: fades after a wall breach\n};",
     "  float flash; // owned by the page: fades after a wall breach\n  float energy;\n};"),
]

# Each entry adds its edits to everything before it. `snippet` is what the
# site shows as the change; `expect` is the outcome the page must report.
GENERATIONS = [
    {
        "id": "classic",
        "label": "v1 · classic flocking",
        "title": "Classic flocking",
        "summary": "The starting behavior. Fast boids slip through walls.",
        "snippet": "// collide() only sees where a boid\n"
                   "// ends its move, so a fast boid can\n"
                   "// jump clean over a 6 px wall.",
        "expect": "applied",
    },
    {
        "id": "tighter",
        "label": "v2 · tighter flocks",
        "title": "Tune the constants",
        "summary": "Wider view, stronger cohesion: bigger, tighter flocks.",
        "flock": TIGHTER,
        "snippet": "view_radius = 90.0f;      // was 48\n"
                   "cohesion_weight = 0.010f; // was 0.004",
        "expect": "applied",
    },
    {
        "id": "fixed",
        "label": "v3 · swept wall collisions",
        "title": "Fix the wall bug",
        "summary": "Test the whole move against each wall, not just where it ends. "
                   "The breach counter stops climbing.",
        "flock": FIXED,
        "snippet": "if (crossed(from_x, from_y, self, w)) {\n"
                   "  self.x = from_x;\n"
                   "  self.y = from_y;\n"
                   "  bounce(self, w);\n"
                   "}",
        "expect": "applied",
    },
    {
        "id": "vortex",
        "label": "v4 · vortex around the pointer",
        "title": "Add a behavior",
        "summary": "Boids near the mouse pointer circle it. Hover over the canvas.",
        "flock": VORTEX,
        "snippet": "if (world->pointer_inside) {\n"
                   "  // steer along the tangent\n"
                   "  self.vx += -dy / distance * 0.6f;\n"
                   "  self.vy +=  dx / distance * 0.6f;\n"
                   "}",
        "expect": "applied",
    },
    {
        "id": "layout",
        "label": "layout change",
        "title": "Change the world layout",
        "summary": "Built against a boid with an extra field, so its ABI identity no longer "
                   "matches the page. The page refuses it before downloading.",
        "world": LAYOUT,
        "snippet": "struct boid {\n  ...\n  float flash;\n  float energy; // new field\n};",
        "expect": "incompatible",
    },
]

TAMPERED = {
    "id": "tampered",
    "label": "tampered artifact",
    "title": "Tamper with the bytes",
    "summary": "The v2 module under a manifest whose SHA-256 is off by one digit. "
               "The page refuses it before instantiating.",
    "snippet": "artifact \"modules/…wasm\" \"…\"\n"
               "// SHA-256 with one digit changed",
    "expect": "integrity",
}


def apply(text, edits, path):
    for old, new in edits:
        if text.count(old) != 1:
            sys.exit(f"{path}: edit anchor not found exactly once:\n{old}")
        text = text.replace(old, new)
    return text


def label_edit(label):
    return [('return "v1 · classic flocking";', f'return "{label}";')]


def publish(build):
    subprocess.run(["cmake", "--build", str(build), "--target", "flock_reload"], check=True,
                   stdout=subprocess.DEVNULL)
    return (build / "public" / "offers" / "latest").read_text()


def manifest_field(manifest, name):
    match = re.search(rf'^{name} "([^"]*)"(?: "([^"]*)")?$', manifest, re.MULTILINE)
    if not match:
        sys.exit(f"published manifest has no {name}:\n{manifest}")
    return match.groups()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=pathlib.Path, required=True,
                        help="scratch copy of this project that the build compiles")
    parser.add_argument("--build", type=pathlib.Path, required=True,
                        help="configured build of --project with the page already built")
    parser.add_argument("--out", type=pathlib.Path, required=True, help="site directory")
    args = parser.parse_args()

    flock_path = args.project / "src" / "flock.cpp"
    world_path = args.project / "src" / "world.hpp"
    base_flock = (ROOT / "src" / "flock.cpp").read_text()
    base_world = (ROOT / "src" / "world.hpp").read_text()

    flock_edits, world_edits, entries = [], [], []
    for generation in GENERATIONS:
        flock_edits += generation.get("flock", [])
        world_edits += generation.get("world", [])
        label = generation["label"] if generation["expect"] == "applied" else entries[-1]["label"]
        flock_path.write_text(apply(base_flock, flock_edits + label_edit(label), flock_path))
        world_path.write_text(apply(base_world, world_edits, world_path))
        manifest = publish(args.build)
        print(f"  generation {generation['id']:<10} {manifest_field(manifest, 'generation_id')[0]}")
        entries.append({**generation, "manifest": manifest})
    flock_path.write_text(base_flock)
    world_path.write_text(base_world)

    # Same bytes as v2 under a new identity, with one digest digit changed.
    tighter = next(entry for entry in entries if entry["id"] == "tighter")["manifest"]
    _, digest = manifest_field(tighter, "artifact")
    wrong = digest[:-1] + ("0" if digest[-1] != "0" else "1")
    tampered = re.sub(r'^generation_id "[^"]*"$', 'generation_id "g-tampered"', tighter,
                      flags=re.MULTILINE).replace(digest, wrong)
    entries.append({**TAMPERED, "manifest": tampered})

    public = args.build / "public"
    if args.out.exists():
        shutil.rmtree(args.out)
    (args.out / "offers" / "modules").mkdir(parents=True)
    for name in ("main.js", "main.wasm"):
        shutil.copy2(public / name, args.out / name)
    for entry in entries:
        artifact, _ = manifest_field(entry["manifest"], "artifact")
        shutil.copy2(public / "offers" / artifact, args.out / "offers" / artifact)

    page = (ROOT / "web" / "index.html").read_text()
    page = page.replace("</head>", '  <link rel="stylesheet" href="pages.css">\n</head>')
    page = page.replace('<script src="main.js"></script>', '<script src="pages.js"></script>')
    (args.out / "index.html").write_text(page)
    for name in ("pages.js", "pages.css"):
        shutil.copy2(ROOT / "web" / name, args.out / name)
    public_entries = [{key: entry[key] for key in
                       ("id", "label", "title", "summary", "snippet", "expect", "manifest")}
                      for entry in entries]
    (args.out / "generations.json").write_text(json.dumps(public_entries, indent=2) + "\n")
    (args.out / ".nojekyll").write_text("")


if __name__ == "__main__":
    main()
