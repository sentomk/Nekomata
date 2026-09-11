// symbol_manifest — the offline answer to "which source file is this from?".
//
// `nekomata manifest <binary>` writes one line per matched symbol: a link-time
// address, the symbol, and the source file it was compiled from. That is the
// one fact the runtime cannot work out for itself: telling two same-named
// static functions apart needs to know which translation unit each came from,
// and the library refuses to link a DWARF reader to find out (every user of a
// hot-reload library would then carry libdwarf into their process).
//
// Discovery is opportunistic: the file is looked for next to the running
// executable, or wherever NEKOMATA_MANIFEST points.
//
// The identity of a translation unit comes from the build integration through
// reload_session::watch(object, source). A manifest entry then answers the
// deterministic question "where was this source's symbol linked?". Inferring
// identity from a fresh object's symbol set is unsafe because ordinary edits
// change that set.

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neko::elf {

struct manifest_entry {
  std::uint64_t address = 0; // link-time virtual address
  std::string symbol;
  std::string source;
};

class symbol_manifest {
public:
  /// Parse the text form. Throws std::runtime_error on malformed input: a
  /// manifest that is half read would disambiguate *wrongly*, which is worse
  /// than not disambiguating at all.
  static symbol_manifest parse(std::string_view text);

  /// Read a manifest from `path`. An empty path, or a file that is not there,
  /// yields an empty manifest — discovery is opportunistic, never fatal.
  static symbol_manifest load(const std::filesystem::path& path);

  /// The manifest a session should use: NEKOMATA_MANIFEST when it is set (an
  /// empty value disables the search entirely), otherwise `<exe>.nekomata-map`
  /// beside the running executable.
  static symbol_manifest discover(const std::filesystem::path& executable);

  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
  [[nodiscard]] bool contains_source(std::string_view source_path) const;

  /// The link-time address of `symbol` attributed to exactly `source_path`, or
  /// nullopt when there is no unique match. A wrong answer here redirects the
  /// wrong function, so the caller must treat nullopt as "refuse".
  [[nodiscard]] std::optional<std::uint64_t> address_in_source(std::string_view symbol,
                                                               std::string_view source_path) const;

private:
  std::vector<manifest_entry> entries_;
};

} // namespace neko::elf
