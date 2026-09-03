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

class NullSymbolProvider final : public neko::SymbolProvider {
public:
    std::vector<neko::FunctionInfo> allFunctions() const override { return {}; }

    neko::TypeLayout layoutOf(neko::TypeId id) const override {
        neko::TypeLayout layout;
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
    NullSymbolProvider provider;
    CHECK(provider.allFunctions().empty());
    CHECK(provider.layoutOf(7).id == 7);
    CHECK(provider.layoutOf(7).size == 0);
}
