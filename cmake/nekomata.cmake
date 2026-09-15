# nekomata — CMake adapter for managed hot-reload groups.
#
# nekomata_add_reload_group(<name> SOURCES <source>... [GROUP_ID <id>])
#
# Creates:
#   <name>         OBJECT library: baseline objects, hot-flag contract, and
#                  the embedded group descriptor for its consumers.
#   <name>_reload  rebuilds changed inputs through the native graph and
#                  publishes one complete immutable generation.
#
# The same native compile edge feeds the baseline link and publication: the
# rule depends on `$<TARGET_OBJECTS:<name>>`, so there is no second copy of
# compile commands to drift.

function(nekomata_add_reload_group name)
  cmake_parse_arguments(ARG "" "GROUP_ID" "SOURCES;UNITS" ${ARGN})
  if(ARG_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "nekomata_add_reload_group(${name}): unknown arguments "
      "${ARG_UNPARSED_ARGUMENTS}")
  endif()
  if(ARG_SOURCES AND ARG_UNITS)
    message(FATAL_ERROR "nekomata_add_reload_group(${name}): SOURCES and UNITS are exclusive")
  endif()
  if(ARG_UNITS)
    message(FATAL_ERROR "nekomata_add_reload_group(${name}): the UNITS form arrives with "
      "heterogeneous group support; group sources through one configuration for now")
  endif()
  if(NOT ARG_SOURCES)
    message(FATAL_ERROR "nekomata_add_reload_group(${name}): SOURCES is required")
  endif()
  if(NOT TARGET neko_publisher)
    message(FATAL_ERROR "nekomata_add_reload_group(${name}): the private neko_publisher "
      "executable is not available; the adapter needs the Nekomata build tree")
  endif()
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    message(FATAL_ERROR "nekomata_add_reload_group(${name}): ${CMAKE_CXX_COMPILER_ID} is not "
      "supported; the hot-flag contract needs Clang or GCC")
  endif()

  # ---- identity ------------------------------------------------------------
  if(ARG_GROUP_ID)
    set(group_id "${ARG_GROUP_ID}")
  else()
    file(RELATIVE_PATH group_dir "${CMAKE_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
    set(group_id "//${group_dir}:${name}")
  endif()
  if(group_id MATCHES "[\\\\\"]")
    message(FATAL_ERROR "nekomata_add_reload_group(${name}): group IDs must not contain "
      "backslashes or quotes")
  endif()

  # ---- baseline objects with the hot-flag contract -------------------------
  add_library(${name} OBJECT ${ARG_SOURCES})
  target_compile_options(${name} PRIVATE
    -O0 -fno-pie -fno-pic -fno-exceptions -fno-asynchronous-unwind-tables)
  set_target_properties(${name} PROPERTIES POSITION_INDEPENDENT_CODE OFF)

  set(fingerprint "${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}-"
    "${CMAKE_CXX_STANDARD}-${name}-${ARG_SOURCES}")
  string(SHA256 compat_digest "${fingerprint}")
  string(SUBSTRING "${compat_digest}" 0 12 compat12)
  # $<CONFIG> joins through file(GENERATE) and the command line, keeping
  # debug and optimized streams separate without hashing at configure time.
  set(publication_key "${name}-${compat12}-$<CONFIG>")
  set(compat_id "cmake:${compat_digest}-$<CONFIG>")
  set(abi_id "elf-${CMAKE_SYSTEM_PROCESSOR}-patch-v1")
  set(generation_root "${CMAKE_BINARY_DIR}/nekomata")

  # ---- the embedded descriptor TU ------------------------------------------
  # neko_publisher emits the framed payload as a numeric byte array, so no
  # escaping or length arithmetic lives in CMake.
  set(member_keys "")
  set(member_args "")
  foreach(source IN LISTS ARG_SOURCES)
    cmake_path(ABSOLUTE_PATH source NORMALIZE BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    file(RELATIVE_PATH member_key "${CMAKE_CURRENT_SOURCE_DIR}" "${source}")
    if(member_key MATCHES "^\\..")
      set(member_key "${source}")
    endif()
    if(member_key MATCHES "[\\\\\"]")
      message(FATAL_ERROR "nekomata_add_reload_group(${name}): member key '${member_key}' must "
        "not contain backslashes or quotes")
    endif()
    list(APPEND member_keys "${member_key}")
    list(APPEND member_args --member "${member_key}" "${source}"
      "-O0 hot contract $<CONFIG>")
  endforeach()

  set(descriptor_tu "${CMAKE_BINARY_DIR}/nekomata-generators/${name}/descriptor_$<CONFIG>.cpp")
  set(descriptor_args --group "${group_id}" --key "${publication_key}"
    --compat "${compat_id}" --abi "${abi_id}" --root "${generation_root}")
  foreach(member_key IN LISTS member_keys)
    list(APPEND descriptor_args --member "${member_key}")
  endforeach()
  add_custom_command(
    OUTPUT "${descriptor_tu}"
    COMMAND $<TARGET_FILE:neko_publisher> descriptor --output "${descriptor_tu}"
      ${descriptor_args}
    DEPENDS neko_publisher
    VERBATIM)
  add_library(${name}_neko_descriptor OBJECT "${descriptor_tu}")
  set_target_properties(${name}_neko_descriptor PROPERTIES POSITION_INDEPENDENT_CODE OFF)
  # Object libraries do not reliably propagate their files through another
  # target's link interface; carry them explicitly as interface sources so
  # every consumer links the descriptor without it joining the publication
  # object set of $<TARGET_OBJECTS:${name}>.
  target_sources(${name} INTERFACE
    "$<TARGET_OBJECTS:${name}_neko_descriptor>")

  # ---- publication ----------------------------------------------------------
  set(stamp "${generation_root}/$<CONFIG>/${name}.reload.stamp")
  add_custom_command(
    OUTPUT "${stamp}"
    COMMAND $<TARGET_FILE:neko_publisher>
      --root "${generation_root}"
      --key "${publication_key}"
      --group "${group_id}"
      --compat "${compat_id}"
      --abi "${abi_id}"
      ${member_args}
      --objects $<TARGET_OBJECTS:${name}>
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${generation_root}/$<CONFIG>"
    COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
    DEPENDS neko_publisher $<TARGET_OBJECTS:${name}>
    COMMAND_EXPAND_LISTS VERBATIM)
  add_custom_target(${name}_reload DEPENDS "${stamp}")
  # File-level dependencies decide staleness; the target-level edge is what
  # makes the Unix Makefiles generator rebuild the objects at all.
  add_dependencies(${name}_reload ${name})
endfunction()
