# nekomata — CMake adapter for managed hot-reload groups.
#
# nekomata_add_reload_unit(<name> SOURCES <source>...)
#
#   One coherent compile configuration for a set of hot sources. Units are
#   not independently publishable; they belong to exactly one group.
#
# nekomata_add_reload_group(<name> SOURCES <source>... [GROUP_ID <id>])
# nekomata_add_reload_group(<name> UNITS <unit>...     [GROUP_ID <id>])
#
# Creates:
#   <name>         For SOURCES: an OBJECT library carrying the hot-flag
#                  contract and the embedded descriptor. For UNITS: an
#                  INTERFACE library injecting every member unit's objects
#                  and the descriptor into its direct consumer.
#   <name>_reload  rebuilds changed inputs through the native graph and
#                  publishes one complete immutable generation.
#
# The same native compile edges feed the baseline link and publication: the
# rule depends on `$<TARGET_OBJECTS:...>` for every member unit, so there is
# no second copy of compile commands to drift.

function(nekomata_add_reload_unit name)
  cmake_parse_arguments(ARG "" "" "SOURCES" ${ARGN})
  if(ARG_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "nekomata_add_reload_unit(${name}): unknown arguments "
      "${ARG_UNPARSED_ARGUMENTS}")
  endif()
  if(NOT ARG_SOURCES)
    message(FATAL_ERROR "nekomata_add_reload_unit(${name}): SOURCES is required")
  endif()
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    message(FATAL_ERROR "nekomata_add_reload_unit(${name}): ${CMAKE_CXX_COMPILER_ID} is not "
      "supported; the hot-flag contract needs Clang or GCC")
  endif()

  add_library(${name} OBJECT ${ARG_SOURCES})
  target_compile_options(${name} PRIVATE
    -O0 -fno-pie -fno-pic -fno-exceptions -fno-asynchronous-unwind-tables)
  set_target_properties(${name} PROPERTIES POSITION_INDEPENDENT_CODE OFF)
  # The group discovers each unit's sources and declaration directory to
  # compute logical member keys and aggregate publication objects.
  set_property(TARGET ${name} PROPERTY NEKOMATA_SOURCES ${ARG_SOURCES})
  set_property(TARGET ${name} PROPERTY NEKOMATA_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
endfunction()

function(nekomata_add_reload_group name)
  cmake_parse_arguments(ARG "" "GROUP_ID" "SOURCES;UNITS" ${ARGN})
  if(ARG_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "nekomata_add_reload_group(${name}): unknown arguments "
      "${ARG_UNPARSED_ARGUMENTS}")
  endif()
  if(ARG_SOURCES AND ARG_UNITS)
    message(FATAL_ERROR "nekomata_add_reload_group(${name}): SOURCES and UNITS are exclusive")
  endif()
  if(NOT ARG_SOURCES AND NOT ARG_UNITS)
    message(FATAL_ERROR "nekomata_add_reload_group(${name}): SOURCES or UNITS is required")
  endif()
  if(NOT TARGET neko_publisher AND NOT TARGET nekomata::host_publisher)
    message(FATAL_ERROR "nekomata_add_reload_group(${name}): the private host publisher "
      "executable is not available; the adapter needs the Nekomata build tree or an "
      "installed nekomata package")
  endif()
  if(TARGET neko_publisher)
    set(neko_publisher_executable "$<TARGET_FILE:neko_publisher>")
  else()
    set(neko_publisher_executable "$<TARGET_FILE:nekomata::host_publisher>")
  endif()

  # Resolve the group identity.
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

  # Collect and validate the member units.
  # SOURCES form creates its own internal unit; UNITS form validates that
  # every unit was declared by nekomata_add_reload_unit.
  if(ARG_SOURCES)
    nekomata_add_reload_unit(${name} SOURCES ${ARG_SOURCES})
    set(member_units "${name}")
  else()
    foreach(unit IN LISTS ARG_UNITS)
      if(NOT TARGET ${unit})
        message(FATAL_ERROR "nekomata_add_reload_group(${name}): unit '${unit}' does not exist")
      endif()
      get_target_property(unit_sources ${unit} NEKOMATA_SOURCES)
      if(NOT unit_sources)
        message(FATAL_ERROR "nekomata_add_reload_group(${name}): '${unit}' was not declared by "
          "nekomata_add_reload_unit")
      endif()
    endforeach()
    set(member_units "${ARG_UNITS}")
  endif()

  # Create the group target.
  # For SOURCES the group target IS the unit (an OBJECT library); for UNITS
  # it is an INTERFACE library injecting every unit's objects plus the
  # descriptor into its direct consumer.
  if(ARG_UNITS)
    add_library(${name} INTERFACE)
    set(all_object_genex "")
    foreach(unit IN LISTS member_units)
      list(APPEND all_object_genex "$<TARGET_OBJECTS:${unit}>")
    endforeach()
    # Interface sources carry the objects; interface link libraries carry
    # the units' usage requirements (include paths, defines) to consumers.
    target_sources(${name} INTERFACE ${all_object_genex})
    target_link_libraries(${name} INTERFACE ${member_units})
  endif()

  # Compute the compatibility fingerprint.
  set(fingerprint "${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}-"
    "${CMAKE_CXX_STANDARD}-${name}")
  foreach(unit IN LISTS member_units)
    get_target_property(unit_sources ${unit} NEKOMATA_SOURCES)
    list(APPEND fingerprint ";${unit}:${unit_sources}")
  endforeach()
  string(SHA256 compat_digest "${fingerprint}")
  string(SUBSTRING "${compat_digest}" 0 12 compat12)
  set(publication_key "${name}-${compat12}-$<CONFIG>")
  set(compat_id "cmake:${compat_digest}-$<CONFIG>")
  set(abi_id "elf-${CMAKE_SYSTEM_PROCESSOR}-patch-v1")
  set(generation_root "${CMAKE_BINARY_DIR}/nekomata")

  # Build member keys and the request-file arguments.
  # Iterate every unit's sources in declaration order to build the ordered
  # member list the descriptor and manifest require.
  set(member_keys "")
  set(request_member_arguments "")
  set(descriptor_member_arguments "")
  set(publication_objects "")
  set(publication_depends "")
  foreach(unit IN LISTS member_units)
    get_target_property(unit_sources ${unit} NEKOMATA_SOURCES)
    get_target_property(unit_dir ${unit} NEKOMATA_SOURCE_DIR)
    foreach(source IN LISTS unit_sources)
      cmake_path(ABSOLUTE_PATH source NORMALIZE BASE_DIRECTORY "${unit_dir}")
      file(RELATIVE_PATH member_key "${unit_dir}" "${source}")
      if(member_key MATCHES "^\\..")
        set(member_key "${source}")
      endif()
      if(member_key MATCHES "[\\\\\"]")
        message(FATAL_ERROR "nekomata_add_reload_group(${name}): member key '${member_key}' "
          "must not contain backslashes or quotes")
      endif()
      if(source MATCHES "[\r\n]")
        message(FATAL_ERROR "nekomata_add_reload_group(${name}): source paths must not "
          "contain newlines")
      endif()
      list(APPEND member_keys "${member_key}")
      string(APPEND descriptor_member_arguments
        "--member\n${member_key}\n")
      string(APPEND request_member_arguments
        "--member\n${member_key}\n${source}\n-O0 hot contract $<CONFIG>\n")
    endforeach()
    list(APPEND publication_objects "$<TARGET_OBJECTS:${unit}>")
    list(APPEND publication_depends "${unit}")
  endforeach()

  foreach(request_value IN ITEMS
      "${generation_root}" "${publication_key}" "${group_id}" "${compat_id}" "${abi_id}")
    if(request_value MATCHES "[\r\n]")
      message(FATAL_ERROR "nekomata_add_reload_group(${name}): request fields must not "
        "contain newlines")
    endif()
  endforeach()

  set(request_directory "${CMAKE_BINARY_DIR}/nekomata-generators/${name}")
  file(MAKE_DIRECTORY "${request_directory}")
  set(request_file "${request_directory}/publish_$<CONFIG>.request")
  string(CONCAT request_content
    "nekomata-publisher-request/1\n"
    "--root\n${generation_root}\n"
    "--key\n${publication_key}\n"
    "--group\n${group_id}\n"
    "--compat\n${compat_id}\n"
    "--abi\n${abi_id}\n")
  string(APPEND request_content
    "${request_member_arguments}--objects\n$<JOIN:${publication_objects},\n>\n")
  file(GENERATE OUTPUT "${request_file}" CONTENT "${request_content}")

  # Generate the embedded descriptor translation unit.
  set(descriptor_tu "${CMAKE_BINARY_DIR}/nekomata-generators/${name}/descriptor_$<CONFIG>.cpp")
  set(descriptor_request "${request_directory}/descriptor_$<CONFIG>.request")
  string(CONCAT descriptor_request_content
    "nekomata-publisher-request/1\n"
    "--output\n${descriptor_tu}\n"
    "--group\n${group_id}\n"
    "--key\n${publication_key}\n"
    "--compat\n${compat_id}\n"
    "--abi\n${abi_id}\n"
    "--root\n${generation_root}\n")
  string(APPEND descriptor_request_content "${descriptor_member_arguments}")
  file(GENERATE OUTPUT "${descriptor_request}" CONTENT "${descriptor_request_content}")
  add_custom_command(
    OUTPUT "${descriptor_tu}"
    COMMAND ${neko_publisher_executable} descriptor --request "${descriptor_request}"
    DEPENDS ${neko_publisher_executable} "${descriptor_request}"
    VERBATIM)
  add_library(${name}_neko_descriptor OBJECT "${descriptor_tu}")
  set_target_properties(${name}_neko_descriptor PROPERTIES POSITION_INDEPENDENT_CODE OFF)
  # Object libraries do not reliably propagate their files through another
  # target's link interface; carry them explicitly as interface sources so
  # every consumer links the descriptor without it joining the publication
  # object set.
  target_sources(${name} INTERFACE
    "$<TARGET_OBJECTS:${name}_neko_descriptor>")

  # Publish immutable generations.
  set(stamp "${generation_root}/$<CONFIG>/${name}.reload.stamp")
  add_custom_command(
    OUTPUT "${stamp}"
    COMMAND ${neko_publisher_executable} --request "${request_file}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${generation_root}/$<CONFIG>"
    COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
    DEPENDS ${neko_publisher_executable} "${request_file}" ${publication_objects}
    VERBATIM)
  add_custom_target(${name}_reload DEPENDS "${stamp}")
  # File-level dependencies decide staleness; the target-level edge is what
  # makes the Unix Makefiles generator rebuild the objects at all.
  add_dependencies(${name}_reload ${publication_depends})
endfunction()
