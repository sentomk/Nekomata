// Umbrella header for the nekomata kernel.
//
// The kernel is platform-neutral: it knows the four interfaces below and
// nothing else. ELF/DWARF and PE/PDB live behind them, in backends/.

#pragma once

#include <hlr/code_substituter.hpp>
#include <hlr/patch_planner.hpp>
#include <hlr/state_manager.hpp>
#include <hlr/symbol_provider.hpp>
#include <hlr/types.hpp>
#include <hlr/version.hpp>
