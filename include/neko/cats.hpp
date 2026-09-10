// cats.hpp — all Nekomata cat faces. One source of truth.
//
// Logo animation, log-level cats, and any future variants live here.
// Update faces in this file only; log.cpp and the TUI read from here.

#pragma once

namespace neko::cats {

// Logo animation: 5 frames, 11 display columns each.
//
// The width has to be identical frame to frame. The logo animates in place,
// so a frame that is one column wider drags the panel beside it sideways as
// it plays — which is what a mix of wide and narrow paws did before.
//
// The reaching paws are U+3063 (っ, East Asian Wide: two columns) and the
// formed ones are U+0E05 (ฅ, narrow: one), so collapsing a paw shortens the
// frame by a column and the space padding makes up the difference. The face
// therefore stays at column 2 in every frame while the paws tuck inward.
//
//   0: arms out                  っ(=•ω•=)っ
//   1: left paw forms              ฅ(=•ω•=)っ
//   2: both paws, eyes open        ฅ(=•ω•=)ฅ    (alert / static logo)
//   3: left eye closes             ฅ(=─ω•=)ฅ
//   4: fully content               ฅ(=─ω─=)ฅ    (idle pose)
constexpr int logo_frame_count = 5;
constexpr const char* logo_frames[logo_frame_count] = {
    "\u3063(=\u2022\u03C9\u2022=)\u3063",   " \u0E05(=\u2022\u03C9\u2022=)\u3063",
    " \u0E05(=\u2022\u03C9\u2022=)\u0E05 ", " \u0E05(=\u2500\u03C9\u2022=)\u0E05 ",
    " \u0E05(=\u2500\u03C9\u2500=)\u0E05 ",
};

/// Static logo (alert): the monitoring face, for inline use.
constexpr const char* logo = logo_frames[2];

// Log-level cats: single-width (7 columns), base structure (=XωX=).
constexpr const char* info = "(=\u2022\u03C9\u2022=)";  // (=•ω•=)
constexpr const char* ok = "(=^\u03C9^=)";              // (=^ω^=)
constexpr const char* warn = "(=\u00AC\u03C9\u00AC=)";  // (=¬ω¬=)
constexpr const char* error = "(=\u00D7\u03C9\u00D7=)"; // (=×ω×=)

} // namespace neko::cats
