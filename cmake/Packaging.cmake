# Windows installer (.msi) through CPack's WIX generator, built from the same install rules as
# `cmake --install` (cmake/Install.cmake), so the package holds the app, the CLI, the worker, every DLL
# they load and Qt's plugins.
#
#   cmake --preset release && cmake --build --preset release
#   cpack --config build/release/CPackConfig.cmake -B build/release/package
#
# Needs the WiX Toolset 3.x (candle.exe and light.exe on PATH, or WIX set to its folder); see
# docs/BUILDING.md. The installer is per machine (CPACK_WIX_INSTALL_SCOPE below): Program Files\Music
# Sample Organizer, a Start menu shortcut and a desktop shortcut for all users, and one uninstall entry.
# It asks for administrator rights. Installing a newer version replaces the older one. The package also
# carries LICENSE, THIRD_PARTY_NOTICES.txt and the license texts of every bundled DLL
# (cmake/Install.cmake).
if(NOT WIN32)
  return()
endif()
# CPACK_WIX_INSTALL_SCOPE (below) exists from CMake 3.29; an older CMake ignores it silently and builds an
# installer that registers its shortcuts and uninstaller for the installing user only. No installer
# rather than that one: configuring still works, but `cpack` finds no CPackConfig.cmake.
if(CMAKE_VERSION VERSION_LESS 3.29)
  message(WARNING "MSO: the Windows installer needs CMake 3.29 or newer (this is ${CMAKE_VERSION}); "
                  "packaging is disabled for this build")
  return()
endif()

set(CPACK_GENERATOR WIX)
set(CPACK_PACKAGE_NAME "Music Sample Organizer")
set(CPACK_PACKAGE_VENDOR "Music Sample Organizer contributors")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Offline sample library browser for producers and sound designers")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Music Sample Organizer")
set(CPACK_PACKAGE_FILE_NAME "MusicSampleOrganizer-${PROJECT_VERSION}-windows-x64")
# Start menu entry (executable name; label) and the same shortcut on the desktop, when the desktop
# application is part of this build (a GUI-less build packages the command-line tools only).
if(TARGET MusicSampleOrganizer)
  set(CPACK_PACKAGE_EXECUTABLES "MusicSampleOrganizer;Music Sample Organizer")
  set(CPACK_CREATE_DESKTOP_LINKS "MusicSampleOrganizer")
endif()

# Never change the upgrade code: Windows uses it to recognise every version as the same product, so a
# newer installer removes the older one instead of installing side by side.
set(CPACK_WIX_UPGRADE_GUID "B9FC9D4D-C5D4-4681-A4F5-737057B2A14B")
# Per machine, set explicitly: with cmake_minimum_required(3.28) policy CMP0172 (3.31) is unset, and the
# WiX 3 generator's default scope is then NONE, which installs into Program Files but registers the
# shortcuts and the uninstaller for the installing user only. The scope is part of the upgrade
# identity, so it must not change after the first released installer.
set(CPACK_WIX_INSTALL_SCOPE perMachine)
set(CPACK_WIX_PRODUCT_ICON "${PROJECT_SOURCE_DIR}/resources/icons/music_sample_organizer.ico")
set(CPACK_WIX_PROGRAM_MENU_FOLDER "Music Sample Organizer")
set(CPACK_WIX_PROPERTY_ARPURLINFOABOUT "https://github.com/alonohana627/organize")
set(CPACK_WIX_ARCHITECTURE x64)
# CPack's WiX license dialog needs RTF; it converts a plain-text license itself but keeps its .txt
# name check, so hand it a copy with a .txt extension.
configure_file("${PROJECT_SOURCE_DIR}/LICENSE" "${PROJECT_BINARY_DIR}/LICENSE.txt" COPYONLY)
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_BINARY_DIR}/LICENSE.txt")

include(CPack)
