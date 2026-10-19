# Application icon wiring. The icon files are generated from resources/icons/music_sample_organizer.svg
# by tools/render_icon.py and committed, so the build needs no SVG renderer.

set(MSO_ICON_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/../resources/icons")
cmake_path(NORMAL_PATH MSO_ICON_DIRECTORY)

## mso_add_executable_icon(<target>)
# Embeds the .ico in a Windows executable (Explorer, taskbar, shortcuts). No effect elsewhere: Linux
# desktops take the icon from the installed .desktop entry instead (see mso_install_desktop_entry).
function(mso_add_executable_icon target)
  if(NOT WIN32)
    return()
  endif()
  set(MSO_APPLICATION_ICON_FILE "${MSO_ICON_DIRECTORY}/music_sample_organizer.ico")
  set(_resource_script "${CMAKE_CURRENT_BINARY_DIR}/${target}_icon.rc")
  configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../resources/windows/application_icon.rc.in"
                 "${_resource_script}" @ONLY)
  target_sources(${target} PRIVATE "${_resource_script}")
endfunction()

## mso_add_window_icon_resources(<target>)
# Compiles the PNG sizes into the Qt resource system under :/icons/, for QApplication::setWindowIcon.
function(mso_add_window_icon_resources target)
  file(GLOB _icon_files CONFIGURE_DEPENDS "${MSO_ICON_DIRECTORY}/png/music_sample_organizer_*.png")
  qt_add_resources(${target} "application_icons" PREFIX "/icons" BASE "${MSO_ICON_DIRECTORY}/png"
                   FILES ${_icon_files})
endfunction()

## mso_install_desktop_entry()
# Linux: installs the .desktop entry and the hicolor theme icons so menus and docks show the app.
function(mso_install_desktop_entry)
  if(WIN32 OR APPLE)
    return()
  endif()
  install(FILES "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../resources/linux/music-sample-organizer.desktop"
          DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
  foreach(size 16 24 32 48 64 128 256)
    install(FILES "${MSO_ICON_DIRECTORY}/png/music_sample_organizer_${size}.png"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps"
            RENAME "music-sample-organizer.png")
  endforeach()
  install(FILES "${MSO_ICON_DIRECTORY}/music_sample_organizer.svg"
          DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps" RENAME "music-sample-organizer.svg")
endfunction()
