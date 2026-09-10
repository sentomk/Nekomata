// Layout invariants for the TUI, independent of any terminal.
//
// The logo animates in place, so every frame has to occupy the same number
// of columns. A frame that is one column wider drags the rest of the row
// sideways as it plays, and it is easy to introduce by accident: the glyphs
// that read as "one character" in a source file are not all one column.
// U+3063 (っ) is East Asian Wide — two columns — while U+0E05 (ฅ) is narrow,
// so the original reaching-paw frames were a column and then two columns
// wider than the resting frame.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "glyph/core/text.h"

#include <neko/cats.hpp>

#include <cstddef>

namespace {

/// Columns a reader will see, by the same rule the renderer lays text out
/// with: per-code-point widths, using Glyph's table.
std::size_t display_width(const char* utf8) {
  std::size_t columns = 0;
  std::size_t pos = 0;
  const std::string_view text{utf8};
  while (pos < text.size()) {
    const glyph::core::Grapheme g = glyph::core::next_grapheme(text, pos);
    pos = g.next;
    columns += g.width;
  }
  return columns;
}

} // namespace

TEST_CASE("logo frames all occupy the same number of columns") {
  REQUIRE(neko::cats::logo_frame_count > 0);
  const std::size_t first = display_width(neko::cats::logo_frames[0]);
  for (int i = 0; i < neko::cats::logo_frame_count; ++i) {
    CAPTURE(i);
    CHECK(display_width(neko::cats::logo_frames[i]) == first);
  }
}

TEST_CASE("the face stays put across the animation") {
  // The paws tuck inward as they form; the cat itself must not move. A frame
  // that shifted the face would read as the whole panel twitching, which is
  // the thing the fixed-width frames are there to prevent.
  //
  // Measured in columns, not bytes: the reaching and formed paws are
  // different glyphs (three UTF-8 bytes vs one) that occupy the same column,
  // and a byte comparison calls that a mismatch.
  const auto face_column = [](const char* frame) -> std::size_t {
    const std::string_view text{frame};
    const std::size_t at = text.find('(');
    if (at == std::string_view::npos) {
      return static_cast<std::size_t>(-1);
    }
    return display_width(std::string(text.substr(0, at)).c_str());
  };

  const std::size_t first = face_column(neko::cats::logo_frames[0]);
  REQUIRE(first != static_cast<std::size_t>(-1));
  for (int i = 0; i < neko::cats::logo_frame_count; ++i) {
    CAPTURE(i);
    CHECK(face_column(neko::cats::logo_frames[i]) == first);
  }
}

TEST_CASE("log-level cats are all the same width") {
  // They start every log line, so a wider level would shift that line's
  // message relative to the others.
  const std::size_t expected = display_width(neko::cats::info);
  CHECK(display_width(neko::cats::ok) == expected);
  CHECK(display_width(neko::cats::warn) == expected);
  CHECK(display_width(neko::cats::error) == expected);
}
