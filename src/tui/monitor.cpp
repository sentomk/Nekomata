// monitor — renders reload session state.
//
// mode::inline_status: styled line(s) in the current terminal (default).
// mode::terminal:      separate terminal window via pty, Nekomata panel
//                      with optional dual-pane layout (monitor + app log).

#include <neko/cats.hpp>
#include <neko/tui/tui.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <deque>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "glyph/core/cell.h"
#include "glyph/core/color.h"
#include "glyph/core/event.h"
#include "glyph/core/geometry.h"
#include "glyph/core/style.h"
#include "glyph/input/detail/vt_decoder.h"
#include "glyph/platform/clipboard.h"
#include "glyph/render/ansi/ansi_renderer.h"
#include "glyph/render/terminal.h"
#include "glyph/view/components/scroll_region.h"
#include "glyph/view/components/status_line.h"
#include "glyph/view/frame.h"
#include "glyph/view/text.h"

#include "terminal.hpp"

namespace neko::tui {
namespace {

using glyph::core::Cell;
using glyph::core::Color;
using glyph::core::Point;
using glyph::core::Rect;
using glyph::core::Size;
using glyph::core::Style;
using glyph::view::Frame;
using glyph::view::ScrollRegionView;
using glyph::view::StatusLineView;

// Render cadence: fast enough that wheel scrolling and drag-select feel
// immediate, slow enough to stay invisible in `top`.
constexpr int kTickMs = 100;

// Input is polled on a much shorter slice than the frame cadence; the
// renderer only writes what changed, so checking input costs nothing.
constexpr int kInputPollMs = 5;

// Ceiling for redraws triggered by input (~60 fps). Dragging a selection
// has to follow the pointer, and waiting for the next cadence tick made it
// feel stuck to the table; the diff renderer keeps an extra frame cheap.
constexpr int kInteractiveFrameMs = 16;

// Logo animation cycle. The cat stretches, watches for a while, and
// stretches again — the idle beat is deliberately much longer than the
// animation so the panel reads as calm rather than busy.
constexpr int kLogoFrameMs = 1600;                                 // per animation frame
constexpr int kLogoAnimMs = kLogoFrameMs * cats::logo_frame_count; // stretch
constexpr int kLogoIdleMs = 12000;                                 // watch
constexpr int kLogoCycleMs = kLogoAnimMs + kLogoIdleMs;
constexpr int kLogoFlashMs = 1200;    // alert flash after a reload
constexpr int kLogoFlashStepMs = 150; // how fast it alternates

// Nekomata palette.
Style title_style() {
  return Style{}.fg(Color::rgb(255, 180, 100));
}
Style tagline_style() {
  return Style{}.fg(Color::rgb(120, 120, 120));
}
Style section_style() {
  return Style{}.fg(Color::rgb(90, 90, 90));
}
Style info_style() {
  return Style{}.fg(Color::rgb(100, 200, 255));
}
Style ok_style() {
  return Style{}.fg(Color::rgb(100, 220, 100));
}
Style error_style() {
  return Style{}.fg(Color::rgb(255, 100, 100));
}
Style warn_style() {
  return Style{}.fg(Color::rgb(255, 200, 100));
}
Style dim_style() {
  return Style{}.fg(Color::rgb(140, 140, 140));
}
Style key_style() {
  return Style{}.fg(Color::rgb(255, 255, 255));
}
Style border_style() {
  return Style{}.fg(Color::rgb(70, 70, 70));
}

/// Write styled text into a Frame at (x, y) — grapheme-aware.
void write_text(Frame& f, int x, int y, const std::string& text, Style style) {
  Cell c = Cell::from_char(U' ', style);
  glyph::view::draw_text(f, Point{x, y}, text, c);
}

void draw_border(Frame& f, const Rect& r, const std::string& title, Style title_st) {
  const int x0 = r.left(), y0 = r.top(), x1 = r.right() - 1, y1 = r.bottom() - 1;
  const Style bs = border_style();
  for (int x = x0 + 1; x < x1; ++x) {
    f.set(Point{x, y0}, Cell::from_char(U'\u2500', bs));
    f.set(Point{x, y1}, Cell::from_char(U'\u2500', bs));
  }
  for (int y = y0 + 1; y < y1; ++y) {
    f.set(Point{x0, y}, Cell::from_char(U'\u2502', bs));
    f.set(Point{x1, y}, Cell::from_char(U'\u2502', bs));
  }
  f.set(Point{x0, y0}, Cell::from_char(U'\u256D', bs));
  f.set(Point{x1, y0}, Cell::from_char(U'\u256E', bs));
  f.set(Point{x0, y1}, Cell::from_char(U'\u2570', bs));
  f.set(Point{x1, y1}, Cell::from_char(U'\u256F', bs));
  if (!title.empty()) {
    write_text(f, x0 + 2, y0, title, title_st);
  }
}

/// One line in the application log pane.
struct log_entry {
  std::string text;
  Style style;
};

/// How many lines may be held back while the reader is scrolled away.
constexpr std::size_t kHeldCap = 500;

/// Incremental UTF-8 decoder: the VT decoder wants code points, the pty
/// hands us bytes, and a multi-byte sequence can be split across reads.
struct utf8_decoder {
  char32_t cp = 0;
  std::uint8_t need = 0;
  std::uint32_t acc = 0;

