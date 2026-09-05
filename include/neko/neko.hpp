// Umbrella header for the nekomata kernel.
//
// The kernel is platform-neutral: it knows the interfaces below and nothing
// else. ELF/DWARF and PE/PDB live behind them, in src/platforms/.

#pragma once

#include <neko/core/types.hpp>
#include <neko/log.hpp>
#include <neko/runtime/code_substituter.hpp>
#include <neko/runtime/object_loader.hpp>
#include <neko/runtime/patch_planner.hpp>
#include <neko/runtime/state_manager.hpp>
#include <neko/runtime/symbol_provider.hpp>
#include <neko/session.hpp>
#include <neko/version.hpp>
