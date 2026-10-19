# `cmake --install <build> --prefix <dir>` produces a self-contained application directory:
#
#   <dir>/bin/MusicSampleOrganizer(.exe), mso(.exe), mso-worker(.exe)   (the worker must sit next to them)
#   Windows: every DLL they need next to them (Qt via windeployqt, plus libc++, FFmpeg and their
#            dependencies from the toolchain), and Qt's plugins where the generated qt.conf points
#            (MSYS2's Qt: <dir>/share/qt6/plugins), so the app starts by double-click without MSYS2.
#   Linux:   Qt libraries and plugins deployed by Qt's deployment script; other libraries come from
#            the system packages.
include(GNUInstallDirs)

set(_mso_installed_targets mso mso-worker)
if(TARGET MusicSampleOrganizer)
  list(APPEND _mso_installed_targets MusicSampleOrganizer)
endif()
install(TARGETS ${_mso_installed_targets} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES LICENSE README.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
if(TARGET MusicSampleOrganizer)
  mso_install_desktop_entry()
endif()

if(TARGET MusicSampleOrganizer)
  # Qt's own deployment (windeployqt on Windows): Qt DLLs, platform/style/image plugins, qt.conf.
  # The application is offline (AGENTS.md invariant 9), so the network-related plugin types (TLS
  # backends, network information, the TUIO touch plugin that pulls in Qt Network) are not shipped.
  set(_mso_deploy_tool_options "")
  if(WIN32)
    set(_mso_deploy_tool_options DEPLOY_TOOL_OPTIONS --skip-plugin-types tls,networkinformation,generic)
  endif()
  qt_generate_deploy_app_script(TARGET MusicSampleOrganizer OUTPUT_SCRIPT _mso_qt_deploy_script
                                NO_TRANSLATIONS NO_UNSUPPORTED_PLATFORM_ERROR ${_mso_deploy_tool_options})
  install(SCRIPT "${_mso_qt_deploy_script}")
endif()

if(WIN32)
  # Everything else the installed executables and Qt plugins load (libc++, libunwind, FFmpeg and its
  # codec libraries, the libraries of Qt's plugins), resolved from the toolchain's bin directory
  # (MSYS2 CLANG64) after the Qt step above. Windows system DLLs are left out.
  get_filename_component(_mso_toolchain_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
  set(_mso_search_directories "${_mso_toolchain_bin}")
  if(TARGET Qt6::Core)
    get_target_property(_mso_qt_core_location Qt6::Core IMPORTED_LOCATION_RELEASE)
    if(NOT _mso_qt_core_location)
      get_target_property(_mso_qt_core_location Qt6::Core LOCATION)
    endif()
    get_filename_component(_mso_qt_bin "${_mso_qt_core_location}" DIRECTORY)
    list(APPEND _mso_search_directories "${_mso_qt_bin}")
  endif()
  install(CODE "
    if(POLICY CMP0207)
      cmake_policy(SET CMP0207 NEW)  # normalize paths before matching the exclusion patterns
    endif()
    set(mso_search_directories [[${_mso_search_directories}]])
    set(mso_bin_directory \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}\")
    file(GLOB mso_executables \"\${mso_bin_directory}/*.exe\")
    file(GLOB_RECURSE mso_libraries \"\${CMAKE_INSTALL_PREFIX}/*.dll\")  # Qt DLLs and plugins
    file(GET_RUNTIME_DEPENDENCIES
      EXECUTABLES \${mso_executables}
      LIBRARIES \${mso_libraries}
      DIRECTORIES \"\${mso_bin_directory}\" \${mso_search_directories}
      RESOLVED_DEPENDENCIES_VAR mso_resolved
      UNRESOLVED_DEPENDENCIES_VAR mso_unresolved
      CONFLICTING_DEPENDENCIES_PREFIX mso_conflicting
      PRE_EXCLUDE_REGEXES [[^api-ms-]] [[^ext-ms-]]
      POST_EXCLUDE_REGEXES [[.*[/\\\\][Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\].*]])
    # A DLL the Qt step already placed in bin/ may also resolve to the toolchain copy (a conflict);
    # the installed copy wins and nothing is copied twice.
    foreach(mso_library IN LISTS mso_resolved mso_conflicting_FILENAMES)
      get_filename_component(mso_library_name \"\${mso_library}\" NAME)
      if(NOT EXISTS \"\${mso_bin_directory}/\${mso_library_name}\")
        if(DEFINED mso_conflicting_\${mso_library})
          list(GET mso_conflicting_\${mso_library} 0 mso_library)
        endif()
        file(INSTALL \"\${mso_library}\" DESTINATION \"\${mso_bin_directory}\")
      endif()
    endforeach()
    if(mso_unresolved)
      message(WARNING \"MSO install: unresolved runtime dependencies: \${mso_unresolved}\")
    endif()
  ")
endif()
