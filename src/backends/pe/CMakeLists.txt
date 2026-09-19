# PE binary backend.
#
# format/        offline COFF reading (sections, symbols, relocations,
#                unwind associations) — pure byte parsing, built on every
#                platform so the reader suites run wherever a fixture can be
#                produced
# runtime/       in-process hot replacement (code_pages) — Windows only
# debug/         PDB symbol source through the MS DIA SDK — deliberately not
#                a hand-rolled parser, with an interface boundary left open
#                for a future self-contained one

add_library(neko_pe_file STATIC
  format/object_file.cpp
  format/image_layout.cpp)
nekomata_apply_warnings(neko_pe_file)
nekomata_apply_sanitizers(neko_pe_file)
nekomata_apply_clang_tidy(neko_pe_file)

if(NEKOMATA_TARGET_WINDOWS)
  add_library(neko_backend_pe STATIC runtime/code_pages.cpp)
  add_library(nekomata::backends::pe ALIAS neko_backend_pe)

  target_include_directories(neko_backend_pe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(neko_backend_pe
    PUBLIC neko_runtime
    PRIVATE neko_base_c)

  nekomata_apply_warnings(neko_backend_pe)
  nekomata_apply_sanitizers(neko_backend_pe)
  nekomata_apply_clang_tidy(neko_backend_pe)

  # DIA SDK discovery walks the compiler's directory ancestors to the VS
  # installation root — registry-free and drive-agnostic, because neither the
  # VS7 key nor msdia140 COM registration exists on a clean VS 2022 install.
  # NEKOMATA_DIA_SDK_ROOT overrides for exotic layouts.
  set(neko_dia_root "")
  if(DEFINED ENV{NEKOMATA_DIA_SDK_ROOT})
    set(neko_dia_root "$ENV{NEKOMATA_DIA_SDK_ROOT}")
  else()
    get_filename_component(neko_dia_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    foreach(neko_level RANGE 8)
      get_filename_component(neko_dia_dir "${neko_dia_dir}" DIRECTORY)
      if(EXISTS "${neko_dia_dir}/DIA SDK/include/dia2.h")
        set(neko_dia_root "${neko_dia_dir}")
        break()
      endif()
    endforeach()
    if(neko_dia_root STREQUAL "")
      foreach(neko_dia_root IN ITEMS
          "C:/Program Files/Microsoft Visual Studio/2022/Community"
          "C:/Program Files/Microsoft Visual Studio/2022/Professional"
          "C:/Program Files/Microsoft Visual Studio/2022/Enterprise"
          "C:/Program Files/Microsoft Visual Studio/2022/BuildTools")
        if(EXISTS "${neko_dia_root}/DIA SDK/include/dia2.h")
          break()
        endif()
        set(neko_dia_root "")
      endforeach()
    endif()
  endif()

  if(NOT neko_dia_root STREQUAL "" AND MSVC)
    set(neko_dia_include "${neko_dia_root}/DIA SDK/include")
    if(NOT EXISTS "${neko_dia_include}/dia2.h")
      set(neko_dia_root "")
    endif()
  endif()

  if(MSVC AND NOT neko_dia_root STREQUAL "")
    # Prefer the product copy of msdia140; the DIA SDK folder is an optional
    # component the toolset host may not have.
    set(NEKO_MSDIA_CANDIDATE "L\"\"")
    foreach(neko_msdia_path IN ITEMS
        "${neko_dia_root}/Common7/IDE/msdia140.dll"
        "${neko_dia_root}/DIA SDK/bin/amd64/msdia140.dll")
      if(EXISTS "${neko_msdia_path}")
        string(REPLACE "\\" "/" neko_msdia_path "${neko_msdia_path}")
        set(NEKO_MSDIA_CANDIDATE "L\"${neko_msdia_path}\"")
        break()
      endif()
    endforeach()

    configure_file(debug/msdia_candidate.hpp.in
      "${PROJECT_BINARY_DIR}/generated/pe/msdia_candidate.hpp" @ONLY)
    # A silent non-substitution once shipped a literal '@' to the compiler;
    # never let that reach a build again.
    file(READ "${PROJECT_BINARY_DIR}/generated/pe/msdia_candidate.hpp" neko_msdia_generated)
    if(neko_msdia_generated MATCHES "@")
      message(FATAL_ERROR "msdia_candidate.hpp.in substitution failed")
    endif()

    add_library(neko_backend_pdb STATIC debug/process_symbols.cpp)
    add_library(nekomata::backends::pdb ALIAS neko_backend_pdb)
    target_include_directories(neko_backend_pdb PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}"
      "${PROJECT_BINARY_DIR}/generated/pe")
    # Third-party COM headers stay outside the warning policy (libdwarf
    # precedent); the path may contain a space.
    target_include_directories(neko_backend_pdb SYSTEM PRIVATE "${neko_dia_include}")
    target_link_libraries(neko_backend_pdb
      PUBLIC neko_runtime
      PRIVATE neko_base_c
        "${neko_dia_root}/DIA SDK/lib/amd64/diaguids.lib"
        ole32 oleaut32)
    nekomata_apply_warnings(neko_backend_pdb)
    nekomata_apply_sanitizers(neko_backend_pdb)
    nekomata_apply_clang_tidy(neko_backend_pdb)
  else()
    # Symbol backend placeholder when no DIA SDK is present (non-MSVC
    # drivers on Windows, or a toolset without the optional component).
    add_library(neko_backend_pdb INTERFACE)
    add_library(nekomata::backends::pdb ALIAS neko_backend_pdb)
  endif()
endif()
