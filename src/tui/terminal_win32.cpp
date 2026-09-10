// terminal_win32 — the Windows side of neko::tui's platform seam.
//
//   placement::current  the console nekomata was started from: VT output
//                       (ENABLE_VIRTUAL_TERMINAL_PROCESSING), VT input
//                       (ENABLE_VIRTUAL_TERMINAL_INPUT), UTF-8 code pages.
//   placement::pty      not supported yet. The equivalent — ConPTY via
//                       CreatePseudoConsole plus spawning wt/conhost to
//                       attach — is its own piece of work.
//
// The quirks documented below are the expensive part of this file:
//
//   * Console handles are waitable, so read() parks on
//     WaitForSingleObject exactly the way the POSIX side parks on poll().
//     Redirected stdin (a pipe, a file) is NOT waitable; it is served
//     through PeekNamedPipe/ReadFile instead, so a piped session still
//     ends with a clean EOF rather than a wedge.
//   * Setting the console mode is absolute, not additive: the input mode
//     below replaces ENABLE_LINE_INPUT/ENABLE_ECHO_INPUT/ENABLE_QUICK_EDIT
//     in one call, which is the raw-mode equivalent of the POSIX termios
//     dance. Everything changed here is restored on the way out.
//   * VT output wants more than ENABLE_VIRTUAL_TERMINAL_PROCESSING: without
//     DISABLE_NEWLINE_AUTO_RETURN the console returns the carriage on every
//     LF, which double-advances full-frame writes. Older conhost versions
//     reject that flag, so it is attempted first and dropped on failure.

#include "terminal.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace neko::tui::detail {
namespace {

class win32_terminal final : public terminal {
public:
  ~win32_terminal() override {
    // Restore in reverse order of takeover, and only what took.
    if (in_mode_saved_ && in_ != nullptr && in_ != INVALID_HANDLE_VALUE) {
      ::SetConsoleMode(in_, in_mode_);
      ::SetConsoleCP(in_cp_);
    }
    if (out_mode_saved_ && out_ != nullptr && out_ != INVALID_HANDLE_VALUE) {
      ::SetConsoleMode(out_, out_mode_);
      ::SetConsoleOutputCP(out_cp_);
    }
    // Standard handles are borrowed, not owned: nothing to close.
  }

  void take_current_terminal() {
    in_ = ::GetStdHandle(STD_INPUT_HANDLE);
    out_ = ::GetStdHandle(STD_OUTPUT_HANDLE);

    DWORD mode = 0;
    if (out_ != nullptr && out_ != INVALID_HANDLE_VALUE && ::GetConsoleMode(out_, &mode)) {
      out_mode_saved_ = true;
      out_mode_ = mode;
      out_cp_ = ::GetConsoleOutputCP();
      ::SetConsoleOutputCP(CP_UTF8);
      const DWORD wanted = mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING |
                           DISABLE_NEWLINE_AUTO_RETURN;
      if (!::SetConsoleMode(out_, wanted)) {
        // Older conhost: without DISABLE_NEWLINE_AUTO_RETURN, LFs return
        // the carriage. Full-frame writes still land, just noisier.
        ::SetConsoleMode(out_, mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
      }
    }

    in_is_console_ = in_ != nullptr && in_ != INVALID_HANDLE_VALUE && ::GetConsoleMode(in_, &mode);
    if (in_is_console_) {
      in_mode_saved_ = true;
      in_mode_ = mode;
      in_cp_ = ::GetConsoleCP();
      ::SetConsoleCP(CP_UTF8);
      // Absolute, not additive: LINE_INPUT, ECHO_INPUT, QUICK_EDIT and
      // PROCESSED_INPUT all go away. VT input delivers keys as escape
      // sequences — UTF-8 bytes for text, SGR for the mouse — which is
      // exactly what Glyph's VtDecoder already eats on POSIX.
      ::SetConsoleMode(in_, ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_MOUSE_INPUT);
    }
  }

  bool write(std::string_view bytes) override {
    if (out_ == nullptr || out_ == INVALID_HANDLE_VALUE) {
      return false;
    }
    // placement::current shares the application's stdout, which is left
    // blocking — same regime as the POSIX side: a terminal that accepts
    // nothing is a terminal nobody is looking at. Console writes are
    // synchronous and effectively never block; a redirected pipe with a
    // stopped reader can, and that is accepted here as there.
    const char* p = bytes.data();
    std::size_t left = bytes.size();
    while (left > 0) {
      const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(left, 1u << 20));
      DWORD n = 0;
      if (!::WriteFile(out_, p, chunk, &n, nullptr) || n == 0) {
        return false;
      }
      p += n;
      left -= n;
    }
    return true;
  }

