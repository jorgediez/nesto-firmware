add_executable(nesto_tests
  main.cpp
  test_input_script.cpp
  test_png.cpp
  test_press_gestures.cpp
)
target_link_libraries(nesto_tests PRIVATE nesto_core doctest::doctest)
nesto_target_options(nesto_tests)

add_test(NAME nesto_tests COMMAND nesto_tests)
