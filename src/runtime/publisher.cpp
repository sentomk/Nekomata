#include "publisher.hpp"

#include "generation_offer.hpp"
#include <base/sha256.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#define NOMINMAX // windows.h macros must not eat std::min/std::max
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace neko::detail {
namespace {

// Exclusive, blocking advisory lock held for one publication. One stream has
// one logical producer; the lock serializes sequence allocation and the
// offer release when processes race anyway.
class stream_lock {
public:
  explicit stream_lock(const std::filesystem::path& path) {
#if defined(_WIN32)
    handle_ =
        CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
      throw std::runtime_error("cannot open stream lock '" + path.generic_string() + "'");
    }
    OVERLAPPED overlapped{};
    if (!LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &overlapped)) {
      CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
      throw std::runtime_error("cannot lock stream '" + path.generic_string() + "'");
    }
#else
    fd_ = ::open(path.c_str(), O_CREAT | O_RDWR, 0666);
    if (fd_ < 0) {
      throw std::runtime_error("cannot open stream lock '" + path.generic_string() + "'");
    }
    // Inside the condition the call cannot parse as a declaration of a
    // `struct flock` variable. BSD flock() locks the open file description,
    // so threads of one process exclude each other too.
    if (::flock(fd_, LOCK_EX) != 0) {
      ::close(fd_);
      fd_ = -1;
      throw std::runtime_error("cannot lock stream '" + path.generic_string() + "'");
    }
#endif
  }

  ~stream_lock() {
#if defined(_WIN32)
    if (handle_ != INVALID_HANDLE_VALUE) {
      OVERLAPPED overlapped{};
      UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, &overlapped);
      CloseHandle(handle_);
    }
#else
    if (fd_ >= 0) {
      ::close(fd_);
    }
#endif
  }

  stream_lock(const stream_lock&) = delete;
  stream_lock& operator=(const stream_lock&) = delete;

private:
#if defined(_WIN32)
  void* handle_ = nullptr;
#else
  int fd_ = -1;
#endif
};

std::optional<std::string> read_file(const std::filesystem::path& path) {
  std::error_code ec;
  if (ec || !std::filesystem::is_regular_file(path, ec)) {
    return std::nullopt;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return std::nullopt;
  }
  std::string content{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  if (input.bad()) {
    return std::nullopt;
  }
  return content;
}

void write_file(const std::filesystem::path& path, std::string_view bytes) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!output) {
    throw std::runtime_error("cannot write publication file '" + path.generic_string() + "'");
  }
}

std::uint64_t next_sequence(const std::filesystem::path& offers) {
  std::uint64_t next = 1;
  std::error_code ec;
  if (std::filesystem::is_directory(offers, ec) && !ec) {
    for (const auto& entry : std::filesystem::directory_iterator(offers)) {
      const auto name = entry.path().filename().string();
      if (!name.ends_with(".ready")) {
        continue;
      }
      try {
        const auto reference = parse_generation_offer_marker(name, name);
        next = std::max(next, reference.sequence + 1);
      } catch (const generation_offer_error&) {
        // Foreign or malformed names never take part in sequence allocation.
      }
    }
  }
  return next;
}

} // namespace

publish_result publish_generation(const publish_request& request) {
  if (request.publication_key.empty() || request.group_id.empty() ||
      request.compatibility_id.empty() || request.abi_id.empty() || request.members.empty()) {
    throw std::runtime_error(
        "publish request needs a publication key, group, compatibility and ABI identities, and at "
        "least one member");
  }

  const auto stream = request.generation_root / request.publication_key;
  std::filesystem::create_directories(stream / "offers");
  std::filesystem::create_directories(stream / "generations");
  const stream_lock lock(stream / ".publish.lock");

  const auto sequence = next_sequence(stream / "offers");

  // Build the manifest value first: digests come from the object bytes, and
  // the generation ID is deterministic in the published content.
  generation_offer offer;
  offer.group_id = request.group_id;
  offer.sequence = sequence;
  offer.compatibility_id = request.compatibility_id;
  offer.abi_id = request.abi_id;
  offer.changed_inputs = request.changed_inputs;
  offer.members.reserve(request.members.size());
  std::string identity_input;
  for (const auto& member : request.members) {
    const auto bytes = read_file(member.object_file);
    if (!bytes) {
      throw std::runtime_error("cannot read member object '" + member.member + "' at '" +
                               member.object_file.generic_string() + "'");
    }
    generation_offer_member record;
    record.member = member.member;
    record.object_path = "objects/" + member.member + ".o";
    record.sha256 = sha256_hex(*bytes);
    record.source_identity = member.source_identity;
    record.build_information = member.build_information;
    identity_input += record.sha256;
    offer.members.push_back(std::move(record));
  }
  // Identity is the content only: republishing identical objects reuses the
  // generation ID at the next sequence, which the model explicitly allows.
  offer.generation_id = "g-" + sha256_hex(identity_input).substr(0, 16);
  validate_generation_offer(offer, "publisher request");

  const auto manifest = serialize_generation_offer(offer);

  // Stage every file beside the stream, then release with two same-directory
  // renames: the generation directory first, the ready offer last. A crash
  // leaves staging data and maybe an unreferenced generation, never an
  // incomplete offer.
  const auto nonce = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  const auto staging = stream / (".staging-" + nonce);
  std::filesystem::create_directories(staging);
  for (const auto& member : request.members) {
    const auto bytes = read_file(member.object_file);
    if (!bytes) {
      throw std::runtime_error("member object '" + member.member + "' vanished while publishing");
    }
    write_file(staging / ("objects/" + member.member + ".o"), *bytes);
  }
  write_file(staging / "manifest", manifest);

  const auto generation_directory = stream / "generations" / offer.generation_id;
  // Identical content yields the same ID: republishing replaces the old
  // directory under the stream lock instead of failing the rename.
  std::error_code replace_ec;
  std::filesystem::remove_all(generation_directory, replace_ec);
  std::filesystem::rename(staging, generation_directory);

  const auto marker =
      stream / "offers" / serialize_generation_offer_marker({offer.sequence, offer.generation_id});
  write_file(stream / "offers" / "offer.staging", "");
  std::filesystem::rename(stream / "offers" / "offer.staging", marker);

  return {offer.sequence, offer.generation_id};
}

} // namespace neko::detail