  std::string read(std::chrono::milliseconds wait) override {
    std::string out;
    if (closed_ || in_ == nullptr || in_ == INVALID_HANDLE_VALUE) {
      return out;
    }
    if (in_is_console_) {
      return read_console(wait);
    }
    return read_redirected(wait);
  }

  glyph::core::Size size() override {
    // The panel is meant to match the host terminal; when stdout is
    // redirected (no buffer info), stderr is the next best thing — the
    // same fallback order as the POSIX side.
    const glyph::core::Size from_out = console_size(out_);
    if (from_out.w > 0 && from_out.h > 0) {
      return from_out;
    }
    const glyph::core::Size from_err = console_size(::GetStdHandle(STD_ERROR_HANDLE));
    if (from_err.w > 0 && from_err.h > 0) {
      return from_err;
    }
    return glyph::core::Size{80, 24};
  }

  [[nodiscard]] bool input_closed() const override { return closed_; }

private:
  static glyph::core::Size console_size(HANDLE h) {
    if (h == nullptr || h == INVALID_HANDLE_VALUE) {
      return glyph::core::Size{0, 0};
    }
    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    if (!::GetConsoleScreenBufferInfo(h, &csbi)) {
      return glyph::core::Size{0, 0};
    }
    const int w = csbi.srWindow.Right - csbi.srWindow.Left + 1;
    const int ht = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    if (w <= 0 || ht <= 0) {
      return glyph::core::Size{0, 0};
    }
    return glyph::core::Size{static_cast<glyph::core::coord_t>(w),
                             static_cast<glyph::core::coord_t>(ht)};
  }

  std::string read_console(std::chrono::milliseconds wait) {
    std::string out;
    if (::WaitForSingleObject(in_, static_cast<DWORD>(wait.count())) != WAIT_OBJECT_0) {
      return out; // timeout, or an abandoned handle: no bytes either way
    }
    for (;;) {
      char buf[256];
      DWORD n = 0;
      if (!::ReadFile(in_, buf, sizeof(buf), &n, nullptr)) {
        break; // a dying console is not EOF; just stop
      }
      if (n == 0) {
        closed_ = true; // EOF
        break;
      }
      out.append(buf, n);
      // Take everything already queued: a single escape sequence can
      // arrive as several input records, and the decoder wants them
      // together.
      if (::WaitForSingleObject(in_, 0) != WAIT_OBJECT_0) {
        break;
      }
    }
    return out;
  }

  std::string read_redirected(std::chrono::milliseconds wait) {
    // Pipes are not waitable. Poll PeekNamedPipe in slices until the
    // budget runs out; a disk file answers ReadFile immediately, so the
    // first Peek failure falls through to one plain read.
    std::string out;
    const auto deadline = std::chrono::steady_clock::now() + wait;
    for (;;) {
      DWORD avail = 0;
      if (!::PeekNamedPipe(in_, nullptr, 0, nullptr, &avail, nullptr)) {
        char buf[256];
        DWORD n = 0;
        if (!::ReadFile(in_, buf, sizeof(buf), &n, nullptr) || n == 0) {
          closed_ = true; // broken pipe, or a file at EOF
        } else {
          out.append(buf, n);
        }
        return out;
      }
      if (avail > 0) {
        char buf[256];
        DWORD n = 0;
        const DWORD want = static_cast<DWORD>(std::min<DWORD>(avail, sizeof(buf)));
        if (::ReadFile(in_, buf, want, &n, nullptr) && n > 0) {
          out.append(buf, n);
        }
        return out;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        return out;
      }
      ::Sleep(10);
    }
  }

  HANDLE in_ = nullptr;
  HANDLE out_ = nullptr;
  bool in_is_console_ = false;
  bool closed_ = false;
  bool in_mode_saved_ = false;
  bool out_mode_saved_ = false;
  DWORD in_mode_ = 0;
  DWORD out_mode_ = 0;
  UINT in_cp_ = 0;
  UINT out_cp_ = 0;
};

} // namespace

std::unique_ptr<terminal> terminal::open(placement where) {
  if (where == placement::pty) {
    throw std::runtime_error("neko::tui: a separate terminal is not supported on Windows yet "
                             "(the ConPTY backend is planned)");
  }
  auto t = std::make_unique<win32_terminal>();
  t->take_current_terminal();
  return t;
}

} // namespace neko::tui::detail
