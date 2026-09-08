// Smoke tests for the nekomata kernel.
//
// Purpose: prove the kernel interfaces are implementable and the build
// pipeline works on every CI combination. The end-to-end reload behavior is
// covered by the neko.hello_reload integration test (Linux only).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <neko/neko.hpp>

#include <string>
#include <vector>

namespace {

class null_symbol_provider final : public neko::symbol_provider {
public:
  std::vector<neko::function_info> all_functions() const override { return {}; }

  std::optional<neko::function_info> function_by_name(std::string_view) const override {
    return std::nullopt;
  }

  std::size_t count_functions(std::string_view) const override { return 0; }

  std::size_t count_globals(std::string_view) const override { return 0; }

  std::optional<neko::global_variable> global_by_name(std::string_view) const override {
    return std::nullopt;
  }

  neko::type_layout layout_of(neko::type_id id) const override {
    neko::type_layout layout;
    layout.id = id;
    return layout;
  }
};

} // namespace

TEST_CASE("version_string matches the configured project version") {
  CHECK(neko::version_string() == NEKOMATA_VERSION_STRING);
  CHECK_FALSE(neko::version_string().empty());
}

TEST_CASE("kernel interfaces are implementable and default-usable") {
  null_symbol_provider provider;
  CHECK(provider.all_functions().empty());
  CHECK_FALSE(provider.function_by_name("tick").has_value());
  CHECK_FALSE(provider.global_by_name("g_counter").has_value());
  CHECK(provider.layout_of(7).id == 7);
}

TEST_CASE("reload_session rejects incomplete backend bundles") {
  neko::backend_bundle incomplete;
  incomplete.symbols = std::make_shared<null_symbol_provider>();
  CHECK_THROWS_AS(neko::reload_session{std::move(incomplete)}, std::runtime_error);
}
