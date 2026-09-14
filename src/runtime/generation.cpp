#include "generation.hpp"

#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace neko::detail {
namespace {

class claimed_manifest {
public:
  explicit claimed_manifest(std::filesystem::path path) : path_(std::move(path)) {}

  ~claimed_manifest() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
  }

  claimed_manifest(const claimed_manifest&) = delete;
  claimed_manifest& operator=(const claimed_manifest&) = delete;

private:
  std::filesystem::path path_;
};

[[noreturn]] void reject_manifest(const std::filesystem::path& path, std::size_t line,
                                  std::string_view message) {
  throw std::runtime_error("invalid generation manifest '" + path.string() + "' at line " +
                           std::to_string(line) + ": " + std::string(message));
}

std::filesystem::path resolve_path(const std::filesystem::path& base, const std::string& value) {
  std::filesystem::path path{value};
  if (path.is_relative()) {
    path = base / path;
  }
  std::error_code ec;
  const auto absolute = std::filesystem::absolute(path, ec);
  return (ec ? path : absolute).lexically_normal();
}

std::string read_quoted(std::istringstream& row, const std::filesystem::path& path,
                        std::size_t line, std::string_view field) {
  row >> std::ws;
  if (row.peek() != '"') {
    reject_manifest(path, line, std::string(field) + " must be quoted");
  }
  std::string value;
  row >> std::quoted(value);
  if (!row) {
    reject_manifest(path, line, std::string("cannot read ") + std::string(field));
  }
  return value;
}

void require_end(std::istringstream& row, const std::filesystem::path& path, std::size_t line) {
  row >> std::ws;
  if (!row.eof()) {
    reject_manifest(path, line, "unexpected trailing fields");
  }
}

legacy_generation_offer parse_manifest(std::string_view text, const std::filesystem::path& path) {
  std::istringstream input{std::string(text)};
  std::string line;
  std::size_t line_number = 0;
  if (!std::getline(input, line)) {
    reject_manifest(path, 1, "missing format header");
  }
  ++line_number;
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  if (line != "nekomata-generation-v1") {
    reject_manifest(path, line_number, "expected nekomata-generation-v1");
  }

  legacy_generation_offer offer;
  bool has_id = false;
  std::unordered_set<std::string> changed_files;
  std::unordered_set<std::string> object_paths;
  std::unordered_set<std::string> source_paths;
  const auto base = path.parent_path();

  while (std::getline(input, line)) {
    ++line_number;
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    std::istringstream row{line};
    row >> std::ws;
    if (row.eof() || row.peek() == '#') {
      continue;
    }

    std::string directive;
    row >> directive;
    if (directive == "id") {
      if (has_id) {
        reject_manifest(path, line_number, "duplicate id");
      }
      offer.id = read_quoted(row, path, line_number, "id");
      require_end(row, path, line_number);
      if (offer.id.empty()) {
        reject_manifest(path, line_number, "id must not be empty");
      }
      has_id = true;
      continue;
    }

    if (directive == "changed") {
      const auto raw = read_quoted(row, path, line_number, "changed path");
      require_end(row, path, line_number);
      if (raw.empty()) {
        reject_manifest(path, line_number, "changed path must not be empty");
      }
      auto changed = resolve_path(base, raw);
      if (!changed_files.insert(changed.generic_string()).second) {
        reject_manifest(path, line_number, "duplicate changed path");
      }
      offer.changed_files.push_back(std::move(changed));
      continue;
    }

    if (directive == "object") {
      const auto raw_object = read_quoted(row, path, line_number, "object path");
      const auto raw_source = read_quoted(row, path, line_number, "source path");
      const auto build_information = read_quoted(row, path, line_number, "build information");
      require_end(row, path, line_number);
      if (raw_object.empty() || raw_source.empty() || build_information.empty()) {
        reject_manifest(path, line_number, "object fields must not be empty");
      }

      legacy_generation_object_offer object;
      object.object_path = resolve_path(base, raw_object);
      object.source_path = resolve_path(base, raw_source);
      object.build_information = build_information;
      if (!object_paths.insert(object.object_path.generic_string()).second) {
        reject_manifest(path, line_number, "duplicate object path");
      }
      if (!source_paths.insert(object.source_path.generic_string()).second) {
        reject_manifest(path, line_number, "duplicate source path");
      }
      offer.objects.push_back(std::move(object));
      continue;
    }

    reject_manifest(path, line_number, "unknown directive '" + directive + "'");
  }

  if (!has_id) {
    reject_manifest(path, line_number + 1, "missing id");
  }
  if (offer.changed_files.empty()) {
    reject_manifest(path, line_number + 1, "missing changed paths");
  }
  if (offer.objects.empty()) {
    reject_manifest(path, line_number + 1, "missing objects");
  }
  return offer;
}

} // namespace

std::optional<legacy_generation_offer>
try_claim_generation_manifest(const std::filesystem::path& manifest_path) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(manifest_path, ec)) {
    return std::nullopt;
  }

  const auto claimed_path =
      manifest_path.parent_path() / (manifest_path.filename().string() + ".claimed");
  std::filesystem::rename(manifest_path, claimed_path, ec);
  if (ec) {
    return std::nullopt;
  }
  claimed_manifest claim{claimed_path};

  std::ifstream file(claimed_path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("cannot open claimed generation manifest: " + claimed_path.string());
  }
  const std::string text{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
  if (file.bad()) {
    throw std::runtime_error("cannot read claimed generation manifest: " + claimed_path.string());
  }
  return parse_manifest(text, manifest_path);
}

} // namespace neko::detail
