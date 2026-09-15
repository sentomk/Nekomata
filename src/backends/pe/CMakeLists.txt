# PE backend (Windows) — not implemented yet.
#
# Planned scope, mirroring the ELF backend's layout:
#   format/  offline PE reading (relocations, sections)
#   runtime/ in-process replacement (VirtualProtect, /hotpatch entries)
#   debug/   PDB symbol source (MS DIA SDK, deliberately not a hand-rolled
#            parser; interface boundary with a possible future
#            self-contained parser)
#
# Status: not implemented. Stubs keep the directory wired into the build.
add_library(neko_backend_pe INTERFACE)
add_library(nekomata::backends::pe ALIAS neko_backend_pe)

add_library(neko_backend_pdb INTERFACE)
add_library(nekomata::backends::pdb ALIAS neko_backend_pdb)
