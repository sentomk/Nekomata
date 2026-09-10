// Mouse and input tests for the TUI monitor.
//
// These drive the real monitor through real ptys. The parent plays the
// attached terminal (reads the bytes, writes input sequences), the child runs
// the monitor with its stdio wired to a slave. That covers what a unit test
// cannot: the mouse-mode enable string, the decoder wiring, the log pane's
// hit-testing, the pty sizing handshake, and the sticky-bottom rule. Every one
// of those has failed silently in practice — the wheel did nothing because
// ?1002h was missing from the enable string; the panel rendered at 80x24 on
// macOS because TIOCSWINSZ on a master whose slave had never been opened is
// quietly ignored; and scrolling up was undone by the next log line because
// ScrollRegionView::push_line re-follows the bottom unconditionally.
//
// Assertions read a reconstructed screen, not the byte stream. The renderer
// diffs against its previous frame and emits absolute cursor moves plus the
// changed spans, so a byte stream carries fragments: grepping it for a whole
// line tests the rendering strategy rather than what a viewer sees.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "glyph/core/text.h"

#include <neko/runtime/code_substituter.hpp>
#include <neko/runtime/fwd.hpp>
#include <neko/runtime/object_loader.hpp>
#include <neko/runtime/state_manager.hpp>
#include <neko/runtime/symbol_provider.hpp>
#include <neko/tui/tui.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

namespace {

// ---------------------------------------------------------------------------
// Null backend: these tests exercise the monitor, not a real reload.
// ---------------------------------------------------------------------------
class null_loader final : public neko::object_loader {
public:
  neko::loaded_image load(const std::uint8_t*, std::size_t) override {
    throw std::runtime_error("null backend");
  }
};

class null_symbols final : public neko::symbol_provider, public neko::state_manager {
public:
  std::vector<neko::function_info> all_functions() const override { return {}; }
  std::optional<neko::function_info> function_by_name(std::string_view) const override {
    return std::nullopt;
  }
  std::size_t count_functions(std::string_view) const override { return 0; }
  std::optional<neko::global_variable> global_by_name(std::string_view) const override {
    return std::nullopt;
  }
  std::size_t count_globals(std::string_view) const override { return 0; }
  neko::type_layout layout_of(neko::type_id id) const override {
    neko::type_layout l;
    l.id = id;
    return l;
  }
  void* map_global(std::string_view) override { return nullptr; }
};

class null_substituter final : public neko::code_substituter {
public:
  void* reserve_code_near(std::uintptr_t, std::uint64_t) override { return nullptr; }
  bool commit_code(void*, const void*, std::uint64_t) override { return false; }
  bool precheck_entry(std::uintptr_t, void*) override { return false; }
  bool snapshot_entry(std::uintptr_t, std::uint8_t[5]) override { return false; }
  bool patch_entry(std::uintptr_t, void*) override { return false; }
  bool restore_entry(std::uintptr_t, const std::uint8_t[5]) override { return false; }
};

// ---------------------------------------------------------------------------
// Just enough terminal to reconstruct what a viewer sees: absolute cursor
// moves, carriage returns, line feeds, and a clear. Styles and mode toggles
// are consumed and dropped. Cells hold bytes rather than glyphs, which is
// exact for the ASCII text these tests assert on and harmless elsewhere —
// the renderer positions the cursor explicitly before every span, so column
// drift never accumulates.
//
// The parser is a resumable state machine because a read boundary can fall
// anywhere, including in the middle of an escape sequence. A parser that
// only understood sequences contained in a single feed() call left escape
// tails on screen as text and quietly produced garbage.
// ---------------------------------------------------------------------------
class terminal_screen {
public:
  terminal_screen(int cols, int rows) : cols_(cols), cells_(rows, std::string(cols, ' ')) {}

  void feed(std::string_view bytes) {
    for (const char raw : bytes) {
      char32_t cp = 0;
      if (!utf8_.feed(static_cast<unsigned char>(raw), cp)) {
        continue; // mid-sequence
      }
      handle(cp);
    }
  }

  [[nodiscard]] std::vector<std::string> rows() const { return cells_; }

