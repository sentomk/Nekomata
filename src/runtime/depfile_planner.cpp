#include <neko/runtime/depfile_planner.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace neko {
namespace {

std::filesystem::path normalized_path(std::filesystem::path path,
                                      const std::filesystem::path& base = {}) {
  if (path.empty()) {
    return {};
  }
  if (path.is_relative() && !base.empty()) {
    path = base / path;
  }
  std::error_code ec;
  const auto absolute = std::filesystem::absolute(path, ec);
  if (ec) {
    throw std::runtime_error("cannot resolve dependency path '" + path.string() +
                             "': " + ec.message());
  }
  return absolute.lexically_normal();
}

std::string read_depfile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot open dependency file: " + path.string());
  }
  const std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
  if (input.bad()) {
    throw std::runtime_error("cannot read dependency file: " + path.string());
  }
  return text;
}

std::string collapse_continuations(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t index = 0; index < text.size(); ++index) {
    if (text[index] == '\\' && index + 1 < text.size() && text[index + 1] == '\n') {
      out.push_back(' ');
      ++index;
      continue;
    }
    if (text[index] == '\\' && index + 2 < text.size() && text[index + 1] == '\r' &&
        text[index + 2] == '\n') {
      out.push_back(' ');
      index += 2;
      continue;
    }
    out.push_back(text[index]);
  }
  return out;
}

bool windows_drive_colon(std::string_view line, std::size_t index) {
  return index == 1 && line.size() > 2 && std::isalpha(static_cast<unsigned char>(line[0])) != 0 &&
         (line[2] == '/' || line[2] == '\\');
}

std::size_t rule_separator(std::string_view line) {
  bool escaped = false;
  for (std::size_t index = 0; index < line.size(); ++index) {
    if (escaped) {
      escaped = false;
      continue;
    }
    if (line[index] == '\\') {
      escaped = true;
      continue;
    }
    if (line[index] == '#') {
      return std::string_view::npos;
    }
    if (line[index] == ':' && !windows_drive_colon(line, index)) {
      return index;
    }
  }
  return std::string_view::npos;
}

[[noreturn]] void reject_depfile(const std::filesystem::path& path, std::size_t line,
                                 std::string_view reason) {
  throw std::runtime_error("invalid dependency file '" + path.string() + "' at line " +
                           std::to_string(line) + ": " + std::string(reason));
}

void add_prerequisites(std::string_view line, std::size_t start,
                       const std::filesystem::path& depfile, std::size_t line_number,
                       const std::filesystem::path& working_directory,
                       std::unordered_set<std::string>& dependencies) {
  std::string token;
  const auto flush = [&] {
    if (!token.empty()) {
      dependencies.insert(normalized_path(token, working_directory).generic_string());
      token.clear();
    }
  };

  for (std::size_t index = start; index < line.size(); ++index) {
    const auto value = line[index];
    if (value == '\\') {
      if (index + 1 == line.size()) {
        reject_depfile(depfile, line_number, "dangling escape");
      }
      token.push_back(line[++index]);
      continue;
    }
    if (value == '$' && index + 1 < line.size() && line[index + 1] == '$') {
      token.push_back('$');
      ++index;
      continue;
    }
    if (value == '#') {
      break;
    }
    if (std::isspace(static_cast<unsigned char>(value)) != 0) {
      flush();
      continue;
    }
    token.push_back(value);
  }
  flush();
}

std::unordered_set<std::string> dependencies_for(const depfile_entry& entry) {
  const auto text = collapse_continuations(read_depfile(entry.dependency_file));
  std::istringstream input{text};
  std::unordered_set<std::string> dependencies;
  std::string line;
  std::size_t line_number = 0;
  bool found_rule = false;

  while (std::getline(input, line)) {
    ++line_number;
    const auto first = line.find_first_not_of(" \t\r");
    if (first == std::string::npos || line[first] == '#') {
      continue;
    }
    const auto separator = rule_separator(line);
    if (separator == std::string::npos) {
      reject_depfile(entry.dependency_file, line_number, "expected ':' after target");
    }
    found_rule = true;
    add_prerequisites(line, separator + 1, entry.dependency_file, line_number,
                      entry.working_directory, dependencies);
  }

  if (!found_rule) {
    reject_depfile(entry.dependency_file, line_number + 1, "missing dependency rule");
  }
  const auto translation_unit = entry.translation_unit.generic_string();
  if (!dependencies.contains(translation_unit)) {
    throw std::runtime_error("dependency file '" + entry.dependency_file.string() +
                             "' does not describe translation unit '" + translation_unit + "'");
  }
  return dependencies;
}

} // namespace

depfile_planner::depfile_planner(std::vector<depfile_entry> entries)
    : entries_(std::move(entries)) {
  std::error_code ec;
  const auto current_directory = std::filesystem::current_path(ec);
  if (ec) {
    throw std::runtime_error("cannot read current working directory: " + ec.message());
  }

  std::unordered_set<std::string> translation_units;
  for (auto& entry : entries_) {
    if (entry.translation_unit.empty() || entry.dependency_file.empty()) {
      throw std::invalid_argument(
          "depfile_planner: translation unit and dependency file must not be empty");
    }
    entry.working_directory = normalized_path(
        entry.working_directory.empty() ? current_directory : entry.working_directory);
    entry.translation_unit = normalized_path(entry.translation_unit, entry.working_directory);
    entry.dependency_file = normalized_path(entry.dependency_file, entry.working_directory);
    if (!translation_units.insert(entry.translation_unit.generic_string()).second) {
      throw std::invalid_argument("depfile_planner: duplicate translation unit '" +
                                  entry.translation_unit.string() + "'");
    }
  }
}

std::vector<patch_plan> depfile_planner::plan(const change_set& changes) const {
  if (changes.changed_files.empty()) {
    return {};
  }

  std::unordered_set<std::string> changed;
  for (const auto& path : changes.changed_files) {
    changed.insert(normalized_path(path).generic_string());
  }

  patch_plan plan;
  for (const auto& entry : entries_) {
    const auto dependencies = dependencies_for(entry);
    const bool affected = std::any_of(changed.begin(), changed.end(), [&](const auto& path) {
      return dependencies.contains(path);
    });
    if (affected) {
      plan.translation_units.push_back(entry.translation_unit.generic_string());
    }
  }
  if (plan.translation_units.empty()) {
    return {};
  }
  return {std::move(plan)};
}

} // namespace neko
