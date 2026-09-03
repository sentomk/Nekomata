# PDB symbol backend (Windows) — Phase 3.
#
# Scope (proposal §Detailed Design):
#   * PDB reading via the MS DIA SDK (deliberately not a hand-rolled parser)
#   * interface boundary with a possible future self-contained parser
#     (proposal §Open Areas)
#
# Status: not implemented. Stub keeps the directory wired into the build.
add_library(hlr_backend_pdb INTERFACE)
add_library(nekomata::backends::pdb ALIAS hlr_backend_pdb)
