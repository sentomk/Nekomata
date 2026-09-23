#pragma once

#include <neko/detail/wasm_module_descriptor.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace neko::wasm {

// Owns one loader reference. Destruction releases uncommitted references;
// keep_resident makes code survive all C++ handles until the page closes.
class module_image {
public:
  virtual ~module_image() = default;
  [[nodiscard]] virtual const module_header* descriptor() const noexcept = 0;
  virtual void keep_resident() noexcept = 0;
};

// Outcome classification supplied by the loader for an imageless result.
enum class module_load_status : std::uint8_t { loaded, load_failed, digest_mismatch };

struct module_load_result {
  std::unique_ptr<module_image> image;
  std::string message;
  // Digest mismatches classify as `integrity` failures; every other
  // imageless result is an ordinary load failure.
  module_load_status status = module_load_status::loaded;
};

class module_loader {
public:
  using completion = std::function<void(module_load_result)>;
  virtual ~module_loader() = default;
  // Own the path, expected SHA-256, and completion until delivering exactly
  // one result on the calling event loop. The digest must be verified
  // against the fetched bytes before instantiation when the loader has
  // them; loaders without byte access cannot fake that guarantee.
  virtual void open(std::string path, std::string_view sha256, completion complete) = 0;
};

struct module_contract {
  std::string abi_id;
  // Exact ordered membership; abi_id also covers each entry's signature.
  std::vector<std::string> entries;
  // Expected SHA-256 of the artifact bytes: 64 lowercase hex digits. The
  // contract never trusts an unverified artifact.
  std::string sha256;
};

enum class candidate_status : std::uint8_t { loading, ready, rejected, cancelled, activated };
enum class candidate_error : std::uint8_t {
  none,
  invalid_contract,
  load_failed,
  integrity,
  missing_descriptor,
  incompatible,
  invalid_descriptor,
};

class prepared_module {
public:
  [[nodiscard]] module_function entry(std::string_view name) const noexcept;
  [[nodiscard]] std::size_t entry_count() const noexcept { return entries_.size(); }

private:
  friend class candidate;
  friend class active_module;
  struct owned_entry {
    std::string name;
    module_function address;
  };
  std::unique_ptr<module_image> image_;
  std::vector<owned_entry> entries_;
};

// Single-event-loop object. Preparation never invokes a behavior entry.
// Destroying or cancelling it discards late results, without cancelling I/O.
class candidate {
public:
  candidate(module_loader& loader, std::string path, module_contract contract);
  candidate(candidate_error error, std::string message);
  ~candidate();
  candidate(candidate&&) noexcept;
  candidate& operator=(candidate&&) noexcept;
  candidate(const candidate&) = delete;
  candidate& operator=(const candidate&) = delete;

  [[nodiscard]] candidate_status status() const noexcept;
  [[nodiscard]] candidate_error error() const noexcept;
  [[nodiscard]] std::string_view message() const noexcept;
  void cancel() noexcept;

private:
  friend class active_module;
  struct state;
  std::shared_ptr<state> state_;
};

class active_module {
public:
  // Caller keeps all reloadable code quiescent for this call. A non-ready or
  // consumed candidate returns false and leaves both objects unchanged.
  [[nodiscard]] bool activate(candidate& prepared) noexcept;
  [[nodiscard]] std::shared_ptr<const prepared_module> current() const noexcept;

private:
  std::shared_ptr<prepared_module> current_;
};

} // namespace neko::wasm
