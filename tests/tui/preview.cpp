// tui_preview — dual-pane Nekomata TUI with sample data.
//
// Left/top pane: Nekomata monitor (reload status)
// Right/bottom pane: application log viewer
//
// Layout adapts to terminal width: side-by-side when wide (>=100 cols),
// stacked when narrow. FullScreenGuard manages terminal state.

#include <chrono>
#include <cstdio>
#include <deque>
#include <iostream>
#include <sstream>
#include <thread>

#include <sys/ioctl.h>
#include <unistd.h>

#include "glyph/core/cell.h"
#include "glyph/core/color.h"
#include "glyph/core/geometry.h"
#include "glyph/core/style.h"
#include "glyph/render/ansi/ansi_renderer.h"
#include "glyph/render/terminal.h"
#include "glyph/view/frame.h"
#include "glyph/view/text.h"

using glyph::core::Cell;
using glyph::core::Color;
using glyph::core::Point;
using glyph::core::Rect;
using glyph::core::Size;
using glyph::core::Style;
using glyph::view::Frame;

// Nekomata palette
static Style title_s() {
  return Style{}.fg(Color::rgb(255, 180, 100));
}
static Style tag_s() {
  return Style{}.fg(Color::rgb(120, 120, 120));
}
static Style sec_s() {
  return Style{}.fg(Color::rgb(90, 90, 90));
}
static Style info_s() {
  return Style{}.fg(Color::rgb(100, 200, 255));
}
static Style ok_s() {
  return Style{}.fg(Color::rgb(100, 220, 100));
}
static Style err_s() {
  return Style{}.fg(Color::rgb(255, 100, 100));
}
static Style warn_s() {
  return Style{}.fg(Color::rgb(255, 200, 100));
}
static Style dim_s() {
  return Style{}.fg(Color::rgb(140, 140, 140));
}
static Style key_s() {
  return Style{}.fg(Color::rgb(255, 255, 255));
}
static Style brd_s() {
  return Style{}.fg(Color::rgb(70, 70, 70));
}
static Style log_app_s() {
  return Style{}.fg(Color::rgb(180, 180, 180));
}
static Style log_neko_s() {
  return Style{}.fg(Color::rgb(100, 200, 255));
}
static Style log_err_s() {
  return Style{}.fg(Color::rgb(255, 120, 120));
}

static void wt(Frame& f, int x, int y, const char* s, Style st) {
  Cell c = Cell::from_char(U' ', st);
  glyph::view::draw_text(f, Point{x, y}, s, c);
}

static void draw_border(Frame& f, const Rect& r, const char* title, Style title_st) {
  const int x0 = r.left(), y0 = r.top(), x1 = r.right() - 1, y1 = r.bottom() - 1;
  for (int x = x0 + 1; x < x1; ++x) {
    f.set(Point{x, y0}, Cell::from_char(U'\u2500', brd_s()));
    f.set(Point{x, y1}, Cell::from_char(U'\u2500', brd_s()));
  }
  for (int y = y0 + 1; y < y1; ++y) {
    f.set(Point{x0, y}, Cell::from_char(U'\u2502', brd_s()));
    f.set(Point{x1, y}, Cell::from_char(U'\u2502', brd_s()));
  }
  f.set(Point{x0, y0}, Cell::from_char(U'\u256D', brd_s()));
  f.set(Point{x1, y0}, Cell::from_char(U'\u256E', brd_s()));
  f.set(Point{x0, y1}, Cell::from_char(U'\u2570', brd_s()));
  f.set(Point{x1, y1}, Cell::from_char(U'\u256F', brd_s()));
  if (title && title[0]) {
    // Title embedded in top border
    Cell tc = Cell::from_char(U' ', title_st);
    glyph::view::draw_text(f, Point{x0 + 2, y0}, title, tc);
  }
}

static void draw_monitor(Frame& f, const Rect& area, bool reloaded, int tick) {
  draw_border(f, area, " Nekomata ", title_s());
  const int x = area.left() + 3;
  const int y0 = area.top() + 1;
  int y = y0;

  wt(f, x, y, "Nekomata", title_s());
  wt(f, area.right() - 12, y, "(=･ω･=)", title_s());
  y++;
  wt(f, x, y, "native hot-reload · no restarts", tag_s());
  y += 2;

  wt(f, x, y, "── watching ", sec_s());
  y++;
  wt(f, x + 2, y, "/tmp/hot.new.o", dim_s());
  y += 2;

  wt(f, x, y, "── pending ", sec_s());
  y++;
  if (!reloaded) {
    wt(f, x + 2, y, "(=･ω･=) monitoring...", info_s());
    y += 2;
  } else {
    wt(f, x + 2, y, "(=･ω･=) hot.o — 2 functions ready", info_s());
    y++;
    wt(f, x + 6, y, "· tick()         hot.cpp:12", dim_s());
    y++;
    wt(f, x + 6, y, "· draw_slider()  hot.cpp:87", dim_s());
    y += 2;
  }

  wt(f, x, y, "── history ", sec_s());
  y++;
  if (reloaded) {
    wt(f, x + 2, y, "(=^ω^=)  14:03:42  applied  2 fn", ok_s());
    y++;
    wt(f, x + 2, y, "(=×ω×=)  14:01:05  rejected", err_s());
    y++;
  } else {
    wt(f, x + 2, y, "(=･ω･=)  no reloads yet", dim_s());
    y++;
  }

  // Key bindings at bottom
  const int ky = area.bottom() - 2;
  wt(f, x, ky, "(=･ω･=) monitoring", info_s());
  wt(f, x, ky + 1, "  ", dim_s());
  wt(f, x + 2, ky + 1, "r", key_s());
  wt(f, x + 3, ky + 1, " reload ", dim_s());
  wt(f, x + 11, ky + 1, "s", key_s());
  wt(f, x + 12, ky + 1, " skip ", dim_s());
  wt(f, x + 18, ky + 1, "q", key_s());
  wt(f, x + 19, ky + 1, " quit", dim_s());
}

