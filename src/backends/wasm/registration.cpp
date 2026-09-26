#include "registration.hpp"

#include <neko/wasm.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

namespace neko::wasm::detail {
namespace {
group_record* first_record = nullptr;
plt_slot_record* first_plt_slot = nullptr;

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

void register_plt_slot(plt_slot_record& record) noexcept {
  for (auto* current = first_plt_slot; current; current = current->next) {
    if (current == &record) {
      return;
    }
  }
  record.next = first_plt_slot;
  first_plt_slot = &record;
}

void apply_plt_slots(std::string_view group, const prepared_module& module) noexcept {
  for (auto* record = first_plt_slot; record; record = record->next) {
    if (record->group != group) {
      continue;
    }
    if (const module_function entry = module.entry(record->entry); entry != nullptr) {
      record->assign(record->slot, entry);
    }
  }
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

void validate_plt_slots(const std::vector<group_registration>& groups,
                        const plt_slot_record* first) {
  for (auto* record = first; record; record = record->next) {
    const auto group = std::find_if(groups.begin(), groups.end(), [&](const auto& value) {
      return value.group_id == record->group;
    });
    if (group == groups.end()) {
      throw std::invalid_argument("wasm registration: PLT slot group '" +
                                  std::string{record->group} + "' is not registered");
    }
    const auto& entries = group->expected_contract.entries;
    if (std::find(entries.begin(), entries.end(), record->entry) == entries.end()) {
      throw std::invalid_argument("wasm registration: PLT slot entry '" +
                                  std::string{record->entry} + "' is not in group '" +
                                  group->group_id + "'");
    }
  }
}

void validate_plt_slots(const std::vector<group_registration>& groups) {
  validate_plt_slots(groups, first_plt_slot);
}

std::vector<group_registration> discover_groups() {
  return read_group_records(first_record);
}
} // namespace neko::wasm::detail
