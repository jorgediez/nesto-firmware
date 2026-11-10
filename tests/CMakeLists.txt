add_executable(nesto_tests
  main.cpp
  test_bytes.cpp
  test_crypto.cpp
  test_frame.cpp
  test_input_script.cpp
  test_message.cpp
  test_png.cpp
  test_press_gestures.cpp
)
target_link_libraries(nesto_tests PRIVATE nesto_core nesto_protocol doctest::doctest)
target_compile_definitions(nesto_tests PRIVATE
  NESTO_VECTORS_FILE="${PROJECT_SOURCE_DIR}/tests/vectors/protocol-v1.json"
)
nesto_target_options(nesto_tests)

add_test(NAME nesto_tests COMMAND nesto_tests)
