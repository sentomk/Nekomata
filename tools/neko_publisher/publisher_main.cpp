// neko_publisher — the private host publication executable.
//
// Build adapters invoke this after their native build produced the group's
// objects. It is implementation machinery for `nekomata_add_reload_group()`
// and friends, not a supported user-facing CLI; its arguments change with
// the adapter module and carry no stability promise.

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "runtime/publisher.hpp"

namespace {

int usage() {
  std::cerr << "usage: neko_publisher --root DIR --key KEY --group ID --compat ID --abi ID"
            << " [--changed INPUT]..."
            << " --member KEY OBJECT SOURCE-IDENTITY BUILD-INFORMATION...\n";
  return 2;
}

} // namespace

int main(int argc, char** argv) {
  neko::detail::publish_request request;
  std::vector<neko::detail::publish_member> members;

  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    const auto value = [argc, argv, &i]() -> const char* {
      if (i + 1 >= argc) {
        return nullptr;
      }
      return argv[++i];
    };
    if (flag == "--root") {
      const auto v = value();
      if (v == nullptr) {
        return usage();
      }
      request.generation_root = v;
    } else if (flag == "--key") {
      const auto v = value();
      if (v == nullptr) {
        return usage();
      }
      request.publication_key = v;
    } else if (flag == "--group") {
      const auto v = value();
      if (v == nullptr) {
        return usage();
      }
      request.group_id = v;
    } else if (flag == "--compat") {
      const auto v = value();
      if (v == nullptr) {
        return usage();
      }
      request.compatibility_id = v;
    } else if (flag == "--abi") {
      const auto v = value();
      if (v == nullptr) {
        return usage();
      }
      request.abi_id = v;
    } else if (flag == "--changed") {
      const auto v = value();
      if (v == nullptr) {
        return usage();
      }
      request.changed_inputs.emplace_back(v);
    } else if (flag == "--member") {
      if (i + 4 >= argc) {
        return usage();
      }
      neko::detail::publish_member member;
      member.member = argv[++i];
      member.object_file = argv[++i];
      member.source_identity = argv[++i];
      member.build_information = argv[++i];
      members.push_back(std::move(member));
    } else {
      return usage();
    }
  }
  request.members = std::move(members);

  try {
    const auto result = neko::detail::publish_generation(request);
    std::cout << result.sequence << ' ' << result.generation_id << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "neko_publisher: " << error.what() << '\n';
    return 1;
  }
}
