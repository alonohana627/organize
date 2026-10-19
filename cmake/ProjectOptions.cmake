# Compiler options shared by every first-party target. Third-party code under deps/ does not use them.
function(mso_apply_options target)
  target_compile_options(${target} PRIVATE
    -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual
    -Wimplicit-fallthrough -Wno-missing-field-initializers)
  if(MSO_WARNINGS_AS_ERRORS)
    target_compile_options(${target} PRIVATE -Werror)
  endif()
  if(WIN32)
    # Unicode Win32 APIs only, no min/max macros, Windows 10+ API surface (Windows 11 is the target).
    target_compile_definitions(${target} PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
  endif()
endfunction()
