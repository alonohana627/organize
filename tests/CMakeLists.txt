file(GLOB _test_src CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
add_executable(mso_tests ${_test_src})
target_link_libraries(mso_tests PRIVATE mso_app mso::doctest)
mso_apply_options(mso_tests)
add_dependencies(mso_tests mso-worker)

# One CTest entry per test-suite so a failure names its area. doctest filters by suite name.
foreach(suite domain catalog search import media audio analysis export)
  add_test(NAME ${suite} COMMAND mso_tests --test-suite=${suite} --no-intro)
endforeach()
