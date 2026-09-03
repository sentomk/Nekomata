# PE binary backend (Windows) — Phase 3.
#
# Scope (proposal §Detailed Design):
#   * PE relocation processing for freshly compiled code
#   * /hotpatch (/FUNCTIONPADMIN) entry-rewrite convention
#
# Status: not implemented. Stub keeps the directory wired into the build.
add_library(neko_backend_pe INTERFACE)
add_library(nekomata::backends::pe ALIAS neko_backend_pe)