  /// Every integer that follows `prefix` anywhere on screen — "line 42"
  /// yields 42. Missing when the pane shows something else.
  [[nodiscard]] std::vector<int> numbers_after(std::string_view prefix) const {
    std::vector<int> found;
    for (const auto& row : cells_) {
      for (std::size_t at = row.find(prefix); at != std::string::npos;
           at = row.find(prefix, at + 1)) {
        std::size_t p = at + prefix.size();
        int value = 0;
        bool digits = false;
        while (p < row.size() && row[p] >= '0' && row[p] <= '9') {
          value = value * 10 + (row[p] - '0');
          digits = true;
          ++p;
        }
        if (digits) {
          found.push_back(value);
        }
      }
    }
    return found;
  }

private:
  enum class state { text, escape, csi };

  void handle(char32_t cp) {
    switch (state_) {
    case state::text:
      if (cp == U'\033') {
        state_ = state::escape;
      } else if (cp == U'\r') {
        x_ = 0;
      } else if (cp == U'\n') {
        y_ = std::min(y_ + 1, static_cast<int>(cells_.size()) - 1);
      } else {
        put(cp);
      }
      break;
    case state::escape:
      // Only CSI is used by the renderer; anything else is dropped.
      if (cp == U'[') {
        params_.clear();
        state_ = state::csi;
      } else {
        state_ = state::text;
      }
      break;
    case state::csi:
      if ((cp >= U'0' && cp <= U'9') || cp == U';' || cp == U'?') {
        params_ += static_cast<char>(cp);
      } else {
        apply_csi(static_cast<char>(cp));
        state_ = state::text;
      }
      break;
    }
  }

  // Columns advance by the code point's display width, using Glyph's own
  // rule so the model and the renderer agree. Getting this wrong is fatal
  // rather than cosmetic: the renderer's coordinates are cells, so a model
  // that counted bytes — or treated a wide kana as one column — drifts and
  // applies every later diff span to the wrong cell. Non-ASCII is stored as
  // a dot; the assertions read ASCII text only.
  void put(char32_t cp) {
    const std::uint8_t width = glyph::core::cell_width(cp);
    if (y_ >= 0 && y_ < static_cast<int>(cells_.size())) {
      if (x_ >= 0 && x_ < cols_ && width > 0) {
        const char c = (cp >= 0x20 && cp < 0x7F) ? static_cast<char>(cp) : '.';
        cells_[static_cast<std::size_t>(y_)][static_cast<std::size_t>(x_)] = c;
      }
    }
    x_ += width;
  }

  void apply_csi(char final_ch) {
    if (final_ch == 'H' || final_ch == 'f') {
      move_cursor(params_);
    } else if (final_ch == 'J') {
      for (auto& row : cells_) {
        row.assign(static_cast<std::size_t>(cols_), ' ');
      }
      x_ = 0;
      y_ = 0;
    }
    // 'm' (style), 'h'/'l' (modes), everything else: not needed here.
    params_.clear();
  }

  void move_cursor(const std::string& params) {
    int row = 1;
    int col = 1;
    const std::size_t semi = params.find(';');
    try {
      if (semi == std::string::npos) {
        if (!params.empty() && params[0] != '?') {
          row = std::stoi(params);
        }
      } else {
        const std::string first = params.substr(0, semi);
        const std::string second = params.substr(semi + 1);
        if (!first.empty() && first[0] != '?') {
          row = std::stoi(first);
        }
        if (!second.empty()) {
          col = std::stoi(second);
        }
      }
    } catch (const std::exception&) {
      return;
    }
    y_ = std::max(0, std::min(row - 1, static_cast<int>(cells_.size()) - 1));
    x_ = std::max(0, std::min(col - 1, cols_ - 1));
  }