  /// Feed one byte; returns true when `out` holds a complete code point.
  bool feed(unsigned char b, char32_t& out) {
    if (need == 0) {
      if (b < 0x80) {
        out = b;
        return true;
      }
      if ((b & 0xE0) == 0xC0) {
        acc = b & 0x1F;
        need = 1;
      } else if ((b & 0xF0) == 0xE0) {
        acc = b & 0x0F;
        need = 2;
      } else if ((b & 0xF8) == 0xF0) {
        acc = b & 0x07;
        need = 3;
      }
      // Stray continuation / invalid lead: drop the byte.
      return false;
    }
    if ((b & 0xC0) != 0x80) { // lost sync mid-sequence
      need = 0;
      acc = 0;
      return false;
    }
    acc = (acc << 6) | (b & 0x3F);
    if (--need == 0) {
      out = static_cast<char32_t>(acc);
      return true;
    }
    return false;
  }
};

} // namespace

// ---------------------------------------------------------------------------
// Terminal monitor: owns a pty, renders in a background thread.
// ---------------------------------------------------------------------------
class monitor::terminal_monitor {
public:
  terminal_monitor(reload_session& session, const monitor_config& config)
      : session_(session), title_(config.title), show_log_(config.show_app_log), running_{true},
        fullscreen_(config.render_mode == mode::fullscreen) {
    renderer_ = std::make_unique<glyph::render::AnsiRenderer>(out_);
    terminal_ =
        detail::terminal::open(fullscreen_ ? detail::placement::current : detail::placement::pty);
    render_thread_ = std::thread([this] { loop(); });
  }

  void log_line(std::string text) { queue_log(std::move(text), dim_style()); }

  [[nodiscard]] bool running() const { return running_.load(); }

  ~terminal_monitor() {
    running_.store(false);
    if (render_thread_.joinable()) {
      render_thread_.join();
    }
    // terminal_ restores the host terminal on destruction.
  }

private:
  void loop() {
    write_display("\033[?1049h\033[?25l"); // alt screen + hide cursor
    // Mouse reporting, matching what Glyph's own input backend enables:
    // 1000 = press/release, 1002 = button-event tracking (motion while a
    // button is held, i.e. drag), 1006 = SGR coordinates. Dropping 1002
    // costs both drag-select and, on several terminals, the wheel.
    write_display("\033[?1000h\033[?1002h\033[?1006h");
    int frame_count = 0;
    auto next_frame = std::chrono::steady_clock::now();
    auto last_frame = next_frame - std::chrono::milliseconds(kTickMs);
    while (running_.load()) {
      const auto now = std::chrono::steady_clock::now();
      const bool cadence_due = now >= next_frame;
      const bool interactive_due =
          dirty_ && now - last_frame >= std::chrono::milliseconds(kInteractiveFrameMs);
      if (cadence_due || interactive_due) {
        render_frame(frame_count);
        ++frame_count;
        last_frame = now;
        dirty_ = false;
        next_frame = now + std::chrono::milliseconds(kTickMs);
      }
      // Poll input on a short slice: rendering at the frame cadence but
      // sleeping on it too made every wheel notch and keypress wait up to
      // a whole frame before it was even looked at.
      ++tick_;
      handle_input_nb();
      std::this_thread::sleep_for(std::chrono::milliseconds(kInputPollMs));
    }
    write_display("\033[?1000l\033[?1002l\033[?1006l"); // mouse off
    write_display("\033[2J\033[H\033[?25h\033[?1049l"); // restore
  }

