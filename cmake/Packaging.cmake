# Windows installer (.msi) through CPack's WIX generator, built from the same install rules as
# `cmake --install` (cmake/Install.cmake), so the package holds the app, the CLI, the worker, every DLL
# they load and Qt's plugins.
#
#   cmake --preset release && cmake --build --preset release
#   cpack --config build/release/CPackConfig.cmake -B build/release/package
#
# Needs the WiX Toolset 3.x (candle.exe and light.exe on PATH, or WIX set to its folder); see
# docs/BUILDING.md. The installer is per machine: Program Files\Music Sample Organizer, a Start
# menu shortcut and a desktop shortcut. Installing a newer version replaces the older one.
if(NOT WIN32)
  return()
endif()

set(CPACK_GENERATOR WIX)
set(CPACK_PACKAGE_NAME "Music Sample Organizer")
set(CPACK_PACKAGE_VENDOR "Music Sample Organizer contributors")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Offline sample library browser for producers and sound designers")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Music Sample Organizer")
set(CPACK_PACKAGE_FILE_NAME "MusicSampleOrganizer-${PROJECT_VERSION}-windows-x64")
# Start menu entry (executable name; label) and the same shortcut on the desktop.
set(CPACK_PACKAGE_EXECUTABLES "MusicSampleOrganizer;Music Sample Organizer")
set(CPACK_CREATE_DESKTOP_LINKS "MusicSampleOrganizer")

# Never change the upgrade code: Windows uses it to recognise every version as the same product, so a
# newer installer removes the older one instead of installing side by side.
set(CPACK_WIX_UPGRADE_GUID "B9FC9D4D-C5D4-4681-A4F5-737057B2A14B")
set(CPACK_WIX_PRODUCT_ICON "${PROJECT_SOURCE_DIR}/resources/icons/music_sample_organizer.ico")
set(CPACK_WIX_PROGRAM_MENU_FOLDER "Music Sample Organizer")
set(CPACK_WIX_PROPERTY_ARPURLINFOABOUT "https://github.com/alonohana627/organize")
set(CPACK_WIX_ARCHITECTURE x64)
# CPack's WiX license dialog needs RTF; it converts a plain-text license itself but keeps its .txt
# name check, so hand it a copy with a .txt extension.
configure_file("${PROJECT_SOURCE_DIR}/LICENSE" "${PROJECT_BINARY_DIR}/LICENSE.txt" COPYONLY)
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_BINARY_DIR}/LICENSE.txt")

include(CPack)
