// monitor — renders reload session state.
//
// mode::inline_status: styled line(s) in the current terminal (default).
// mode::terminal:      separate terminal window via pty, Nekomata panel
//                      with optional dual-pane layout (monitor + app log).

#include <neko/cats.hpp>
#include <neko/tui/tui.hpp>

#include <atomic>
#include <chrono>
#include <deque>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "glyph/core/cell.h"
#include "glyph/core/color.h"
#include "glyph/core/geometry.h"
#include "glyph/core/style.h"
#include "glyph/render/ansi/ansi_renderer.h"
#include "glyph/render/terminal.h"
#include "glyph/view/components/status_line.h"
#include "glyph/view/frame.h"
#include "glyph/view/text.h"

namespace neko::tui {
namespace {

using glyph::core::Cell;
using glyph::core::Color;
using glyph::core::Point;
using glyph::core::Rect;
using glyph::core::Size;
using glyph::core::Style;
using glyph::view::Frame;
using glyph::view::StatusLineView;

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

struct log_entry {
  std::string text;
  Style style;
};

/// Nekomata logo animation: 5 frames, 9 columns each.
///   0: reaching out (curious)     っ(=•ω•=)っ
///   1: left paw forms             ฅ(=•ω•=)っ
///   2: both paws, eyes open       ฅ(=•ω•=)ฅ   ← alert
///   3: left eye closes            ฅ(=─ω•=)ฅ
///   4: fully content              ฅ(=─ω─=)ฅ   ← resting
} // namespace

// ---------------------------------------------------------------------------
// Terminal monitor: owns a pty, renders in a background thread.
// ---------------------------------------------------------------------------
class monitor::terminal_monitor {
public:
  terminal_monitor(reload_session& session, const monitor_config& config)
      : session_(session), title_(config.title), show_log_(config.show_app_log), running_{true} {
    open_pty();
    render_thread_ = std::thread([this] { loop(); });
  }

  ~terminal_monitor() {
    running_.store(false);
    if (render_thread_.joinable()) {
      render_thread_.join();
    }
    if (master_fd_ >= 0) {
      ::close(master_fd_);
    }
  }

private:
  void open_pty() {
    master_fd_ = ::posix_openpt(O_RDWR | O_NOCTTY);
    if (master_fd_ < 0) {
      throw std::runtime_error("neko::tui: cannot create pty");
    }
    ::grantpt(master_fd_);
    ::unlockpt(master_fd_);
    slave_path_ = ::ptsname(master_fd_);

    // The attached terminal (screen/tmux) needs to know the pty size.
    // Without this the pty defaults to 0x0 and rendering is invisible.
    // Sync the pty size from the host process's terminal. screen/tmux
    // don't propagate sizes to external ptys (they're not the session
    // leader), so the pty would stay at kernel default (0x0) forever.
    // Assume the attached terminal is the same size as the host's.
    struct winsize host_ws;
    if (::ioctl(STDERR_FILENO, TIOCGWINSZ, &host_ws) == 0 && host_ws.ws_col > 0) {
      ::ioctl(master_fd_, TIOCSWINSZ, &host_ws);
    }

    // Raw mode: ANSI escape sequences must pass through the line
    // discipline unmodified. Without this, ONLCR translates \n to
    // \r\n (breaking cursor positioning) and ECHO reflects our output
    // back to the master (confusing input handling).
    struct termios raw {};
    if (::tcgetattr(master_fd_, &raw) == 0) {
      raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
      raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
      raw.c_oflag &= ~(OPOST | ONLCR);
      raw.c_cc[VMIN] = 1;
      raw.c_cc[VTIME] = 0;
      ::tcsetattr(master_fd_, TCSANOW, &raw);
    }

    std::fprintf(stderr, "Nekomata TUI: %s\n", slave_path_.c_str());
    std::fprintf(stderr, "  attach:  tmux split-window 'screen %s'\n", slave_path_.c_str());
    std::string cmd = "tmux split-window -h 'screen " + slave_path_ + "' > /dev/null 2>&1 &";
    std::system(cmd.c_str());
  }