  void render_frame(int frame_count) {
    const auto stats = session_.session_stats();

    // Session events and application lines both go through the queue;
    // the view only receives them while the reader is at the bottom.
    if (stats.applied != last_applied_ || stats.rejected != last_rejected_) {
      if (stats.applied > last_applied_) {
        queue_log(std::string(cats::ok) + " reload applied: " + stats.last_result, ok_style());
      }
      if (stats.rejected > last_rejected_) {
        queue_log(std::string(cats::error) + " rejected: " + stats.last_result, error_style());
      }
      last_applied_ = stats.applied;
      last_rejected_ = stats.rejected;
    }
    flush_logs_if_following();

    // Terminal size, straight from the platform layer: the pty in terminal
    // mode, the real terminal in fullscreen mode.
    const Size term_size = terminal_->size();
    const int term_w = term_size.w;
    const int term_h = term_size.h;

    // Fresh frame with the actual terminal size (not a fixed member).
    Frame frame{Size{term_w, term_h}, Cell::from_char(U' ')};

    // Layout.
    constexpr int MONITOR_W = 48;
    constexpr int MONITOR_H = 22;
    constexpr int LOG_W = 52;

    log_area_ = Rect{}; // no log pane until the layout grants one
    Rect monitor_area;
    if (show_log_ && term_w >= MONITOR_W + LOG_W + 1) {
      // Dual pane, side-by-side, centered.
      const int total_w = MONITOR_W + 1 + LOG_W;
      const int total_h = MONITOR_H;
      const int ox = std::max(0, (term_w - total_w) / 2);
      const int oy = std::max(0, (term_h - total_h) / 2);
      monitor_area = Rect{ox, oy, MONITOR_W, MONITOR_H};
      log_area_ = Rect{ox + MONITOR_W + 1, oy, LOG_W, MONITOR_H};
    } else if (show_log_ && term_h >= MONITOR_H + 16) {
      // Dual pane, stacked, centered.
      const int total_h = MONITOR_H + 1 + 14;
      const int ox = std::max(0, (term_w - MONITOR_W) / 2);
      const int oy = std::max(0, (term_h - total_h) / 2);
      monitor_area = Rect{ox, oy, MONITOR_W, MONITOR_H};
      log_area_ = Rect{ox, oy + MONITOR_H + 1, MONITOR_W, 14};
    } else {
      // Single pane, centered.
      const int ox = std::max(0, (term_w - MONITOR_W) / 2);
      const int oy = std::max(0, (term_h - MONITOR_H) / 2);
      monitor_area = Rect{ox, oy, MONITOR_W, MONITOR_H};
    }

    render_monitor_pane(frame, monitor_area, stats, frame_count);
    if (!log_area_.empty()) {
      render_log_pane(frame, log_area_);
    }

    // Flush. The renderer diffs against the frame it drew last time, so
    // only what actually changed goes out.
    out_.str({});
    out_.clear();
    renderer_->render(frame);
    write_display(out_.str());
  }

  void render_log_pane(Frame& frame, const Rect& area) {
    const std::size_t held = held_count();
    const std::string title =
        held > 0 ? " log  +" + std::to_string(held) + " new " : std::string(" log ");
    draw_border(frame, area, title, held > 0 ? warn_style() : dim_style());
    const Rect inner{area.left() + 2, area.top() + 1, area.size.w - 4, area.size.h - 2};
    if (inner.empty()) {
      return;
    }
    if (log_view_.line_count() == 0) {
      write_text(frame, inner.left(), inner.top(),
                 std::string(cats::info) + " waiting for events...", dim_style());
      return;
    }
    // ScrollRegionView owns scrollback, wrapping, wheel scrolling, and the
    // selection highlight; we only hand it the area.
    log_view_.render(frame, inner);
  }