  /// Incremental UTF-8: a read boundary can split a multi-byte glyph too.
  struct utf8_decoder {
    char32_t cp = 0;
    std::uint32_t acc = 0;
    std::uint8_t need = 0;

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
        return false;
      }
      if ((b & 0xC0) != 0x80) {
        need = 0;
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

  int cols_;
  std::vector<std::string> cells_;
  int x_ = 0;
  int y_ = 0;
  state state_ = state::text;
  std::string params_;
  utf8_decoder utf8_;
};

// ---------------------------------------------------------------------------
// A monitor on a pty, driven from the parent side of the pair.
// ---------------------------------------------------------------------------
constexpr int kCols = 140;
constexpr int kRows = 40;
constexpr int kLogLines = 60;

class pty_monitor {
public:
  /// stream = true keeps the child pushing one new line every 150 ms, the
  /// way a live log behaves; the default pushes a fixed batch and stops.
  explicit pty_monitor(bool stream = false) : stream_(stream), screen_(kCols, kRows) {
    master_ = ::posix_openpt(O_RDWR | O_NOCTTY);
    REQUIRE(master_ >= 0);
    ::grantpt(master_);
    ::unlockpt(master_);
    slave_path_ = ::ptsname(master_);

    child_ = ::fork();
    REQUIRE(child_ >= 0);
    if (child_ == 0) {
      child_main();
    }

    // Size the pty only after the child has opened the slave: macOS accepts
    // TIOCSWINSZ on a master whose slave has never been opened, reports
    // success, and still reports 0x0 afterwards. The monitor reads the size
    // every frame, so a late size is picked up.
    ::usleep(300 * 1000);
    struct winsize ws {};
    ws.ws_col = kCols;
    ws.ws_row = kRows;
    ::ioctl(master_, TIOCSWINSZ, &ws);
    ::usleep(600 * 1000); // let a frame render at the real size
  }

  ~pty_monitor() {
    if (child_ > 0) {
      ::kill(child_, SIGKILL);
      int status = 0;
      ::waitpid(child_, &status, 0);
    }
    if (master_ >= 0) {
      ::close(master_);
    }
  }

  pty_monitor(const pty_monitor&) = delete;
  pty_monitor& operator=(const pty_monitor&) = delete;

  /// Read whatever the monitor drew during `window`, feeding the screen.
  ///
  /// Driven by poll(), not by a blocking read: a blocking read returns as
  /// soon as bytes arrive, so a "short" window would quietly wait out a
  /// whole render cadence and any latency assertion built on it would pass
  /// no matter how slow the monitor was.
  std::string drain(std::chrono::milliseconds window) {
    std::string out;
    const auto deadline = std::chrono::steady_clock::now() + window;
    char buf[4096];
    for (;;) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        break;
      }
      const auto left =
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
      struct pollfd pfd {};
      pfd.fd = master_;
      pfd.events = POLLIN;
      if (::poll(&pfd, 1, static_cast<int>(std::max<std::int64_t>(left, 1))) <= 0) {
        break;
      }
      const ssize_t n = ::read(master_, buf, sizeof(buf));
      if (n > 0) {
        out.append(buf, static_cast<std::size_t>(n));
        screen_.feed(std::string_view(buf, static_cast<std::size_t>(n)));
      }
    }
    return out;
  }

  [[nodiscard]] const terminal_screen& screen() const { return screen_; }