  void loop() {
    write_ansi("\033[?1049h\033[?25l"); // alt screen + hide cursor
    int frame_count = 0;
    while (running_.load()) {
      render_frame(frame_count);
      ++frame_count;
      // Non-blocking input check, then sleep for the render cadence.
      handle_input_nb();
      std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }
    write_ansi("\033[2J\033[H\033[?25h\033[?1049l"); // restore
  }

  void render_frame(int frame_count) {
    const auto stats = session_.session_stats();

    // Accumulate log entries from session stats.
    if (stats.applied != last_applied_ || stats.rejected != last_rejected_) {
      if (stats.applied > last_applied_) {
        logs_.push_back({"(=^\u03C9^=) reload applied: " + stats.last_result, ok_style()});
      }
      if (stats.rejected > last_rejected_) {
        logs_.push_back({"(=\u00D7\u03C9\u00D7=) rejected: " + stats.last_result, error_style()});
      }
      last_applied_ = stats.applied;
      last_rejected_ = stats.rejected;
      if (logs_.size() > 50) {
        logs_.pop_front();
      }
    }

    // Terminal size (from pty).
    struct winsize ws;
    ioctl(master_fd_, TIOCGWINSZ, &ws);
    const int term_w = ws.ws_col > 0 ? ws.ws_col : 80;
    const int term_h = ws.ws_row > 0 ? ws.ws_row : 24;

    // Fresh frame with the actual terminal size (not a fixed member).
    Frame frame{Size{term_w, term_h}, Cell::from_char(U' ')};

    // Layout.
    constexpr int MONITOR_W = 48;
    constexpr int MONITOR_H = 22;
    constexpr int LOG_W = 52;

    Rect monitor_area;
    if (show_log_ && term_w >= MONITOR_W + LOG_W + 1) {
      // Dual pane, side-by-side, centered.
      const int total_w = MONITOR_W + 1 + LOG_W;
      const int total_h = MONITOR_H;
      const int ox = std::max(0, (term_w - total_w) / 2);
      const int oy = std::max(0, (term_h - total_h) / 2);
      monitor_area = Rect{ox, oy, MONITOR_W, MONITOR_H};
      const Rect log_area{ox + MONITOR_W + 1, oy, LOG_W, MONITOR_H};
      render_log_pane(frame, log_area);
    } else if (show_log_ && term_h >= MONITOR_H + 16) {
      // Dual pane, stacked, centered.
      const int total_h = MONITOR_H + 1 + 14;
      const int ox = std::max(0, (term_w - MONITOR_W) / 2);
      const int oy = std::max(0, (term_h - total_h) / 2);
      monitor_area = Rect{ox, oy, MONITOR_W, MONITOR_H};
      const Rect log_area{ox, oy + MONITOR_H + 1, MONITOR_W, 14};
      render_log_pane(frame, log_area);
    } else {
      // Single pane, centered.
      const int ox = std::max(0, (term_w - MONITOR_W) / 2);
      const int oy = std::max(0, (term_h - MONITOR_H) / 2);
      monitor_area = Rect{ox, oy, MONITOR_W, MONITOR_H};
    }

    render_monitor_pane(frame, monitor_area, stats, frame_count);

    // Flush.
    std::ostringstream oss;
    glyph::render::AnsiRenderer renderer{oss};
    renderer.render(frame);
    write_ansi("\033[H"); // home cursor; full-frame render handles the rest
    write_ansi(oss.str());
  }

  void render_monitor_pane(Frame& frame, const Rect& area, const reload_session::stats& stats,
                           int frame_count) {
    draw_border(frame, area, " Nekomata Monitor ", title_style());
    const int x = area.left() + 3;
    int y = area.top() + 2;

    // Nekomata animated logo (top-right).
    // Startup: play frames 0→4 (curious → content).
    // Monitoring: rest on frame 4. A reload flashes frame 2 (alert).
    y = area.top() + 2;
    int logo_frame;
    if (frame_count < cats::logo_frame_count * 6) {
      logo_frame = std::min(frame_count / 6, cats::logo_frame_count - 1); // intro (slow)
    } else {
      const std::size_t applied_now = stats.applied;
      if (applied_now > last_logo_applied_) {
        // A reload just happened — flash the alert frame briefly.
        logo_frame = (frame_count % 8 < 4) ? 2 : 4;
        if (frame_count % 8 == 7) {
          last_logo_applied_ = applied_now;
        }
      } else {
        logo_frame = 4; // resting: content, eyes closed
      }
    }
    write_text(frame, area.right() - 12, y, cats::logo_frames[logo_frame], title_style());
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
      write_text(frame, x + 2, y, "(=\u00B7\u03C9\u00B7=) " + stats.last_result, st);
      y++;
    } else {
      write_text(frame, x + 2, y, "(=\u00B7\u03C9\u00B7=) monitoring...", info_style());
      y++;
    }
    y++;