  /// Queue one log line. Lines are held while the reader is scrolled away
  /// from the live end and flushed the moment they come back — see
  /// flush_logs_if_following().
  void queue_log(std::string text, Style style) {
    std::lock_guard<std::mutex> lock(log_mutex_);
    if (pending_logs_.size() >= kHeldCap) {
      pending_logs_.pop_front(); // the pane keeps 500 lines anyway
    }
    pending_logs_.push_back(log_entry{std::move(text), style});
    dirty_ = true;
  }

  /// Push held lines into the view, but only while the reader is at the
  /// bottom. ScrollRegionView::push_line re-follows the bottom
  /// unconditionally, so pushing under a scrolled-up reader would yank
  /// them away from what they are reading — and a live log feeds lines
  /// faster than anyone can read them. Held lines appear as soon as the
  /// reader scrolls back down.
  void flush_logs_if_following() {
    if (log_view_.scroll_offset() != 0) {
      return;
    }
    std::deque<log_entry> fresh;
    {
      std::lock_guard<std::mutex> lock(log_mutex_);
      fresh.swap(pending_logs_);
    }
    for (auto& entry : fresh) {
      log_view_.push_line(std::move(entry.text), entry.style);
    }
  }

  [[nodiscard]] std::size_t held_count() {
    std::lock_guard<std::mutex> lock(log_mutex_);
    return pending_logs_.size();
  }

  /// True if the point falls inside the currently rendered log pane.
  [[nodiscard]] bool in_log_pane(Point p) const {
    return !log_area_.empty() && p.x >= log_area_.left() && p.x < log_area_.right() &&
           p.y >= log_area_.top() && p.y < log_area_.bottom();
  }

  [[nodiscard]] Point clamp_to_log(Point p) const {
    if (log_area_.empty()) {
      return p;
    }
    p.x =
        std::clamp(p.x, log_area_.left(), static_cast<glyph::core::coord_t>(log_area_.right() - 1));
    p.y =
        std::clamp(p.y, log_area_.top(), static_cast<glyph::core::coord_t>(log_area_.bottom() - 1));
    return p;
  }

  void handle_input_nb() {
    const std::string bytes = terminal_->read(std::chrono::milliseconds(kInputPollMs));
    for (const char raw : bytes) {
      char32_t cp = 0;
      if (utf8_.feed(static_cast<unsigned char>(raw), cp)) {
        decoder_.feed(cp);
      }
    }
    decoder_.flush(true);

    while (decoder_.has_event()) {
      dispatch(decoder_.pop());
      dirty_ = true; // an event may have moved the selection or the view
    }
  }

  void dispatch(const glyph::core::Event& ev) {
    if (const auto* key = std::get_if<glyph::core::KeyEvent>(&ev)) {
      dispatch_key(*key);
    } else if (const auto* m = std::get_if<glyph::core::MouseEvent>(&ev)) {
      dispatch_mouse(*m);
    }
  }

  void dispatch_key(const glyph::core::KeyEvent& key) {
    if (key.code == glyph::core::KeyCode::Esc) {
      if (log_view_.selection_active()) {
        log_view_.select_clear();
      } else {
        running_.store(false);
      }
      return;
    }
    if (key.code != glyph::core::KeyCode::Char || key.mods != glyph::core::Mod::None) {
      return;
    }
    switch (key.ch) {
    case U'y':
      copy_selection();
      break;
    case U'r':
      try {
        session_.update();
      } catch (...) {
      }
      break;
    case U'q':
      running_.store(false);
      break;
    default:
      break;
    }
  }

  void dispatch_mouse(const glyph::core::MouseEvent& m) {
    // Wheel: the log pane is the only scrollable region on screen, so the
    // wheel drives it from anywhere rather than only when the pointer is
    // parked inside it. Requiring the pointer inside was the first version
    // and it reads as "the wheel is broken" — the natural gesture is to
    // scroll wherever the reader is looking, which is usually not the pane
    // under the cursor.
    if (m.action == glyph::core::MouseAction::Scroll) {
      if (!log_area_.empty()) {
        log_view_.on_mouse(m);
      }
      return;
    }
    if (!in_log_pane(m.pos) && !log_view_.selection_active()) {
      return;
    }

    if (m.action == glyph::core::MouseAction::Down && m.button == glyph::core::MouseButton::Left) {
      log_view_.select_begin(clamp_to_log(m.pos));
      note_.clear();
    } else if ((m.action == glyph::core::MouseAction::Drag ||
                m.action == glyph::core::MouseAction::Move) &&
               log_view_.selection_active()) {
      // Dragging past an edge pulls more output into view, like tmux. The
      // selection is content-anchored, so rows swept out mid-drag still
      // make it into extract_selection().
      if (!log_area_.empty() && m.pos.y <= log_area_.top()) {
        log_view_.scroll_up(3);
      } else if (!log_area_.empty() && m.pos.y >= log_area_.bottom() - 1) {
        log_view_.scroll_down(3);
      }
      log_view_.select_extend(clamp_to_log(m.pos));
    } else if (m.action == glyph::core::MouseAction::Up &&
               m.button == glyph::core::MouseButton::Left) {
      copy_selection();
    }
  }

