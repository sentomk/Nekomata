# PDB symbol backend (Windows) — not implemented yet.
#
# Planned scope:
#   * PDB reading via the MS DIA SDK (deliberately not a hand-rolled parser)
#   * interface boundary with a possible future self-contained parser
#
# Status: not implemented. Stub keeps the directory wired into the build.
add_library(neko_backend_pdb INTERFACE)
add_library(nekomata::backends::pdb ALIAS neko_backend_pdb)
