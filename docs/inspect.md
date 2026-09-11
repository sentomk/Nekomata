# Inspecting a binary

`nekomata inspect <binary>` reports what a binary's debug information says
about its functions — compilation units, names, declaration locations, code
ranges, and how DWARF entries associate with the ELF symbol table — without
running it, attaching to a process, or modifying the file. Exit code zero means
the inspection finished, **not** that every function is matched or safe to
patch.

Everything below is the contract: what is matched, what is refused, and the
resource budgets. It describes the tool, not the reload path; the runtime uses
its own symbol lookup.

```sh
./build/debug/tools/nekomata/nekomata inspect \
  ./build/debug/tests/dwarf/neko_dwarf_fixture
ctest --test-dir build/debug -R 'neko.dwarf' --output-on-failure
```

`inspect` reads an existing binary without executing it, attaching to a process,
or modifying the file. It reports compilation units, function names, optional
linkage names, and half-open code ranges `[begin, end)`. Same-name local functions
remain separate records under their own compilation units. Addresses are
**link-time virtual addresses**, not file offsets or relocated process addresses.
The binary plus CU/DIE offsets identify records within that file; offsets are
not stable identities across rebuilds. Missing linkage names are printed as
unavailable, never guessed from source names.

Functions also report declaration file, line and column from `DW_AT_decl_*`.
Missing attributes and explicitly unspecified (zero) coordinates are printed
as `<unavailable>`. `DW_AT_specification` / `DW_AT_abstract_origin` inheritance
is followed, with direct values taking precedence. An inherited file index is
resolved against the **originating DIE's** compilation unit, including references
across CUs. This describes the declaration recorded by the compiler, not the
function's full source extent, an address-to-line map, or a stable identity
across rebuilds. It may refer to a header rather than the CU's main source.

File paths combine the DWARF 4 line-table header's filename/include directory
with the recorded compilation directory. Absolute paths stay absolute; relative
paths stay relative if the recorded directories are relative or missing.
No source file is opened, and paths are not canonicalized against the machine
running the inspector. Missing line tables leave the file unknown without
discarding independently known line/column values. Invalid indexes, malformed
coordinates and unsupported line-table formats fail inspection. Dynamically
added file entries (`DW_LNE_define_file`) are not resolved by this header-only
file-table reader; references beyond the header's entries are rejected.

The initial supported input is an x86-64 ELF `ET_EXEC` with embedded DWARF 4,
DWARF32 offsets and 8-byte addresses, built with `-O0 -g -gdwarf-4`, `-fno-pie`
and linked with `-no-pie`. These options belong to the **inspected project's**
build, not to Nekomata itself. PIE/shared libraries, relocatable `.o` files,
DWARF 5, split/compressed debug information, inline instances and function range
lists are not supported yet. Declarations are skipped; definitions without
emitted ranges are reported separately. Missing or unsupported information
causes a diagnostic and nonzero exit instead of a partial successful listing.

The internal ELF-only reader additionally indexes functions in x86-64 `ET_REL`
objects, including separate function sections. Their symbol values are offsets
within the recorded section, not virtual addresses. This does not yet enable
`.o` input in `nekomata inspect`: relocatable DWARF reading and section-aware
ELF/DWARF association remain unsupported, and runtime loading is unchanged.

The ELF-only reader can also inspect relocations targeting `.debug_info` in
`ET_REL` files. It follows section links rather than relocation-section names,
retains relocation and symbol-table identities, and computes section-relative
`S + A` offsets for `R_X86_64_32` and `R_X86_64_64` RELA entries. It does not
apply relocations or decode DWARF. Missing `.debug_info` is distinct from a
present section without relocations. Undefined/special symbol references,
compressed/split/grouped debug information, other relocation types, overlapping
fields, overflow and out-of-bounds references fail explicitly. An offset equal
to the referenced section's size is retained as a possible exclusive endpoint;
this is not approval of a function range. The reader limits total relocation
entries and entries in referenced symbol tables to one million each, and total
section-name/symbol string-table reads to 64 MiB per call.

The inspector also associates DWARF functions with defined ELF `STT_FUNC`
entries from `.symtab`, retaining table/entry identity, section, binding,
visibility, address and size. It reads both through one open file descriptor;
do not modify that file in place during inspection. A match requires a unique
entry address on both sides, an exact nonzero size, and an equal linkage name
when DWARF provides one. Missing linkage names remain unavailable, not guessed.
Same-name local symbols stay distinct. Multiple symbols at an entry (including
aliases), or multiple DWARF definitions there, remain `ambiguous` even if one
name matches. Zero symbol size means unknown, not an empty function.

Each emitted function reports `match=matched`, `missing-symtab`,
`missing-symbol`, `ambiguous`, `unknown-size`, `range-mismatch`, or
`linkage-mismatch`, followed by every candidate's ELF metadata. ELF functions
without a DWARF entry (such as startup code) are listed separately. Missing
`.symtab` is reported without substituting `.dynsym`; malformed tables fail
inspection. Extended section numbering and special function section indexes
are currently rejected. Per inspection, symbol-table entries and stored
association candidates are each limited to one million; total string-table
bytes and copied function-name bytes are each limited to 64 MiB. Inputs that
exceed these limits fail explicitly rather than returning a partial index.
DWARF inspection additionally budgets 64 MiB each for cumulative copied string
bytes (including temporary path assembly) and source-table headers, one million
source file/directory entries across loaded CU tables, and eight million
attribute-reference traversal steps. Source-table entries use direct indexing;
resolved paths are cached per CU/index. Budget failures produce no partial listing.
libdwarf 2.3.2 expands line-program rows when opening a line context, so this
inspector decodes only the bounded DWARF 4 header from the same ELF descriptor.
It does not read, execute or validate the line program. libdwarf remains the
reader for compilation units, DIEs and attributes.

This is an offline foundation, not an expansion of the runtime's hot-reload
support or a cross-build matching API. Exit code zero means inspection finished,
**not** that every function is matched or safe to patch. The runtime still uses
its original symbol lookup. Tests compare fixture ranges and linkage names
with GNU `nm`, and cover duplicate local names, aliases, overloads, out-of-line
members, malformed input, missing metadata and repeated inspection.
`readelf --debug-dump=info <binary>` or
`llvm-dwarfdump --debug-info <binary>` can provide another independent reference.

Linux inspection builds fetch checksum-pinned **libdwarf 2.3.2**, built as a
shared library and linked only into the inspection tooling. Its headers do not
enter Nekomata's public API or the runtime. The upstream library is LGPL-2.1;
its license and notices remain in the fetched source tree. See
[upstream licensing](https://github.com/davea42/libdwarf-code/blob/v2.3.2/COPYING).
This build does not include `dwarfdump` or debug-section decompression libraries.

For an offline build, supply an already extracted copy of that exact version:

```sh
cmake --preset debug \
  -DFETCHCONTENT_SOURCE_DIR_LIBDWARF=/absolute/path/to/libdwarf-code-2.3.2
```

Alternatively, `-DNEKOMATA_ENABLE_DWARF_INSPECTION=OFF` keeps the existing runtime
and its tests buildable without downloading libdwarf. macOS/Windows builds do
not fetch this dependency and report that `inspect` is unavailable.
