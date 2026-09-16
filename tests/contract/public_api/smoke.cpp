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

class null_symbol_provider final : public neko::backend::symbol_provider {
public:
  std::vector<neko::backend::function_info> all_functions() const override { return {}; }

  std::optional<neko::backend::function_info> function_by_name(std::string_view) const override {
    return std::nullopt;
  }

  std::size_t count_functions(std::string_view) const override { return 0; }

  std::size_t count_globals(std::string_view) const override { return 0; }

  std::optional<neko::backend::global_variable> global_by_name(std::string_view) const override {
    return std::nullopt;
  }

  neko::backend::type_layout layout_of(neko::backend::type_id id) const override {
    neko::backend::type_layout layout;
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

TEST_CASE("generation linkage metadata keeps target and encoding distinct") {
  const neko::backend::generation_symbol object_symbol{
      "counter",
      neko::backend::generation_symbol_kind::object,
      16,
  };
  const neko::backend::generation_fixup function_call{
      "tick",
      neko::backend::generation_symbol_kind::function,
      neko::backend::generation_fixup_kind::function_trampoline,
      32,
  };

  CHECK(object_symbol.kind == neko::backend::generation_symbol_kind::object);
  CHECK(object_symbol.offset_in_image == 16);
  CHECK(function_call.target_kind == neko::backend::generation_symbol_kind::function);
  CHECK(function_call.kind == neko::backend::generation_fixup_kind::function_trampoline);
  CHECK(function_call.offset_in_image == 32);
}

TEST_CASE("reload_session rejects incomplete backend bundles") {
  neko::backend::bundle incomplete;
  incomplete.symbols = std::make_shared<null_symbol_provider>();
  CHECK_THROWS_AS(neko::reload_session{std::move(incomplete)}, std::runtime_error);
}
