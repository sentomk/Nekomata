#include "descriptor_discovery.hpp"

#include <span>

#include "descriptor_section.hpp"

// The linker synthesizes __start_<name>/__stop_<name> for input sections
// whose names are valid C identifiers, so `neko_groups` yields exactly these
// two symbols. Both are weak: without an embedded section they resolve to
// null and discovery simply reports no groups. The weak-undefined-data trick
// is an ELF linking property; other object formats report no section.
#if defined(__ELF__)
extern "C" const std::uint8_t __start_neko_groups[] __attribute__((weak));
extern "C" const std::uint8_t __stop_neko_groups[] __attribute__((weak));
#endif

namespace neko::detail {

bool embedded_descriptor_section(const std::uint8_t*& begin, const std::uint8_t*& end) {
#if defined(__ELF__)
  begin = __start_neko_groups;
  end = __stop_neko_groups;
  return begin != nullptr && end != nullptr && begin < end;
#else
  begin = nullptr;
  end = nullptr;
  return false;
#endif
}

std::vector<group_descriptor> discover_embedded_descriptors() {
  const std::uint8_t* begin = nullptr;
  const std::uint8_t* end = nullptr;
  if (!embedded_descriptor_section(begin, end)) {
    return {};
  }
  return resolve_descriptor_records(
      parse_descriptor_section(std::span<const std::uint8_t>{begin, end}, "neko_groups"));
}

} // namespace neko::detail