  /// Block until the monitor writes something (a frame went out), so a test
  /// can act just after a cadence tick instead of racing it. Returns the
  /// bytes read, empty on timeout.
  std::string wait_for_output(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    char buf[4096];
    for (;;) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        break;
      }
      const auto left =
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
      struct pollfd pfd {};
      pfd.fd = master_;
      pfd.events = POLLIN;
      if (::poll(&pfd, 1, static_cast<int>(std::max<std::int64_t>(left, 1))) <= 0) {
        break;
      }
      const ssize_t n = ::read(master_, buf, sizeof(buf));
      if (n > 0) {
        screen_.feed(std::string_view(buf, static_cast<std::size_t>(n)));
        return std::string(buf, static_cast<std::size_t>(n));
      }
    }
    return {};
  }

  void send(const std::string& bytes) {
    REQUIRE(::write(master_, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()));
  }

  /// One SGR wheel notch at (col, row), 1-based terminal coordinates.
  void wheel(bool up, int col, int row) {
    send("\033[<" + std::to_string(up ? 64 : 65) + ";" + std::to_string(col) + ";" +
         std::to_string(row) + "M");
  }

  [[nodiscard]] bool alive() const {
    int status = 0;
    return ::waitpid(child_, &status, WNOHANG) == 0;
  }

  /// Wait for the child to exit on its own; false on timeout. Output is
  /// drained throughout: an unread pty fills up and the child's render
  /// thread is supposed to drop frames rather than block, but a test that
  /// never reads would still be testing a situation no terminal produces.
  bool wait_exit(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    int status = 0;
    char buf[4096];
    while (std::chrono::steady_clock::now() < deadline) {
      if (::waitpid(child_, &status, WNOHANG) == child_) {
        child_ = -1; // reaped; the destructor must not kill or wait again
        return true;
      }
      while (::read(master_, buf, sizeof(buf)) > 0) {
        // discard
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
  }

private:
  void child_main() {
    ::setsid();
    const int slave = ::open(slave_path_.c_str(), O_RDWR);
    if (slave < 0) {
      ::_exit(2);
    }
    ::ioctl(slave, TIOCSCTTY, 0);
    ::dup2(slave, STDIN_FILENO);
    ::dup2(slave, STDOUT_FILENO);
    if (slave > STDERR_FILENO) {
      ::close(slave);
    }
    ::close(master_);

    auto sym = std::make_shared<null_symbols>();
    neko::backend_bundle bundle;
    bundle.loader = std::make_shared<null_loader>();
    bundle.symbols = sym;
    bundle.state = sym;
    bundle.substituter = std::make_shared<null_substituter>();

    neko::reload_session session{std::move(bundle)};
    {
      neko::tui::monitor tui{
          session,
          {.title = "test", .render_mode = neko::tui::mode::fullscreen, .show_app_log = true}};
      for (int i = 0; i < kLogLines; ++i) {
        tui.log_line("line " + std::to_string(i));
      }
      int next = kLogLines;
      for (int i = 0; i < 100 && tui.running(); ++i) {
        if (stream_) {
          tui.log_line("line " + std::to_string(next++));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
      }
    }
    ::_exit(0);
  }

  int master_ = -1;
  pid_t child_ = -1;
  bool stream_ = false;
  std::string slave_path_;
  terminal_screen screen_;
};

} // namespace

TEST_CASE("monitor: enables the mouse modes the wheel needs") {
  // A terminal only reports wheel and drag events for the modes it was
  // asked for. Glyph's own input backend asks for 1000 (press/release),
  // 1002 (button events == drag) and 1006 (SGR coordinates); a monitor
  // that omits 1002 gets neither drag-select nor, on several terminals,
  // the wheel — which is exactly how this failed once.
  pty_monitor pty;
  const std::string frames = pty.drain(std::chrono::milliseconds(400));
  CHECK(frames.find("\033[?1000h") != std::string::npos);
  CHECK(frames.find("\033[?1002h") != std::string::npos);
  CHECK(frames.find("\033[?1006h") != std::string::npos);
}

TEST_CASE("monitor: terminal mode sizes its own pty from the host terminal") {
  // The monitor's terminal mode creates a pty for itself and copies the
  // host terminal's size onto it. On macOS the copy is silently dropped
  // unless the pty's slave has been opened first, and the size is reset
  // again when the last slave closes — which left the panel rendering
  // against the 80x24 fallback (no log pane, no wheel target).
  //
  // The app's terminal here is a pty the test owns; the monitor's own pty
  // is reached through the path it reports on stderr.
  constexpr int kHostCols = 132;
  constexpr int kHostRows = 37;

  int host_master = ::posix_openpt(O_RDWR | O_NOCTTY);
  REQUIRE(host_master >= 0);
  ::grantpt(host_master);
  ::unlockpt(host_master);
  const std::string host_slave_path = ::ptsname(host_master);
  const int host_slave = ::open(host_slave_path.c_str(), O_RDWR | O_NOCTTY);
  REQUIRE(host_slave >= 0);
  struct winsize host_ws {};
  host_ws.ws_col = kHostCols;
  host_ws.ws_row = kHostRows;
  ::ioctl(host_slave, TIOCSWINSZ, &host_ws);

  const pid_t child = ::fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    ::setsid();
    ::ioctl(host_slave, TIOCSCTTY, 0);
    ::dup2(host_slave, STDIN_FILENO);
    ::dup2(host_slave, STDOUT_FILENO);
    ::dup2(host_slave, STDERR_FILENO);
    if (host_slave > STDERR_FILENO) {
      ::close(host_slave);
    }
    ::close(host_master);
    // No tmux in a test run: keep the monitor from splitting a real window
    // on a developer's machine.
    ::setenv("PATH", "/nonexistent", 1);

    auto sym = std::make_shared<null_symbols>();
    neko::backend_bundle bundle;
    bundle.loader = std::make_shared<null_loader>();
    bundle.symbols = sym;
    bundle.state = sym;
    bundle.substituter = std::make_shared<null_substituter>();
    neko::reload_session session{std::move(bundle)};
    neko::tui::monitor tui{
        session, {.title = "test", .render_mode = neko::tui::mode::terminal, .show_app_log = true}};
    for (int i = 0; i < 10; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ::_exit(0);
  }

  // Read the monitor's startup line to learn which pty it created. The
  // master must be non-blocking or this read waits forever when nothing
  // arrives (the failure mode is then a test timeout, not a test failure).
  ::fcntl(host_master, F_SETFL, ::fcntl(host_master, F_GETFL, 0) | O_NONBLOCK);
  std::string reported;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    char buf[512];
    const ssize_t n = ::read(host_master, buf, sizeof(buf));
    if (n > 0) {
      reported.append(buf, static_cast<std::size_t>(n));
    }
    if (reported.find("Nekomata TUI: /dev/") != std::string::npos) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  const std::string marker = "Nekomata TUI: ";
  const std::size_t at = reported.find(marker);
  REQUIRE(at != std::string::npos);
  const std::size_t start = at + marker.size();
  const std::size_t end = reported.find_first_of("\r\n", start);
  REQUIRE(end != std::string::npos);
  const std::string monitor_slave = reported.substr(start, end - start);

  const int peek = ::open(monitor_slave.c_str(), O_RDONLY | O_NOCTTY);
  REQUIRE(peek >= 0);
  struct winsize got {};
  const int rc = ::ioctl(peek, TIOCGWINSZ, &got);
  ::close(peek);

  ::kill(child, SIGKILL);
  int status = 0;
  ::waitpid(child, &status, 0);
  ::close(host_slave);
  ::close(host_master);

  CHECK(rc == 0);
  CHECK(static_cast<int>(got.ws_col) == kHostCols);
  CHECK(static_cast<int>(got.ws_row) == kHostRows);
}

TEST_CASE("monitor: renders at the pty size, log pane included") {
  // The log pane only exists when the terminal is wide enough, so this is
  // also the guard for the layout: a pty stuck at 0x0 renders at the 80x24
  // fallback, the pane disappears, and every mouse test below goes with it.
  pty_monitor pty;
  (void)pty.drain(std::chrono::milliseconds(400));
  CHECK(pty.screen().numbers_after("line ").size() > 0);
  const auto rows = pty.screen().rows();
  const bool has_log_title = std::any_of(rows.begin(), rows.end(), [](const std::string& row) {
    return row.find("log") != std::string::npos;
  });
  CHECK(has_log_title);
}

TEST_CASE("monitor: wheel scrolls the log pane") {
  pty_monitor pty;

  // The view follows the newest line until the user scrolls away.
  (void)pty.drain(std::chrono::milliseconds(500));
  const auto seen_before = pty.screen().numbers_after("line ");
  REQUIRE_FALSE(seen_before.empty());
  CHECK(*std::max_element(seen_before.begin(), seen_before.end()) == kLogLines - 1);

  // Ten notches up, pointer inside the right-hand log pane. Five rows per
  // notch default in ScrollRegionView: far more than the pane is tall.
  for (int i = 0; i < 10; ++i) {
    pty.wheel(/*up=*/true, kCols - 20, 12);
  }
  const std::string raw_after = pty.drain(std::chrono::milliseconds(500));
  const auto seen_after = pty.screen().numbers_after("line ");
  REQUIRE_FALSE(seen_after.empty());

  const int oldest_after = *std::min_element(seen_after.begin(), seen_after.end());
  const int oldest_before = *std::min_element(seen_before.begin(), seen_before.end());
  CHECK(oldest_after < oldest_before); // older output scrolled into view

  // ...and wheel-down comes back to the live end.
  for (int i = 0; i < 12; ++i) {
    pty.wheel(/*up=*/false, kCols - 20, 12);
  }
  (void)pty.drain(std::chrono::milliseconds(500));
  const auto seen_back = pty.screen().numbers_after("line ");
  REQUIRE_FALSE(seen_back.empty());
  CHECK(*std::max_element(seen_back.begin(), seen_back.end()) == kLogLines - 1);
}

TEST_CASE("monitor: the wheel drives the log pane from anywhere") {
  // There is exactly one scrollable region on screen, so the wheel is not
  // required to be over it. An earlier version ignored wheel events whose
  // pointer sat over the monitor pane, which is where the pointer usually
  // is — reported as "the wheel does nothing".
  pty_monitor pty;

  (void)pty.drain(std::chrono::milliseconds(500));
  const auto seen_before = pty.screen().numbers_after("line ");
  REQUIRE_FALSE(seen_before.empty());

  // Column 5 is inside the monitor panel, not the log pane.
  for (int i = 0; i < 10; ++i) {
    pty.wheel(/*up=*/true, 5, 12);
  }
  (void)pty.drain(std::chrono::milliseconds(500));
  const auto seen_after = pty.screen().numbers_after("line ");
  REQUIRE_FALSE(seen_after.empty());
  CHECK(*std::min_element(seen_after.begin(), seen_after.end()) <
        *std::min_element(seen_before.begin(), seen_before.end()));

  // And it comes back to the live end, from the same off-pane position.
  for (int i = 0; i < 12; ++i) {
    pty.wheel(/*up=*/false, 5, 12);
  }
  (void)pty.drain(std::chrono::milliseconds(500));
  const auto seen_back = pty.screen().numbers_after("line ");
  REQUIRE_FALSE(seen_back.empty());
  CHECK(*std::max_element(seen_back.begin(), seen_back.end()) == kLogLines - 1);
}

TEST_CASE("monitor: new lines do not yank a reader who scrolled up") {
  // The pane follows the live end until the reader scrolls away, and then
  // holds still: a live log feeds lines faster than anyone can read them,
  // so re-following on every push makes both the wheel and drag-selection
  // pointless. Held lines are marked in the pane title and appear the
  // moment the reader comes back down.
  pty_monitor pty{/*stream=*/true};

  (void)pty.drain(std::chrono::milliseconds(500));
  const auto seen_start = pty.screen().numbers_after("line ");
  REQUIRE_FALSE(seen_start.empty());

  for (int i = 0; i < 10; ++i) {
    pty.wheel(/*up=*/true, 90, 12);
  }

  // Let several new lines arrive while the reader sits in the past.
  (void)pty.drain(std::chrono::milliseconds(1500));
  const auto seen_held = pty.screen().numbers_after("line ");
  REQUIRE_FALSE(seen_held.empty());

  // Nothing newer than the batch that existed before the scroll may have
  // pushed its way back in.
  CHECK(*std::max_element(seen_held.begin(), seen_held.end()) < kLogLines);
  CHECK(*std::min_element(seen_held.begin(), seen_held.end()) <
        *std::min_element(seen_start.begin(), seen_start.end()));

  // Scrolling back to the bottom releases them.
  for (int i = 0; i < 40; ++i) {
    pty.wheel(/*up=*/false, 90, 12);
  }
  (void)pty.drain(std::chrono::milliseconds(800));
  const auto seen_released = pty.screen().numbers_after("line ");
  REQUIRE_FALSE(seen_released.empty());
  CHECK(*std::max_element(seen_released.begin(), seen_released.end()) >= kLogLines);
}

TEST_CASE("monitor: a drag highlights the selection right away") {
  // The highlight used to wait for the next cadence tick — up to 100 ms
  // behind the pointer, which reads as the selection being stuck to the
  // table. Input now schedules a redraw of its own.
  //
  // Reverse video (SGR "0;7") appears only for the selection: every other
  // style in the panel is a foreground colour, and none of the palette's
  // components is 7, so the attribute cannot be confused with a colour.
  pty_monitor pty;
  (void)pty.drain(std::chrono::milliseconds(400));

  // Sync to just after a cadence frame, so the next one is a full tick away
  // and only an input-driven redraw can beat the deadline below.
  REQUIRE_FALSE(pty.wait_for_output(std::chrono::milliseconds(1000)).empty());

  // Columns 72..76 (1-based) are inside the log pane's text, not the blank
  // tail of the row: a selection over trailing spaces highlights nothing.
  pty.send("\033[<0;72;14M");  // left button down on a log line
  pty.send("\033[<32;76;14M"); // drag right with the button held

  // A third of a cadence tick: waiting for the next frame lands here only
  // if the highlight is not driven by input.
  const std::string bytes = pty.drain(std::chrono::milliseconds(33));
  CHECK(bytes.find("\033[0;7") != std::string::npos);
}

TEST_CASE("monitor: 'q' closes the panel and stops the app loop") {
  pty_monitor pty;
  CHECK(pty.alive());

  pty.send("q");
  CHECK(pty.wait_exit(std::chrono::milliseconds(3000)));
}
