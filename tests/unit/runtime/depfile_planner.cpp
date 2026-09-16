// Unit tests for GNU Make-compatible compiler dependency-file planning.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <neko/legacy/depfile_planner.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

class temporary_directory {
public:
  temporary_directory() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ =
        std::filesystem::temp_directory_path() / ("neko-depfile-planner-" + std::to_string(nonce));
    if (!std::filesystem::create_directory(path_)) {
      throw std::runtime_error("cannot create depfile planner test directory");
    }
  }

  ~temporary_directory() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  const std::filesystem::path& path() const { return path_; }

private:
  std::filesystem::path path_;
};

void write_file(const std::filesystem::path& path, std::string_view text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  output << text;
  if (!output) {
    throw std::runtime_error("cannot create depfile planner test input");
  }
}

neko::backend::change_set changed(const std::filesystem::path& path) {
  neko::backend::change_set changes;
  changes.changed_files.push_back(path.generic_string());
  return changes;
}

} // namespace

TEST_CASE("a shared header expands to every dependent translation unit") {
  temporary_directory temporary;
  const auto root = temporary.path();
  write_file(root / "deps/a.d", "obj/a.o: source/a.cpp include/shared.hpp \\\n"
                                " include/only_a.hpp\n"
                                "include/shared.hpp:\n"
                                "include/only_a.hpp:\n");
  write_file(root / "deps/b.d", "obj/b.o: source/b.cpp include/shared.hpp\n"
                                "include/shared.hpp:\n");

  const neko::depfile_planner planner{{
      {"source/a.cpp", "deps/a.d", root},
      {"source/b.cpp", "deps/b.d", root},
  }};

  const auto plans = planner.plan(changed(root / "include/shared.hpp"));
  REQUIRE(plans.size() == 1);
  REQUIRE(plans[0].translation_units.size() == 2);
  CHECK(plans[0].translation_units[0] == (root / "source/a.cpp").generic_string());
  CHECK(plans[0].translation_units[1] == (root / "source/b.cpp").generic_string());

  const auto direct = planner.plan(changed(root / "source/a.cpp"));
  REQUIRE(direct.size() == 1);
  REQUIRE(direct[0].translation_units.size() == 1);
  CHECK(direct[0].translation_units[0] == (root / "source/a.cpp").generic_string());

  CHECK(planner.plan(changed(root / "include/unrelated.hpp")).empty());
  CHECK(planner.plan(neko::backend::change_set{}).empty());
}

TEST_CASE("depfile paths support Make escapes and refresh between plans") {
  temporary_directory temporary;
  const auto root = temporary.path();
  const auto depfile = root / "deps/widget.d";
  write_file(depfile, "obj/widget.o: source/widget.cpp include/path\\ with\\ spaces.hpp "
                      "include/cash$$money.hpp include/hash\\#name.hpp\n");

  const neko::depfile_planner planner{{
      {"source/widget.cpp", "deps/widget.d", root},
  }};

  auto plans = planner.plan(changed(root / "include/path with spaces.hpp"));
  REQUIRE(plans.size() == 1);
  CHECK(plans[0].translation_units ==
        std::vector<std::string>{(root / "source/widget.cpp").generic_string()});
  CHECK(planner.plan(changed(root / "include/cash$money.hpp")).size() == 1);
  CHECK(planner.plan(changed(root / "include/hash#name.hpp")).size() == 1);

  write_file(depfile, "obj/widget.o: source/widget.cpp include/new.hpp\n");
  CHECK(planner.plan(changed(root / "include/path with spaces.hpp")).empty());
  CHECK(planner.plan(changed(root / "include/new.hpp")).size() == 1);
}

TEST_CASE("a missing depfile rejects an incomplete dependency graph") {
  temporary_directory temporary;
  const auto root = temporary.path();
  const auto depfile = root / "deps/missing.d";
  const neko::depfile_planner planner{{
      {"source/widget.cpp", "deps/missing.d", root},
  }};

  const auto message = "cannot open dependency file: " + depfile.generic_string();
  CHECK_THROWS_WITH_AS(planner.plan(changed(root / "include/shared.hpp")), message.c_str(),
                       std::runtime_error);
}

TEST_CASE("malformed depfiles are rejected instead of returning partial plans") {
  temporary_directory temporary;
  const auto root = temporary.path();
  const auto no_rule = root / "deps/no-rule.d";
  const auto no_colon = root / "deps/no-colon.d";
  const auto dangling_escape = root / "deps/dangling-escape.d";
  const auto wrong_source = root / "deps/wrong-source.d";
  write_file(no_rule, "# generated but empty\n\n");
  write_file(no_colon, "obj/widget.o source/widget.cpp\n");
  write_file(dangling_escape, "obj/widget.o: source/widget.cpp \\");
  write_file(wrong_source, "obj/widget.o: source/other.cpp include/shared.hpp\n");

  const auto plan_with = [&](const std::filesystem::path& depfile) {
    const neko::depfile_planner planner{{
        {"source/widget.cpp", depfile, root},
    }};
    return planner.plan(changed(root / "include/shared.hpp"));
  };

  CHECK_THROWS_WITH_AS(plan_with(no_rule),
                       ("invalid dependency file '" + no_rule.generic_string() +
                        "' at line 3: missing dependency rule")
                           .c_str(),
                       std::runtime_error);
  CHECK_THROWS_WITH_AS(plan_with(no_colon),
                       ("invalid dependency file '" + no_colon.generic_string() +
                        "' at line 1: expected ':' after target")
                           .c_str(),
                       std::runtime_error);
  CHECK_THROWS_WITH_AS(plan_with(dangling_escape),
                       ("invalid dependency file '" + dangling_escape.generic_string() +
                        "' at line 1: dangling escape")
                           .c_str(),
                       std::runtime_error);
  CHECK_THROWS_WITH_AS(plan_with(wrong_source),
                       ("dependency file '" + wrong_source.generic_string() +
                        "' does not describe translation unit '" +
                        (root / "source/widget.cpp").generic_string() + "'")
                           .c_str(),
                       std::runtime_error);
}

TEST_CASE("duplicate translation-unit registrations are rejected") {
  temporary_directory temporary;
  const auto root = temporary.path();
  const auto source = (root / "source/widget.cpp").generic_string();

  CHECK_THROWS_WITH_AS(neko::depfile_planner({
                           {"source/widget.cpp", "deps/first.d", root},
                           {"source/../source/widget.cpp", "deps/second.d", root},
                       }),
                       ("depfile_planner: duplicate translation unit '" + source + "'").c_str(),
                       std::invalid_argument);
}
