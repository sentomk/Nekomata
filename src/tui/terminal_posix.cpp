// terminal_posix — the POSIX side of neko::tui's platform seam.
//
// Two placements, both built on termios:
//
//   placement::current  the terminal nekomata was started from. Raw stdin,
//                       frames on stdout, nothing forked.
//   placement::pty      a fresh pty. The panel lives there and the user
//                       attaches with screen/tmux; the application's own
//                       terminal is untouched.
//
// The quirks documented below are the expensive part of this file. They were
// all found the hard way and are worth reading before changing anything.

#include "terminal.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace neko::tui::detail {
namespace {

/// Switch a tty to raw mode, reporting whether it took.
bool make_raw(int fd, struct termios& saved) {
  if (::tcgetattr(fd, &saved) != 0) {
    return false;
  }
  struct termios raw = saved;
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST | ONLCR);
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;
  return ::tcsetattr(fd, TCSANOW, &raw) == 0;
}

class posix_terminal final : public terminal {
public:
  ~posix_terminal() override {
    if (raw_active_ && raw_fd_ >= 0) {
      ::tcsetattr(raw_fd_, TCSANOW, &saved_);
    }
    if (slave_fd_ >= 0) {
      ::close(slave_fd_);
    }
    if (master_fd_ >= 0) {
      ::close(master_fd_);
    }
  }

  void take_current_terminal() {
    raw_fd_ = STDIN_FILENO;
    raw_active_ = make_raw(raw_fd_, saved_);
    in_fd_ = STDIN_FILENO;
    out_fd_ = STDOUT_FILENO;
  }

  void make_pty() {
    master_fd_ = ::posix_openpt(O_RDWR | O_NOCTTY);
    if (master_fd_ < 0) {
      throw std::runtime_error("neko::tui: cannot create pty");
    }
    ::grantpt(master_fd_);
    ::unlockpt(master_fd_);
    slave_path_ = ::ptsname(master_fd_);

    // The master is ours alone, so it can stay non-blocking: writes drop
    // frames instead of blocking when the attached terminal stops draining.
    const int flags = ::fcntl(master_fd_, F_GETFL, 0);
    if (flags >= 0) {
      ::fcntl(master_fd_, F_SETFL, flags | O_NONBLOCK);
    }

    // Hold the slave open for the pty's whole life. macOS resets the window
    // size when the last slave fd closes, which would undo the size sync
    // below the moment this function returns; a slave holder also keeps the
    // device alive between attachments.
    //
    // Raw mode has to go on the *slave*, not the master: Linux accepts
    // termios on either end, but macOS rejects the master with ENOTTY, and
    // the settings then silently never apply — ECHO reflects our own frames
    // back and ICANON holds them until a newline a frame may never contain.
    slave_fd_ = ::open(slave_path_.c_str(), O_RDWR | O_NOCTTY);
    if (slave_fd_ >= 0) {
      // Size the pty from the host terminal. screen/tmux do not propagate
      // sizes to foreign ptys (they are not the session leader), so without
      // this the pty stays at the kernel default 0x0 and the panel renders
      // against the 80x24 fallback.
      //
      // This must happen *after* the slave has been opened: macOS accepts
      // TIOCSWINSZ on a master whose slave has never been opened, returns
      // success, and still reports 0x0 afterwards. Linux is happy either way.
      struct winsize host_ws {};
      if (::ioctl(STDERR_FILENO, TIOCGWINSZ, &host_ws) == 0 && host_ws.ws_col > 0) {
        ::ioctl(slave_fd_, TIOCSWINSZ, &host_ws);
      }
      make_raw(slave_fd_, saved_); // saved_ is unused: the pty is not restored
    }

    // CRLF, not a bare LF: the host terminal may be in a mode where LF alone
    // does not return the carriage (a screen session that did not clean up
    // leaves -onlcr behind), and this hint has to stay readable either way.
    std::fprintf(stderr, "Nekomata TUI: %s\r\n", slave_path_.c_str());
    std::fprintf(stderr, "  attach:  screen %s   (or: tmux split-window 'screen %s')\r\n",
                 slave_path_.c_str(), slave_path_.c_str());

    in_fd_ = master_fd_;
    out_fd_ = master_fd_;
  }

  void write(std::string_view bytes) override {
    if (out_fd_ < 0) {
      return;
    }
    const char* p = bytes.data();
    std::size_t left = bytes.size();
    while (left > 0) {
      const ssize_t n = ::write(out_fd_, p, left);
      if (n > 0) {
        p += n;
        left -= static_cast<std::size_t>(n);
        continue;
      }
      if (n < 0 && errno == EINTR) {
        continue;
      }
      return; // EAGAIN or a dead end: drop the rest of this frame
    }
  }

  std::string read(std::chrono::milliseconds wait) override {
    std::string out;
    if (closed_ || in_fd_ < 0) {
      return out;
    }

    // The fd is shared with the application in placement::current, so the
    // non-blocking flag goes on for the duration of this call and comes off
    // again. A pty master is already non-blocking and the toggle is a no-op.
    const int flags = ::fcntl(in_fd_, F_GETFL, 0);
    if (flags >= 0) {
      ::fcntl(in_fd_, F_SETFL, flags | O_NONBLOCK);
    }

    struct pollfd pfd {};
    pfd.fd = in_fd_;
    pfd.events = POLLIN;
    if (::poll(&pfd, 1, static_cast<int>(wait.count())) > 0) {
      for (;;) {
        char buf[256];
        const ssize_t n = ::read(in_fd_, buf, sizeof(buf));
        if (n > 0) {
          out.append(buf, static_cast<std::size_t>(n));
          continue;
        }
        if (n == 0) {
          closed_ = true; // EOF: redirected, detached, or the slave is gone
        }
        break;
      }
    }

    if (flags >= 0) {
      ::fcntl(in_fd_, F_SETFL, flags);
    }
    return out;
  }

  glyph::core::Size size() override {
    struct winsize ws {};
    if (::ioctl(out_fd_, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
      return glyph::core::Size{static_cast<glyph::core::coord_t>(ws.ws_col),
                               static_cast<glyph::core::coord_t>(ws.ws_row)};
    }
    // out_fd_ has no size (redirected, or a pty nobody sized): the host
    // terminal is the next best thing, and it is what the panel is meant to
    // match anyway.
    if (::ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
      return glyph::core::Size{static_cast<glyph::core::coord_t>(ws.ws_col),
                               static_cast<glyph::core::coord_t>(ws.ws_row)};
    }
    return glyph::core::Size{80, 24};
  }

  [[nodiscard]] bool input_closed() const override { return closed_; }

private:
  int master_fd_ = -1;
  int slave_fd_ = -1;
  int in_fd_ = -1;
  int out_fd_ = -1;
  int raw_fd_ = -1;
  bool raw_active_ = false;
  bool closed_ = false;
  struct termios saved_ {};
  std::string slave_path_;
};

} // namespace

std::unique_ptr<terminal> terminal::open(placement where) {
  auto t = std::make_unique<posix_terminal>();
  if (where == placement::current) {
    t->take_current_terminal();
  } else {
    t->make_pty();
  }
  return t;
}

} // namespace neko::tui::detail