  void copy_selection() {
    if (!log_view_.selection_active()) {
      return;
    }
    const std::string text = log_view_.extract_selection();
    if (text.empty()) {
      return;
    }
    if (glyph::platform::copy_to_clipboard(text)) {
      note_ = "copied " + std::to_string(text.size()) + " bytes";
    } else {
      note_ = "clipboard unavailable";
    }
    note_tick_ = tick_;
    log_view_.select_clear();
  }

  /// Which logo frame belongs on screen right now.
  ///
  /// Clock-driven, not render-counter-driven: interactive redraws fire far
  /// more often than the cadence, and an animation stepped by the render
  /// counter would visibly run fast while the user drags a selection.
  ///
  /// The cycle is animation → idle → animation. A reload interrupts it with
  /// a short alert flash, because that is the one state worth noticing.
  [[nodiscard]] int logo_frame_at(std::chrono::steady_clock::time_point now,
                                  const reload_session::stats& stats) {
    const auto since_start =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - started_).count();

    if (stats.applied > last_logo_applied_) {
      if (!flashing_) {
        flashing_ = true;
        flash_until_ = now + std::chrono::milliseconds(kLogoFlashMs);
      }
      if (now < flash_until_) {
        const auto phase =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - started_).count();
        return (phase / kLogoFlashStepMs) % 2 == 0 ? 2 : cats::logo_frame_count - 1;
      }
      flashing_ = false;
      last_logo_applied_ = stats.applied;
    }

