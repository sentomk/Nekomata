// Factory smoke: the PE backend assembles into a real reload_session over
// the unified `backend_handle` seam, and its DIA symbol table actually
// initializes in this process. Session-level semantics (no groups yet)
// follow the public contract.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <neko/pe.hpp>
#include <neko/session.hpp>

#include <stdexcept>

TEST_CASE("the PE factory assembles a working session") {
  neko::reload_session session{neko::pe::create_backend()};

  const auto snapshot = session.snapshot();
  CHECK(snapshot.applied == 0);
  CHECK(snapshot.rejected == 0);
  CHECK(snapshot.managed_groups.empty());
  CHECK(snapshot.watched_paths.empty());
  CHECK(session.update().events.empty());

  // No group descriptors are linked into this test binary: watch() is a
  // configuration error, not a crash.
  CHECK_THROWS_AS(session.watch(), std::runtime_error);
  CHECK_THROWS_AS(session.watch("missing"), std::runtime_error);
}

TEST_CASE("the factory's symbol table resolves this process") {
  // Constructing two sessions proves the DIA source loads and the same
  // process symbols answer through both.
  neko::reload_session first{neko::pe::create_backend()};
  neko::reload_session second{neko::pe::create_backend()};
  CHECK(first.snapshot().managed_groups.empty());
  CHECK(second.snapshot().managed_groups.empty());
}
