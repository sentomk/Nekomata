// symbol_manifest — the offline answer to "which source file is this from?".
//
// `nekomata manifest <binary>` writes one line per matched symbol: a link-time
// address, the symbol, and the source file it was compiled from. That is the
// one fact the runtime cannot work out for itself: telling two same-named
// static functions apart needs to know which translation unit each came from,
// and the library refuses to link a DWARF reader to find out (every user of a
// hot-reload library would then carry libdwarf into their process).
//
// Discovery is opportunistic and needs no API: the file is looked for next to
// the running executable, or wherever NEKOMATA_MANIFEST points. No manifest
// means the previous behaviour, not a broken one.
//
// The identity of a translation unit comes from its *symbol set*: a manifest
// entry says which source defined which symbols, and a fresh object says which
// symbols it defines. Two translation units that share one name almost never
// share all of them, so the overlap is what picks the right one.

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

  /// The link-time address of `symbol` inside the translation unit that best
  /// matches `unit_symbols`, or nullopt when the evidence is not conclusive:
  /// no manifest, no entry for the symbol, or two translation units matching
  /// equally well. A wrong answer here redirects the wrong function, so the
  /// caller must treat nullopt as "fall back to refusing".
  [[nodiscard]] std::optional<std::uint64_t>
  address_in_unit(std::string_view symbol, const std::vector<std::string>& unit_symbols) const;

private:
  std::vector<manifest_entry> entries_;
};

} // namespace neko::elf
