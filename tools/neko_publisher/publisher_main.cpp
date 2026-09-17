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
#include <stdexcept>
#include <string>
#include <vector>

#include "protocol/descriptor_section.hpp"
#include "protocol/group_descriptor.hpp"
#include "runtime/publisher.hpp"

#include <fstream>

namespace {

int usage() {
  std::cerr << "usage: neko_publisher --request FILE\n"
            << "   or: neko_publisher --root DIR --key KEY --group ID --compat ID --abi ID"
            << " [--changed INPUT]... --member KEY SOURCE-IDENTITY BUILD-INFORMATION..."
            << " --objects OBJECT...\n"
            << "   or: neko_publisher descriptor [arguments]\n";
  return 2;
}

std::vector<std::string> load_request_arguments(const std::filesystem::path& path) {
  constexpr std::uintmax_t max_request_bytes = 64ull * 1024 * 1024;
  std::error_code size_error;
  const auto size = std::filesystem::file_size(path, size_error);
  if (size_error) {
    throw std::runtime_error("cannot read publish request '" + path.string() + "'");
  }
  if (size > max_request_bytes) {
    throw std::runtime_error("publish request '" + path.string() + "' exceeds 64 MiB");
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot read publish request '" + path.string() + "'");
  }

  std::string line;
  if (!std::getline(input, line)) {
    throw std::runtime_error("invalid publish request '" + path.string() +
                             "': expected nekomata-publisher-request 1");
  }
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  if (line != "nekomata-publisher-request 1") {
    throw std::runtime_error("invalid publish request '" + path.string() +
                             "': expected nekomata-publisher-request 1");
  }

  std::vector<std::string> arguments;
  std::size_t line_number = 1;
  while (std::getline(input, line)) {
    ++line_number;
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      throw std::runtime_error("invalid publish request '" + path.string() + "' at line " +
                               std::to_string(line_number) + ": empty argument");
    }
    arguments.push_back(std::move(line));
  }
  if (!input.eof()) {
    throw std::runtime_error("cannot read publish request '" + path.string() + "'");
  }
  return arguments;
}

// Writes one descriptor TU: the framed `neko-group-v1` payload as a numeric
// byte array in the `neko_groups` section. Numeric emission keeps every
// escaping question out of the generated source.
int run_descriptor(int argc, char** argv) {
  std::vector<std::string> request_arguments;
  std::vector<char*> request_argv;
  if (argc == 4 && std::string{argv[2]} == "--request") {
    try {
      request_arguments = load_request_arguments(argv[3]);
    } catch (const std::exception& error) {
      std::cerr << "neko_publisher: " << error.what() << '\n';
      return 1;
    }
    request_argv.reserve(request_arguments.size() + 2);
    request_argv.push_back(argv[0]);
    request_argv.push_back(argv[1]);
    for (auto& argument : request_arguments) {
      request_argv.push_back(argument.data());
    }
    argc = static_cast<int>(request_argv.size());
    argv = request_argv.data();
  } else if (argc > 2 && std::string{argv[2]} == "--request") {
    return usage();
  }

  std::filesystem::path output;
  neko::detail::group_descriptor descriptor;
  descriptor.baseline_sequence = 0;
  std::vector<std::string> members;

  for (int i = 2; i < argc; ++i) {
    const std::string flag = argv[i];
    const auto value = [argc, argv, &i]() -> const char* {
      if (i + 1 >= argc) {
        return nullptr;
      }
      return argv[++i];
    };
    if (flag == "--output") {
      const auto v = value();
      if (v == nullptr) {
        return usage();
      }
      output = v;
    } else if (flag == "--group") {
      const auto v = value();
      if (v == nullptr) {
        return usage();
      }
      descriptor.group_id = v;
    } else if (flag == "--key") {
      const auto v = value();
      if (v == nullptr) {
        return usage();
      }
      descriptor.publication_key = v;
    } else if (flag == "--compat") {
      const auto v = value();
      if (v == nullptr) {
        return usage();
      }
      descriptor.compatibility_id = v;
    } else if (flag == "--abi") {
      const auto v = value();
      if (v == nullptr) {
        return usage();
      }
      descriptor.abi_id = v;
    } else if (flag == "--root") {
      const auto v = value();
      if (v == nullptr) {
        return usage();
      }
      descriptor.generation_root_hint = v;
    } else if (flag == "--member") {
      const auto v = value();
      if (v == nullptr) {
        return usage();
      }
      members.emplace_back(v);
    } else {
      return usage();
    }
  }
  descriptor.members = std::move(members);

  try {
    const auto bytes = neko::detail::serialize_descriptor_section({descriptor});
    std::string source = "// Generated by neko_publisher; do not edit.\n"
                         "extern \"C\" {\n"
                         "__attribute__((used, section(\"neko_groups\"))) const unsigned char\n"
                         "    neko_embedded_descriptor[] = {\n";
    char cell[8];
    for (std::size_t index = 0; index < bytes.size(); ++index) {
      std::snprintf(cell, sizeof(cell), "0x%02x,", bytes[index]);
      source += cell;
      if (index % 16 == 15 || index + 1 == bytes.size()) {
        source += '\n';
      }
    }
    source += "};\n}\n";

    std::error_code ec;
    std::filesystem::create_directories(output.parent_path(), ec);
    std::ofstream out(output, std::ios::binary | std::ios::trunc);
    if (!out) {
      std::cerr << "neko_publisher: cannot write '" << output.string() << "'\n";
      return 1;
    }
    out.write(source.data(), static_cast<std::streamsize>(source.size()));
    return out ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << "neko_publisher: " << error.what() << '\n';
    return 1;
  }
}

} // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::string{argv[1]} == "descriptor") {
    return run_descriptor(argc, argv);
  }

  std::vector<std::string> request_arguments;
  std::vector<char*> request_argv;
  if (argc == 3 && std::string{argv[1]} == "--request") {
    try {
      request_arguments = load_request_arguments(argv[2]);
    } catch (const std::exception& error) {
      std::cerr << "neko_publisher: " << error.what() << '\n';
      return 1;
    }
    request_argv.reserve(request_arguments.size() + 1);
    request_argv.push_back(argv[0]);
    for (auto& argument : request_arguments) {
      request_argv.push_back(argument.data());
    }
    argc = static_cast<int>(request_argv.size());
    argv = request_argv.data();
  } else if (argc > 1 && std::string{argv[1]} == "--request") {
    return usage();
  }

  neko::detail::publish_request request;
  std::vector<neko::detail::publish_member> members;
  std::vector<std::filesystem::path> objects;

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
      if (i + 3 >= argc) {
        return usage();
      }
      // Object paths arrive separately through --objects: CMake expands
      // $<TARGET_OBJECTS:...> as one flat list, so members and objects zip
      // by order.
      neko::detail::publish_member member;
      member.member = argv[++i];
      member.source_identity = argv[++i];
      member.build_information = argv[++i];
      members.push_back(std::move(member));
    } else if (flag == "--objects") {
      while (i + 1 < argc && argv[i + 1][0] != '-') {
        objects.emplace_back(argv[++i]);
      }
    } else {
      return usage();
    }
  }
  if (objects.size() != members.size()) {
    std::cerr << "neko_publisher: " << members.size() << " members but " << objects.size()
              << " objects\n";
    return usage();
  }
  for (std::size_t index = 0; index < members.size(); ++index) {
    members[index].object_file = objects[index];
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
