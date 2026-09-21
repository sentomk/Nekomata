#pragma once

#include "candidate.hpp"

#include <neko/session.hpp>

namespace neko::wasm {

// Translate typed backend failures, never their diagnostic text. load_failed
// includes transport, staging and instantiation failures; it does not prove
// an integrity failure. No candidate rejection represents a rolled-back commit.
[[nodiscard]] constexpr reload_error_code classify_candidate_error(candidate_error error) noexcept {
  switch (error) {
  case candidate_error::none:
    return reload_error_code::none;
  case candidate_error::invalid_contract:
    return reload_error_code::invalid_artifact;
  case candidate_error::incompatible:
    return reload_error_code::incompatible;
  case candidate_error::integrity:
    return reload_error_code::integrity;
  case candidate_error::load_failed:
  case candidate_error::missing_descriptor:
  case candidate_error::invalid_descriptor:
    return reload_error_code::object_rejected;
  }
  return reload_error_code::object_rejected;
}

} // namespace neko::wasm
