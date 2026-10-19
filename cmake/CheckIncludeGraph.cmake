# Include-graph check (CTest entry `include-graph`, label `lint`):
#   cmake -DMSO_SOURCE_DIR=<repo> -DMSO_GRAPH_FILE=<build>/mso_include_graph.cmake -P cmake/CheckIncludeGraph.cmake
#
# Every `#include "mso/<module>/..."` (or <...>) in include/mso/<m>/ and src/<m>/ must name module <m>
# itself, a module <m> declares in CMakeLists.txt (directly or transitively), or the generated headers
# (mso/generated/). Every directory under include/mso/ and src/ must be a declared module. tests/ and
# tools/ are exempt. The graph file is written at configure time by mso_finalize_module_graph().
cmake_minimum_required(VERSION 3.28)

if(NOT MSO_SOURCE_DIR OR NOT MSO_GRAPH_FILE)
  message(FATAL_ERROR "usage: cmake -DMSO_SOURCE_DIR=<repo> -DMSO_GRAPH_FILE=<graph> -P CheckIncludeGraph.cmake")
endif()
include("${MSO_GRAPH_FILE}")

set(violations "")
set(checked_files 0)

# Directories that are not declared modules.
foreach(root include/mso src)
  file(GLOB entries LIST_DIRECTORIES true RELATIVE "${MSO_SOURCE_DIR}/${root}" "${MSO_SOURCE_DIR}/${root}/*")
  foreach(entry IN LISTS entries)
    if(IS_DIRECTORY "${MSO_SOURCE_DIR}/${root}/${entry}" AND NOT entry IN_LIST MSO_GRAPH_MODULES
       AND NOT (root STREQUAL "include/mso" AND entry STREQUAL "generated"))
      list(APPEND violations "${root}/${entry}/: not a module declared in CMakeLists.txt")
    endif()
  endforeach()
endforeach()

foreach(module IN LISTS MSO_GRAPH_MODULES)
  set(allowed ${MSO_GRAPH_ALLOWED_${module}} generated)
  file(GLOB_RECURSE files
       "${MSO_SOURCE_DIR}/include/mso/${module}/*.hpp" "${MSO_SOURCE_DIR}/include/mso/${module}/*.h"
       "${MSO_SOURCE_DIR}/src/${module}/*.cpp" "${MSO_SOURCE_DIR}/src/${module}/*.hpp" "${MSO_SOURCE_DIR}/src/${module}/*.h")
  foreach(file IN LISTS files)
    math(EXPR checked_files "${checked_files} + 1")
    file(STRINGS "${file}" includes REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"]mso/")
    foreach(line IN LISTS includes)
      string(REGEX REPLACE "^[ \t]*#[ \t]*include[ \t]*[<\"]mso/([^/>\"]+).*$" "\\1" target "${line}")
      if(NOT target IN_LIST allowed)
        file(RELATIVE_PATH relative "${MSO_SOURCE_DIR}" "${file}")
        string(STRIP "${line}" line)
        list(JOIN MSO_GRAPH_ALLOWED_${module} ", " allowed_text)
        list(APPEND violations "${relative}: ${line} (module '${module}' may include: ${allowed_text})")
      endif()
    endforeach()
  endforeach()
endforeach()

if(violations)
  list(LENGTH violations count)
  list(JOIN violations "\n  " report)
  message(FATAL_ERROR "include-graph: ${count} violation(s):\n  ${report}\n"
                      "Fix the include, or declare the dependency with MODULES in CMakeLists.txt if the layering allows it.")
endif()
message(STATUS "include-graph: ${checked_files} files follow the declared module graph")
