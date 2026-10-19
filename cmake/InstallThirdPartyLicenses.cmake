# Install-time script (Windows, MSYS2 toolchain): ships the license texts of the prebuilt libraries
# that `cmake --install` copied next to the application (Qt, FFmpeg and its codec libraries, libc++,
# libunwind, ...). cmake/Install.cmake runs it after every DLL is in place, so the install tree, and
# the MSI built from it, carry the licenses of exactly the DLLs they contain.
#
# Inputs (set by the install(CODE) call in cmake/Install.cmake):
#   MSO_TOOLCHAIN_PREFIX   the MSYS2 environment prefix the DLLs come from (for example C:/msys64/clang64)
#   MSO_PACMAN             pacman.exe of that MSYS2 installation (empty when not found)
#   MSO_LICENSE_DIRECTORY  absolute destination, <prefix>/share/licenses (the MSYS2 layout; a deeper
#                          folder pushes Qt's longest license file names past WiX's 260-character limit)
#
# Output: <package>/ (a copy of each package's share/licenses/<package>/) and PACKAGES.txt (every package with its version and SPDX license, and where its text is).

# THIRD_PARTY_NOTICES.txt points every reader at PACKAGES.txt, so it is written on every path, also when
# the license files cannot be collected.
function(mso_write_package_summary summary)
  file(MAKE_DIRECTORY "${MSO_LICENSE_DIRECTORY}")
  file(WRITE "${MSO_LICENSE_DIRECTORY}/PACKAGES.txt" "${summary}")
  list(APPEND CMAKE_INSTALL_MANIFEST_FILES "${MSO_LICENSE_DIRECTORY}/PACKAGES.txt")
  set(CMAKE_INSTALL_MANIFEST_FILES "${CMAKE_INSTALL_MANIFEST_FILES}" PARENT_SCOPE)
endfunction()

if(NOT MSO_PACMAN OR NOT EXISTS "${MSO_PACMAN}")
  message(WARNING "MSO install: pacman not found next to the toolchain; the license texts of the shipped "
                  "DLLs are not installed. Build the installer from MSYS2 CLANG64 (docs/BUILDING.md).")
  set(mso_summary "The license files of the prebuilt libraries shipped with Music Sample Organizer could not be\n")
  string(APPEND mso_summary "collected for this installation (it was not built from MSYS2 CLANG64). The libraries are MSYS2\n")
  string(APPEND mso_summary "CLANG64 packages; their sources and licenses are at https://repo.msys2.org/mingw/sources/\n")
  string(APPEND mso_summary "and https://packages.msys2.org/.\n")
  mso_write_package_summary("${mso_summary}")
  return()
endif()

# The install layout mirrors the MSYS2 prefix (bin/*.dll, share/qt6/plugins/...), so each installed DLL
# has its origin at the same relative path under the toolchain prefix.
file(GLOB_RECURSE mso_installed_libraries RELATIVE "${CMAKE_INSTALL_PREFIX}" "${CMAKE_INSTALL_PREFIX}/*.dll")
set(mso_origins "")
foreach(mso_library IN LISTS mso_installed_libraries)
  if(EXISTS "${MSO_TOOLCHAIN_PREFIX}/${mso_library}")
    list(APPEND mso_origins "${MSO_TOOLCHAIN_PREFIX}/${mso_library}")
  endif()
endforeach()
if(NOT mso_origins)
  mso_write_package_summary("This installation ships no prebuilt library from MSYS2 packages.\n")
  return()
endif()

# pacman's field labels ("Name", "Version", "Licenses") are translated under a non-English locale; the
# parsing below needs the English ones.
set(ENV{LC_ALL} C)
set(ENV{LANG} C)
execute_process(COMMAND "${MSO_PACMAN}" -Qoq ${mso_origins}
                OUTPUT_VARIABLE mso_owners ERROR_VARIABLE mso_owner_errors RESULT_VARIABLE mso_result)
if(NOT mso_result EQUAL 0)
  message(WARNING "MSO install: pacman could not name the package of every shipped DLL: ${mso_owner_errors}")
endif()
string(REGEX REPLACE "[\r\n]+" ";" mso_packages "${mso_owners}")
list(FILTER mso_packages EXCLUDE REGEX "^$")
list(REMOVE_DUPLICATES mso_packages)
list(SORT mso_packages)

execute_process(COMMAND "${MSO_PACMAN}" -Qi ${mso_packages}
                OUTPUT_VARIABLE mso_information RESULT_VARIABLE mso_result)
if(NOT mso_result EQUAL 0)
  message(WARNING "MSO install: pacman -Qi failed; package versions and licenses are not recorded")
endif()
# One "Name/Version/Licenses" triple per package, in the order pacman prints them.
string(REGEX MATCHALL "Name +: [^\r\n]+" mso_names "${mso_information}")
string(REGEX MATCHALL "Version +: [^\r\n]+" mso_versions "${mso_information}")
string(REGEX MATCHALL "Licenses +: [^\r\n]+" mso_licenses "${mso_information}")

set(mso_summary "Prebuilt libraries shipped with Music Sample Organizer (MSYS2 CLANG64 packages)\n\n")
string(APPEND mso_summary "Each package's own license files are in the directory of the same name next to this file.\n")
string(APPEND mso_summary "Where a package ships none, its SPDX license applies; GPL-3.0 is the text in ../doc/MusicSampleOrganizer/LICENSE, other\n")
string(APPEND mso_summary "SPDX texts are at https://spdx.org/licenses/. Sources: https://repo.msys2.org/mingw/sources/\n\n")
list(LENGTH mso_names mso_count)
if(mso_count GREATER 0)
  math(EXPR mso_last "${mso_count} - 1")
  foreach(mso_index RANGE ${mso_last})
    list(GET mso_names ${mso_index} mso_name)
    string(REGEX REPLACE "^Name +: " "" mso_name "${mso_name}")
    set(mso_version "unknown")
    set(mso_license "unknown")
    list(LENGTH mso_versions mso_version_count)
    if(mso_index LESS mso_version_count)
      list(GET mso_versions ${mso_index} mso_version)
      string(REGEX REPLACE "^Version +: " "" mso_version "${mso_version}")
    endif()
    list(LENGTH mso_licenses mso_license_count)
    if(mso_index LESS mso_license_count)
      list(GET mso_licenses ${mso_index} mso_license)
      string(REGEX REPLACE "^Licenses +: " "" mso_license "${mso_license}")
      string(REPLACE "spdx:" "" mso_license "${mso_license}")
    endif()
    # mingw-w64-clang-x86_64-qt6-base -> qt6-base, the name of its share/licenses directory.
    string(REGEX REPLACE "^mingw-w64-(clang-|ucrt-)?(x86_64|aarch64|i686)-" "" mso_short_name "${mso_name}")
    set(mso_texts "(no license files in the package)")
    if(IS_DIRECTORY "${MSO_TOOLCHAIN_PREFIX}/share/licenses/${mso_short_name}")
      file(INSTALL "${MSO_TOOLCHAIN_PREFIX}/share/licenses/${mso_short_name}" DESTINATION "${MSO_LICENSE_DIRECTORY}")
      set(mso_texts "${mso_short_name}/")
    endif()
    string(APPEND mso_summary "${mso_name} ${mso_version}\n    License: ${mso_license}\n    Texts: ${mso_texts}\n")
  endforeach()
endif()
mso_write_package_summary("${mso_summary}")
