// Umbrella header for the nekomata kernel.
//
// The kernel is platform-neutral: it knows the interfaces below and nothing
// else. ELF/DWARF and PE/PDB live behind them, in backends/.

#pragma once

#include <neko/code_substituter.hpp>
#include <neko/log.hpp>
#include <neko/object_loader.hpp>
#include <neko/patch_planner.hpp>
#include <neko/session.hpp>
#include <neko/state_manager.hpp>
#include <neko/symbol_provider.hpp>
#include <neko/types.hpp>
#include <neko/version.hpp>
