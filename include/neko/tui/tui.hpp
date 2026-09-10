// tui — terminal UI for a reload session.
//
// A monitor renders the state of a reload session so the developer can
// see what nekomata is doing: what's watched, how many reloads landed,
// what the last attempt did. Attach one when you want visibility; skip
// it when logs are enough.
//
//     neko::reload_session session{neko::elf::create_backend()};
//     neko::tui::monitor tui{session};
//
//     for (;;) {
//         do_work();
//         session.update();
//     }

#pragma once

#include <string>

#include <neko/session.hpp>

namespace neko::tui {

/// Rendering placement.
enum class mode {
  inline_status, ///< styled line(s) in the current terminal (default)
  fullscreen,    ///< take over the terminal nekomata was started from
  terminal,      ///< separate terminal window via pty (not supported on
                 ///< Windows yet: the monitor constructor throws)
};

struct monitor_config {
  std::string title = "Nekomata";
  mode render_mode = mode::inline_status;
  /// Show an application log pane alongside the monitor (terminal mode
  /// only; ignored by inline_status). When false, only the monitor panel.
  bool show_app_log = false;
};

class monitor {
public:
  explicit monitor(reload_session& session, monitor_config config = {});
  ~monitor();

  /// Refresh the display with current session state.
  void render();

  /// Append one line to the application log pane (fullscreen/terminal
  /// modes only; ignored by inline_status). Thread-safe.
  void log_line(std::string text);

  /// False once the user asked the panel to close (`q`, or Esc with no
  /// selection in fullscreen/terminal modes). Always true in
  /// inline_status mode, which has no input of its own.
  [[nodiscard]] bool running() const;

private:
  reload_session& session_;
  monitor_config config_;

  class terminal_monitor;
  std::unique_ptr<terminal_monitor> terminal_;
};

} // namespace neko::tui