struct log_line {
  std::string text;
  Style style;
};

static void draw_log(Frame& f, const Rect& area, const std::deque<log_line>& lines) {
  draw_border(f, area, " app output ", dim_s());
  const int x = area.left() + 2;
  const int y0 = area.top() + 1;
  const int max_lines = area.size.h - 3;
  const std::size_t skip = lines.size() > static_cast<std::size_t>(max_lines)
                               ? lines.size() - static_cast<std::size_t>(max_lines)
                               : 0;
  int y = y0;
  for (std::size_t i = skip; i < lines.size() && y < area.bottom() - 1; ++i, ++y) {
    wt(f, x, y, lines[i].text.c_str(), lines[i].style);
  }
}

int main() {
  // FullScreenGuard: alt screen + cursor + terminal state, restored on exit.
  glyph::render::FullScreenGuard guard{std::cout};

  // Simulated app log
  std::deque<log_line> logs;
  const char* boot[] = {
      "[app] initializing renderer...",          "[app] loading assets (247 files)",
      "[app] window created 1920x1080",          "[app] shader compile: 12 programs",
      "(=･ω･=) process symbols: 2972 functions",
  };
  for (const auto* l : boot) {
    const bool is_neko = l[0] == '(';
    logs.push_back({l, is_neko ? log_neko_s() : log_app_s()});
  }

  for (int tick = 0; tick < 200; ++tick) {
    // Push a new log line every tick
    char buf[128];
    if (tick == 6) {
      logs.push_back({"(=^ω^=) reload applied: 2 function(s) redirected", ok_s()});
      snprintf(buf, sizeof(buf), "[app] tick %d: fps=%.1f mem=48MB (reloaded)", tick, 59.9);
    } else if (tick == 12) {
      logs.push_back({"(=×ω×=) reload rejected: ambiguous symbol", err_s()});
      snprintf(buf, sizeof(buf), "[app] tick %d: fps=60.0 mem=48MB", tick, 60.0);
    } else if (tick % 20 == 0) {
      snprintf(buf, sizeof(buf), "[app] tick %d: fps=%.1f mem=%dMB", tick, 58.0 + (tick % 30) * 0.1,
               42 + (tick / 40) % 8);
    } else {
      snprintf(buf, sizeof(buf), "[app] tick %d: fps=%.1f mem=%dMB", tick, 59.0 + (tick % 10) * 0.1,
               42 + (tick / 40) % 8);
    }
    logs.push_back({buf, log_app_s()});
    if (logs.size() > 50) {
      logs.pop_front();
    }

    const bool reloaded = tick > 6;

    // Terminal size
    struct winsize ws;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
    const int W = ws.ws_col > 0 ? ws.ws_col : 120;
    const int H = ws.ws_row > 0 ? ws.ws_row : 32;

    if (tick == 0) {
      std::fprintf(stderr, "  terminal %dx%d -> %s layout\n", W, H,
                   W >= 100 ? "side-by-side" : "stacked");
    }

    // Layout: side-by-side when wide, stacked when narrow
    Rect monitor_area, log_area;
    if (W >= 100) {
      const int mw = std::max(40, W * 2 / 5); // 40% for monitor
      monitor_area = Rect{0, 0, mw, H};
      log_area = Rect{mw, 0, W - mw, H};
    } else {
      const int mh = std::min(H - 8, std::max(16, H * 3 / 5)); // 60% for monitor
      monitor_area = Rect{0, 0, W, mh};
      log_area = Rect{0, mh, W, H - mh};
    }

    Frame frame{Size{W, H}, Cell::from_char(U' ')};
    draw_monitor(frame, monitor_area, reloaded, tick);
    draw_log(frame, log_area, logs);

    std::ostringstream oss;
    glyph::render::AnsiRenderer renderer{oss};
    renderer.render(frame);
    std::fputs("\033[H", stdout); // home cursor before writing
    std::fputs(oss.str().c_str(), stdout);
    std::fflush(stdout);

    std::this_thread::sleep_for(std::chrono::milliseconds(tick == 6 ? 1500 : 400));
  }

  return 0; // guard restores
}
