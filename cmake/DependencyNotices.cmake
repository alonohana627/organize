# Generates, from deps/manifest.json at configure time (no network):
#   - mso/generated/dependencies.hpp, exposed as the INTERFACE target mso::generated. The header lists
#     every shipped vendored dependency with its license text (MIT and BSD require the notice to ship
#     with the program) plus the system libraries this build links (Qt, FFmpeg), so the About box can
#     never drift from the manifest;
#   - THIRD_PARTY_NOTICES.txt in the build directory, the same notices as a text file that
#     cmake/Install.cmake installs next to LICENSE (and so into the MSI), with where to get the source
#     of the system libraries.
# Dependencies marked "test_only" (doctest) are linked into the tests only and are left out of both.

# Escapes `text` for use inside a C++ "..." string literal.
function(_mso_cpp_string_literal text output)
  string(REPLACE "\\" "\\\\" text "${text}")
  string(REPLACE "\"" "\\\"" text "${text}")
  set(${output} "\"${text}\"" PARENT_SCOPE)
endfunction()

# Appends one DependencyNotice initializer to `content_variable`.
function(_mso_append_notice content_variable name version license purpose url license_text system_library)
  foreach(field name version license purpose url)
    _mso_cpp_string_literal("${${field}}" ${field}_literal)
  endforeach()
  string(REPLACE "\r" "" license_text "${license_text}")
  if(license_text MATCHES "\\)mso_license\"")
    message(FATAL_ERROR "License text of ${name} contains the raw-string delimiter")
  endif()
  set(entry "    {${name_literal}, ${version_literal}, ${license_literal},\n")
  string(APPEND entry "     ${purpose_literal},\n")
  string(APPEND entry "     ${url_literal},\n")
  if(license_text STREQUAL "")
    string(APPEND entry "     {},\n")
  else()
    string(APPEND entry "     R\"mso_license(${license_text})mso_license\",\n")
  endif()
  string(APPEND entry "     ${system_library}},\n")
  set(${content_variable} "${${content_variable}}${entry}" PARENT_SCOPE)
endfunction()

# mso_generate_dependency_notices(SYSTEM_VERSIONS <name> <version> ...)
#   SYSTEM_VERSIONS: for each system library of the manifest that this build links, its name and the
#   version found at configure time. System libraries not listed are left out of the header.
function(mso_generate_dependency_notices)
  cmake_parse_arguments(PARSE_ARGV 0 ARG "" "" "SYSTEM_VERSIONS")
  set(manifest_path "${PROJECT_SOURCE_DIR}/deps/manifest.json")
  file(READ "${manifest_path}" manifest)
  set_property(DIRECTORY "${PROJECT_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${manifest_path}")

  set(entries "")
  set(notices "Music Sample Organizer ${PROJECT_VERSION}: third-party notices
")
  string(APPEND notices "
Music Sample Organizer is free software under the GNU General Public License v3 (LICENSE).
")
  string(APPEND notices "It contains or links the components below; each keeps its own license.
")
  string(JSON dependency_count LENGTH "${manifest}" dependencies)
  math(EXPR last "${dependency_count} - 1")
  foreach(index RANGE ${last})
    foreach(field name version license purpose homepage)
      string(JSON ${field} GET "${manifest}" dependencies ${index} ${field})
    endforeach()
    string(JSON test_only ERROR_VARIABLE no_test_only GET "${manifest}" dependencies ${index} test_only)
    if(NOT no_test_only AND test_only)
      continue()  # linked into the tests only, never shipped
    endif()
    string(JSON license_file ERROR_VARIABLE no_license_file GET "${manifest}" dependencies ${index} license_file)
    set(license_text "")
    if(NOT no_license_file)
      set(license_path "${PROJECT_SOURCE_DIR}/deps/${license_file}")
      if(NOT EXISTS "${license_path}")
        message(FATAL_ERROR "deps/manifest.json: ${name} license_file '${license_file}' does not exist")
      endif()
      file(READ "${license_path}" license_text)
      set_property(DIRECTORY "${PROJECT_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${license_path}")
    endif()
    _mso_append_notice(entries "${name}" "${version}" "${license}" "${purpose}" "${homepage}" "${license_text}" false)
    string(REPLACE "\r" "" license_text "${license_text}")
    string(APPEND notices "
================================================================================
")
    string(APPEND notices "${name} ${version} (${license}), ${homepage}
Used for: ${purpose}
")
    if(license_text STREQUAL "")
      string(APPEND notices "
No license text: the work is in the public domain.
")
    else()
      string(APPEND notices "
${license_text}
")
    endif()
  endforeach()

  string(JSON system_count LENGTH "${manifest}" system_dependencies)
  math(EXPR last "${system_count} - 1")
  foreach(index RANGE ${last})
    foreach(field name license purpose homepage)
      string(JSON ${field} GET "${manifest}" system_dependencies ${index} ${field})
    endforeach()
    list(FIND ARG_SYSTEM_VERSIONS "${name}" position)
    if(position GREATER_EQUAL 0)
      math(EXPR version_position "${position} + 1")
      list(GET ARG_SYSTEM_VERSIONS ${version_position} version)
      _mso_append_notice(entries "${name}" "${version}" "${license}" "${purpose}" "${homepage}" "" true)
      string(JSON source ERROR_VARIABLE no_source GET "${manifest}" system_dependencies ${index} source)
      string(APPEND notices "
================================================================================
")
      string(APPEND notices "${name} ${version} (${license}), ${homepage}
Used for: ${purpose}
")
      string(APPEND notices "
Shipped as unmodified shared libraries (DLLs) that can be replaced.
")
      if(NOT no_source)
        string(APPEND notices "Source code: ${source}
")
      endif()
      string(APPEND notices "On Windows, the license files of the packages these DLLs come from are in share/licenses/ of the installation folder (PACKAGES.txt there lists every package).
")
    endif()
  endforeach()
  # Written only when it changed, so a re-configure does not touch the file (and the install step).
  set(notices_path "${PROJECT_BINARY_DIR}/THIRD_PARTY_NOTICES.txt")
  set(previous_notices "")
  if(EXISTS "${notices_path}")
    file(READ "${notices_path}" previous_notices)
  endif()
  if(NOT previous_notices STREQUAL notices)
    file(WRITE "${notices_path}" "${notices}")
  endif()

  set(MSO_DEPENDENCY_ENTRIES "${entries}")
  set(MSO_PROJECT_VERSION "${PROJECT_VERSION}")
  set(output_directory "${PROJECT_BINARY_DIR}/generated")
  configure_file("${PROJECT_SOURCE_DIR}/cmake/dependencies.hpp.in"
                 "${output_directory}/mso/generated/dependencies.hpp" @ONLY)

  add_library(mso_generated INTERFACE)
  target_include_directories(mso_generated INTERFACE "${output_directory}")
  add_library(mso::generated ALIAS mso_generated)
endfunction()
