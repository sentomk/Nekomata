// Smoke tests for the nekomata kernel skeleton.
//
// Purpose at this stage: prove the four kernel interfaces are implementable
// and the build/pipeline (configure -> build -> ctest) works on every CI
// combination. Real reload tests arrive with the ELF backend in Phase 1.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <neko/neko.hpp>

#include <string>
#include <vector>

namespace {

class null_symbol_provider final : public neko::symbol_provider {
public:
    std::vector<neko::function_info> all_functions() const override { return {}; }

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
    CHECK(provider.layout_of(7).id == 7);
    CHECK(provider.layout_of(7).size == 0);
}