    const auto phase = since_start % kLogoCycleMs;
    if (phase >= kLogoAnimMs) {
      return cats::logo_frame_count - 1; // idle: content, eyes closed
    }
    return std::min(static_cast<int>(phase / kLogoFrameMs), cats::logo_frame_count - 1);
  }

  void render_monitor_pane(Frame& frame, const Rect& area, const reload_session::stats& stats,
                           int frame_count) {
    draw_border(frame, area, " Nekomata Monitor ", title_style());
    const int x = area.left() + 3;
    int y = area.top() + 2;

    // Nekomata animated logo (top-right): stretch, idle, stretch again.
    const int logo_frame = logo_frame_at(std::chrono::steady_clock::now(), stats);
    write_text(frame, area.right() - 13, y, cats::logo_frames[logo_frame], title_style());
    y += 2;
    write_text(frame, x, y, "native hot reload engine for everyone.", tagline_style());
    y += 3;

    write_text(frame, x, y, "\u2500\u2500 watching ", section_style());
    y++;
    for (const auto& path : stats.watched_paths) {
      write_text(frame, x + 2, y, path, dim_style());
      y++;
    }
    y++;

    write_text(frame, x, y, "\u2500\u2500 pending ", section_style());
    y++;
    if (stats.applied > 0 || stats.rejected > 0) {
      const Style st = stats.rejected > 0 ? warn_style() : info_style();
      write_text(frame, x + 2, y, std::string(cats::info) + " " + stats.last_result, st);
      y++;
    } else {
      write_text(frame, x + 2, y, std::string(cats::info) + " monitoring...", info_style());
      y++;
    }
    y++;

    write_text(frame, x, y, "\u2500\u2500 history ", section_style());
    y++;
    write_text(frame, x + 2, y,
               std::to_string(stats.applied) + " applied, " + std::to_string(stats.rejected) +
                   " rejected",
               dim_style());

    // Footer: a transient note wins over the key hints.
    const int ky = area.bottom() - 3;
    if (!note_.empty() && tick_ - note_tick_ < 30) {
      write_text(frame, x, ky, note_, ok_style());
    } else {
      char status[80];
      std::snprintf(status, sizeof(status), "%s frame %d", cats::logo, frame_count);
      write_text(frame, x, ky, status, info_style());
      write_text(frame, x + 2, ky + 1, " ", dim_style());
      write_text(frame, x + 3, ky + 1, "r", key_style());
      write_text(frame, x + 4, ky + 1, " reload ", dim_style());
      write_text(frame, x + 12, ky + 1, "wheel", key_style());
      write_text(frame, x + 17, ky + 1, " scroll ", dim_style());
      write_text(frame, x + 25, ky + 1, "q", key_style());
      write_text(frame, x + 26, ky + 1, " quit", dim_style());
    }
  }

  /// Hand a frame to the display. The terminal layer owns the
  /// non-blocking, drop-what-the-reader-is-not-taking contract.
  void write_display(std::string_view bytes) {
    if (!terminal_->write(bytes)) {
      renderer_->reset();
    }
  }

  reload_session& session_;
  std::string title_;
  bool show_log_;
  std::atomic<bool> running_;
  std::thread render_thread_;
  bool fullscreen_ = false;
  std::unique_ptr<detail::terminal> terminal_;
  std::size_t last_applied_ = 0;
  std::size_t last_logo_applied_ = 0;
  std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point flash_until_{};
  bool flashing_ = false;
  std::size_t last_rejected_ = 0;

  /// App log pane: scrollback, wrap, wheel scrolling, and content-anchored
  /// selection all live inside the view.
  ScrollRegionView log_view_{500};
  std::atomic<bool> dirty_{true}; // render before the next cadence tick
  // One renderer for the whole session: AnsiRenderer keeps the previous
  // frame and emits only the dirty lines, so a fresh renderer per frame
  // (the first version) paid a full-screen repaint ten times a second —
  // ~90 KB/s of escapes with a static panel.
  std::ostringstream out_;
  std::unique_ptr<glyph::render::AnsiRenderer> renderer_;
  std::mutex log_mutex_;
  std::deque<log_entry> pending_logs_;
  glyph::input::detail::VtDecoder decoder_{};
  utf8_decoder utf8_{};
  Rect log_area_{};       // last rendered log pane (mouse hit-testing)
  std::string note_;      // transient footer message ("copied N bytes")
  std::int64_t tick_ = 0; // input-loop ticks, for note expiry
  std::int64_t note_tick_ = 0;
};

// ---------------------------------------------------------------------------
// Public monitor: dispatches by mode.
// ---------------------------------------------------------------------------

monitor::monitor(reload_session& session, monitor_config config)
    : session_(session), config_(std::move(config)) {
  if (config_.render_mode != mode::inline_status) {
    // Both fullscreen and terminal own a render thread; only the output
    // sink differs (this terminal vs a pty).
    terminal_ = std::make_unique<terminal_monitor>(session_, config_);
  }
}

monitor::~monitor() = default;

void monitor::log_line(std::string text) {
  if (terminal_) {
    terminal_->log_line(std::move(text));
  }
}

bool monitor::running() const {
  return !terminal_ || terminal_->running();
}

void monitor::render() {
  if (config_.render_mode != mode::inline_status) {
    return; // the render thread draws on its own cadence
  }

  // mode::inline_status — styled line in the current terminal.
  const auto stats = session_.session_stats();

  StatusLineView line;
  line.add_segment(config_.title, title_style());
  line.add_segment("  ", Style{});

  if (stats.applied > 0 || stats.rejected > 0) {
    line.add_segment(std::to_string(stats.applied) + " applied", ok_style());
    if (stats.rejected > 0) {
      line.add_segment(", ", dim_style());
      line.add_segment(std::to_string(stats.rejected) + " rejected", warn_style());
    }
    line.add_segment("  ", Style{});
    line.add_segment("last: " + stats.last_result, dim_style());
  } else {
    line.add_segment("waiting...", dim_style());
  }

  Frame frame{Size{100, 1}};
  line.render(frame, Rect{0, 0, 100, 1});
  glyph::render::AnsiRenderer renderer{std::cerr};
  renderer.render(frame);
}

} // namespace neko::tui
