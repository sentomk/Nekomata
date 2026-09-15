#include "publisher.hpp"

#include <base/file.hpp>
#include <base/lock.hpp>
#include <base/sha256.hpp>
#include <protocol/generation_offer.hpp>

#include <algorithm>
#include <chrono>
#include <system_error>
#include <utility>

namespace neko::detail {
namespace {

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
  // One stream has one logical producer; the lock serializes sequence
  // allocation and the offer release when processes race anyway.
  const file_lock lock(stream / ".publish.lock");

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
    const auto bytes = read_file_if_present(member.object_file);
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
  // Member keys may nest ("group/a"), so every staged write creates its own
  // parent directories; directory layout is publication policy, not base
  // file policy.
  const auto stage = [&staging](const std::string& relative, std::string_view bytes) {
    const auto destination = staging / relative;
    std::filesystem::create_directories(destination.parent_path());
    write_required_file(destination, bytes, "cannot write publication file");
  };
  for (const auto& member : request.members) {
    const auto bytes = read_file_if_present(member.object_file);
    if (!bytes) {
      throw std::runtime_error("member object '" + member.member + "' vanished while publishing");
    }
    stage("objects/" + member.member + ".o", *bytes);
  }
  stage("manifest", manifest);

  const auto generation_directory = stream / "generations" / offer.generation_id;
  // Identical content yields the same ID: republishing replaces the old
  // directory under the stream lock instead of failing the rename.
  std::error_code replace_ec;
  std::filesystem::remove_all(generation_directory, replace_ec);
  std::filesystem::rename(staging, generation_directory);

  const auto marker =
      stream / "offers" / serialize_generation_offer_marker({offer.sequence, offer.generation_id});
  write_required_file(stream / "offers" / "offer.staging", "", "cannot write publication file");
  std::filesystem::rename(stream / "offers" / "offer.staging", marker);

  return {offer.sequence, offer.generation_id};
}

} // namespace neko::detail
