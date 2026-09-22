#include "registration.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace neko::wasm::detail {
namespace {
group_record* first_record = nullptr;

bool valid_text(std::string_view value) {
  return !value.empty() && value.find('\0') == std::string_view::npos;
}
} // namespace

void register_group(group_record& record) noexcept {
  for (auto* current = first_record; current; current = current->next) {
    if (current == &record) {
      return;
    }
  }
  record.next = first_record;
  first_record = &record;
}

std::vector<group_registration> read_group_records(const group_record* first) {
  std::vector<group_registration> result;
  for (auto* record = first; record; record = record->next) {
    if (!valid_text(record->group_id) || !valid_text(record->manifest_url) ||
        !valid_text(record->abi_id) || record->entries.empty()) {
      throw std::invalid_argument(
          "wasm registration: nonempty ID, URL, ABI and entries are required");
    }
    module_contract contract{std::string{record->abi_id}, {}, {}};
    for (auto name : record->entries) {
      if (!valid_text(name) || std::find(contract.entries.begin(), contract.entries.end(), name) !=
                                   contract.entries.end()) {
        throw std::invalid_argument("wasm registration: entry names must be nonempty and unique");
      }
      contract.entries.emplace_back(name);
    }
    result.push_back({std::string{record->group_id}, std::string{record->manifest_url},
                      &log_offer_event, std::move(contract)});
  }
  std::sort(result.begin(), result.end(),
            [](const auto& a, const auto& b) { return a.group_id < b.group_id; });
  for (std::size_t i = 1; i < result.size(); ++i) {
    if (result[i - 1].group_id == result[i].group_id) {
      throw std::invalid_argument("wasm registration: duplicate reload group '" +
                                  result[i].group_id + "'");
    }
  }
  return result;
}

std::vector<group_registration> discover_groups() {
  return read_group_records(first_record);
}
} // namespace neko::wasm::detail
