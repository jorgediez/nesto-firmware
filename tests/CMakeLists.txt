add_executable(nesto_tests
  main.cpp
  test_backplate.cpp
  test_bytes.cpp
  test_controller.cpp
  test_fan.cpp
  test_lock.cpp
  test_fonts.cpp
  test_crypto.cpp
  test_display_power.cpp
  test_frame.cpp
  test_input_script.cpp
  test_mdns.cpp
  test_message.cpp
  test_panel_session.cpp
  test_png.cpp
  test_press_gestures.cpp
  test_ring_ticks.cpp
  test_probe.cpp
  test_room.cpp
  test_settings.cpp
  test_standby_clock.cpp
  test_auto_brightness.cpp
  test_sensors.cpp
  test_notifications.cpp
  test_sound.cpp
  test_tech_info.cpp
  test_wireless.cpp
  test_ring_keyboard.cpp
  test_ota.cpp
  test_wifi.cpp
  test_wifi_setup.cpp
)
target_link_libraries(nesto_tests PRIVATE nesto_core nesto_protocol nesto_net nesto_room nesto_fonts nesto_probe_checks
                                          doctest::doctest)
target_compile_definitions(nesto_tests PRIVATE
  NESTO_VECTORS_FILE="${PROJECT_SOURCE_DIR}/tests/vectors/protocol-v1.json"
)
nesto_target_options(nesto_tests)

add_test(NAME nesto_tests COMMAND nesto_tests)
