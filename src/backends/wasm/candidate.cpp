#include "candidate.hpp"

#include <algorithm>
#include <utility>

namespace neko::wasm {

struct candidate::state {
  candidate_status status = candidate_status::loading;
  candidate_error error = candidate_error::none;
  std::string message;
  module_contract contract;
  std::shared_ptr<prepared_module> prepared;

  void reject(candidate_error reason, std::string text) {
    error = reason;
    message = std::move(text);
    status = candidate_status::rejected;
  }
};

module_function prepared_module::entry(std::string_view name) const noexcept {
  for (const auto& item : entries_) {
    if (item.name == name) {
      return item.address;
    }
  }
  return nullptr;
}

candidate::candidate(module_loader& loader, std::string path, module_contract contract)
    : state_(std::make_shared<state>()) {
  state_->contract = std::move(contract);
  const auto& expected = state_->contract;
  if (path.empty() || path.find('\0') != std::string::npos || expected.abi_id.empty() ||
      expected.abi_id.find('\0') != std::string::npos || expected.entries.empty()) {
    state_->reject(candidate_error::invalid_contract, "invalid wasm module contract");
    return;
  }
  for (auto it = expected.entries.begin(); it != expected.entries.end(); ++it) {
    if (it->empty() || it->find('\0') != std::string::npos ||
        std::find(expected.entries.begin(), it, *it) != it) {
      state_->reject(candidate_error::invalid_contract, "invalid wasm entry membership");
      return;
    }
  }

  loader.open(std::move(path), [weak = std::weak_ptr<state>{state_}](module_load_result result) {
    const auto pending = weak.lock();
    if (!pending || pending->status != candidate_status::loading) {
      return;
    }
    if (!result.image) {
      pending->reject(candidate_error::load_failed, result.message.empty()
                                                        ? "wasm module load failed"
                                                        : std::move(result.message));
      return;
    }
    const auto* header = result.image->descriptor();
    if (header == nullptr) {
      pending->reject(candidate_error::missing_descriptor, "missing wasm module descriptor");
      return;
    }
    if (header->version != module_interface_version || header->size != sizeof(module_descriptor)) {
      pending->reject(candidate_error::incompatible, "incompatible wasm descriptor layout");
      return;
    }
    const auto& descriptor = *reinterpret_cast<const module_descriptor*>(header);
    if (descriptor.abi_id == nullptr || pending->contract.abi_id != descriptor.abi_id) {
      pending->reject(candidate_error::incompatible, "wasm module ABI mismatch");
      return;
    }
    if (descriptor.entry_count != pending->contract.entries.size() ||
        descriptor.entries == nullptr) {
      pending->reject(candidate_error::invalid_descriptor, "wasm entry membership mismatch");
      return;
    }
    for (std::size_t index = 0; index < descriptor.entry_count; ++index) {
      const auto& entry = descriptor.entries[index];
      if (entry.name == nullptr || entry.address == nullptr ||
          pending->contract.entries[index] != entry.name) {
        pending->reject(candidate_error::invalid_descriptor, "invalid wasm entry descriptor");
        return;
      }
    }
    // Own metadata before making the candidate ready. Activation then needs
    // no allocation, validation or calls into the candidate's behavior.
    auto prepared = std::make_shared<prepared_module>();
    prepared->entries_.reserve(descriptor.entry_count);
    for (std::size_t index = 0; index < descriptor.entry_count; ++index) {
      prepared->entries_.push_back(
          {descriptor.entries[index].name, descriptor.entries[index].address});
    }
    prepared->image_ = std::move(result.image);
    pending->prepared = std::move(prepared);
    pending->status = candidate_status::ready;
  });
}

candidate::~candidate() = default;
candidate::candidate(candidate&&) noexcept = default;
candidate& candidate::operator=(candidate&&) noexcept = default;

candidate_status candidate::status() const noexcept {
  return state_ ? state_->status : candidate_status::cancelled;
}

candidate_error candidate::error() const noexcept {
  return state_ ? state_->error : candidate_error::none;
}

std::string_view candidate::message() const noexcept {
  return state_ ? std::string_view{state_->message} : std::string_view{};
}

void candidate::cancel() noexcept {
  if (state_ &&
      (state_->status == candidate_status::loading || state_->status == candidate_status::ready)) {
    state_->prepared.reset();
    state_->status = candidate_status::cancelled;
  }
}

bool active_module::activate(candidate& prepared) noexcept {
  if (prepared.status() != candidate_status::ready) {
    return false;
  }
  prepared.state_->prepared->image_->keep_resident();
  current_ = std::move(prepared.state_->prepared);
  prepared.state_->status = candidate_status::activated;
  return true;
}

std::shared_ptr<const prepared_module> active_module::current() const noexcept {
  return current_;
}

} // namespace neko::wasm
