#include "publisher.hpp"

#include <base/file.hpp>
#include <base/lock.hpp>
#include <base/sha256.hpp>
#include <protocol/generation_offer.hpp>
#include <protocol/wasm_offer.hpp>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>
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

// Sequence-named artifacts `<sequence>-<generation>.wasm` make sequence
// allocation stateless, mirroring the managed ready markers.
std::uint64_t next_wasm_sequence(const std::filesystem::path& modules) {
  std::uint64_t next = 1;
  std::error_code ec;
  if (std::filesystem::is_directory(modules, ec) && !ec) {
    for (const auto& entry : std::filesystem::directory_iterator(modules)) {
      const auto name = entry.path().filename().string();
      const auto dash = name.find('-');
      if (dash == std::string::npos || !name.ends_with(".wasm")) {
        continue;
      }
      const auto digits = name.substr(0, dash);
      if (digits.empty() || !std::ranges::all_of(digits, [](unsigned char byte) {
            return byte >= '0' && byte <= '9';
          })) {
        continue;
      }
      try {
        next = std::max(next, std::stoull(digits) + 1);
      } catch (const std::out_of_range&) {
        // Absurdly large numbers never take part in sequence allocation.
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

publish_result publish_wasm_offer(const wasm_publish_request& request) {
  if (request.offer_root.empty() || request.publication_key.empty() || request.group_id.empty() ||
      request.abi_id.empty() || request.module_file.empty() || request.entries.empty()) {
    throw std::runtime_error("wasm publish request needs an offer root, publication key, group "
                             "and ABI identities, a module, and at least one entry");
  }

  const auto modules = request.offer_root / "modules";
  std::filesystem::create_directories(modules);
  // One offer URL has one logical producer; the lock serializes sequence
  // allocation and the manifest replacement when processes race anyway.
  const file_lock lock(request.offer_root / ".publish.lock");

  const auto sequence = next_wasm_sequence(modules);

  const auto module_bytes = read_file_if_present(request.module_file);
  if (!module_bytes) {
    throw std::runtime_error("cannot read wasm module '" + request.module_file.generic_string() +
                             "'");
  }
  const auto module_digest = sha256_hex(*module_bytes);

  std::string identity_input = request.abi_id + "\n" + module_digest + "\n";
  for (const auto& entry : request.entries) {
    identity_input += entry;
    identity_input += '\n';
  }

  wasm_offer offer;
  offer.group_id = request.group_id;
  offer.sequence = sequence;
  offer.abi_id = request.abi_id;
  offer.entries = request.entries;
  offer.generation_id = "g-" + sha256_hex(identity_input).substr(0, 16);
  offer.artifact_path = "modules/" + std::to_string(sequence) + "-" + offer.generation_id + ".wasm";
  offer.sha256 = module_digest;
  validate_wasm_offer(offer, "wasm publish request");

  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto staged_module = modules / (".staging-" + std::to_string(nonce) + ".wasm");
  write_required_file(staged_module, *module_bytes, "cannot write wasm artifact");
  std::error_code rename_ec;
  std::filesystem::rename(staged_module, request.offer_root / offer.artifact_path, rename_ec);
  if (rename_ec) {
    std::filesystem::remove(staged_module, rename_ec);
    throw std::runtime_error("cannot release wasm artifact '" + offer.artifact_path + "'");
  }

  // The manifest replacement is the publication point: staged beside its
  // destination and renamed, so a page never observes a torn offer.
  write_required_file(request.offer_root / "latest.staging", serialize_wasm_offer(offer),
                      "cannot write wasm offer");
  std::filesystem::rename(request.offer_root / "latest.staging", request.offer_root / "latest");
  return {offer.sequence, offer.generation_id};
}

} // namespace neko::detail
