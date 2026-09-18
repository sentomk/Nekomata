# PDB symbol suites: DIA against this executable's own PDB, Windows only.
# Address equality with the live process is meaningless under incremental
# linking — code references the ILT thunks while the PDB publics point at
# the real bodies — so this exe links non-incrementally.

nekomata_add_unit_test(
  NAME unit.backends.pdb.process_symbols
  SOURCES process_symbols.cpp
  PRIVATE_INCLUDES "${PROJECT_SOURCE_DIR}/src/backends/pe/debug"
  LIBRARIES neko_backend_pdb)

set_target_properties(neko_unit_backends_pdb_process_symbols PROPERTIES
  LINK_OPTIONS "/INCREMENTAL:NO")
