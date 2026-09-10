// terminal — the platform seam for neko::tui.
//
// Everything the monitor needs from the host terminal and nothing else:
// write frames, read input bytes, report the size, and hand the terminal
// back the way it was found. The monitor itself does no platform work — it
// renders into a Frame and decodes bytes through Glyph, both of which are
// already portable — so a new platform is one file, not a branch in the
// rendering loop.
//
// Implementations live next to this header and are selected at build time:
//   terminal_posix.cpp  a pty, or the current tty through termios
//   terminal_win32.cpp  a ConPTY, or the console through its VT modes
//
// Input is delivered as raw bytes on purpose: escape sequences, SGR mouse
// reports and UTF-8 are decoded once, by Glyph's VtDecoder, for every
// platform.

#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

#include "glyph/core/geometry.h"

namespace neko::tui::detail {

/// Where the panel is drawn.
enum class placement {
  current, ///< take over the terminal nekomata was started from
  pty,     ///< create a separate terminal and print how to attach to it
};

class terminal {
public:
  virtual ~terminal() = default;

  terminal(const terminal&) = delete;
  terminal& operator=(const terminal&) = delete;

  /// Open a terminal. Throws std::runtime_error when one cannot be had.
  static std::unique_ptr<terminal> open(placement where);

  /// Write raw bytes to the display. Best effort by contract: it must never
  /// block, because the far end stopping (a detached screen, a stopped
  /// reader) must not wedge the render thread. Dropping the tail of a frame
  /// is safe — the next one repaints from scratch.
  virtual void write(std::string_view bytes) = 0;

  /// Bytes that arrived within `wait`, empty when none did. Waits for the
  /// first byte only, then takes everything already queued: a single escape
  /// sequence can arrive in pieces, and the decoder wants them together.
  /// A short wait keeps the caller's loop responsive without spinning.
  virtual std::string read(std::chrono::milliseconds wait) = 0;

  /// Size in cells. Falls back to the conventional 80x24 when the host has
  /// no opinion (redirected output, a pty nobody has sized).
  virtual glyph::core::Size size() = 0;

  /// True once input has ended (EOF, detached terminal).
  [[nodiscard]] virtual bool input_closed() const = 0;

protected:
  terminal() = default;
};

} // namespace neko::tui::detail