    write_text(frame, x, y, "\u2500\u2500 history ", section_style());
    y++;
    write_text(frame, x + 2, y,
               std::to_string(stats.applied) + " applied, " + std::to_string(stats.rejected) +
                   " rejected",
               dim_style());

    // Key bindings
    const int ky = area.bottom() - 3;
    char status[80];
    std::snprintf(status, sizeof(status), "%s frame %d", cats::logo_frames[2], frame_count);
    write_text(frame, x, ky, status, info_style());
    write_text(frame, x + 2, ky + 1, " ", dim_style());
    write_text(frame, x + 3, ky + 1, "r", key_style());
    write_text(frame, x + 4, ky + 1, " reload ", dim_style());
    write_text(frame, x + 12, ky + 1, "s", key_style());
    write_text(frame, x + 13, ky + 1, " skip ", dim_style());
    write_text(frame, x + 19, ky + 1, "q", key_style());
    write_text(frame, x + 20, ky + 1, " quit", dim_style());
  }

  void render_log_pane(Frame& frame, const Rect& area) {
    draw_border(frame, area, " log ", dim_style());
    const int x = area.left() + 2;
    const int y0 = area.top() + 1;
    const int max_lines = area.size.h - 3;

    // Show recent entries, newest at bottom.
    const std::size_t skip = logs_.size() > static_cast<std::size_t>(max_lines)
                                 ? logs_.size() - static_cast<std::size_t>(max_lines)
                                 : 0;
    int y = y0;
    for (std::size_t i = skip; i < logs_.size() && y < area.bottom() - 1; ++i, ++y) {
      write_text(frame, x, y, logs_[i].text, logs_[i].style);
    }
    // Show a "waiting" line if empty.
    if (logs_.empty()) {
      write_text(frame, x, y0, "(=\u00B7\u03C9\u00B7=) waiting for events...", dim_style());
    }
  }

  void handle_input_nb() {
    // Set the master fd non-blocking for the duration of the read.
    const int flags = ::fcntl(master_fd_, F_GETFL, 0);
    ::fcntl(master_fd_, F_SETFL, flags | O_NONBLOCK);
    char buf[16];
    const ssize_t len = ::read(master_fd_, buf, sizeof(buf));
    ::fcntl(master_fd_, F_SETFL, flags); // restore
    if (len <= 0) {
      return; // no input available
    }
    for (ssize_t i = 0; i < len; ++i) {
      switch (buf[i]) {
      case 'q':
      case 'Q':
        running_.store(false);
        return;
      case 'r':
      case 'R':
        try {
          session_.update();
        } catch (...) {
        }
        break;
      default:
        break;
      }
    }
  }

  void write_ansi(const std::string& s) {
    if (master_fd_ >= 0) {
      ::write(master_fd_, s.data(), s.size());
    }
  }

  reload_session& session_;
  std::string title_;
  bool show_log_;
  std::atomic<bool> running_;
  std::thread render_thread_;
  int master_fd_ = -1;
  std::string slave_path_;
  std::size_t last_applied_ = 0;
  std::size_t last_logo_applied_ = 0;
  std::size_t last_rejected_ = 0;
  std::deque<log_entry> logs_;
};

// ---------------------------------------------------------------------------
// Public monitor: dispatches by mode.
// ---------------------------------------------------------------------------

monitor::monitor(reload_session& session, monitor_config config)
    : session_(session), config_(std::move(config)) {
  if (config_.render_mode == mode::terminal) {
    terminal_ = std::make_unique<terminal_monitor>(session_, config_);
  }
}

monitor::~monitor() = default;

void monitor::render() {
  if (config_.render_mode == mode::terminal) {
    return; // the terminal thread renders on its own cadence
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
