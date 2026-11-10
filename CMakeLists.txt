cmake_minimum_required(VERSION 3.22)

project(nesto VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 99)

# sim:  desktop simulator, a window through SDL2 (when found) or headless.
# nest: the thermostat itself, drawing to /dev/fb0.
set(NESTO_PLATFORM "sim" CACHE STRING "Hardware back end: sim or nest")
set_property(CACHE NESTO_PLATFORM PROPERTY STRINGS sim nest)
option(NESTO_BUILD_TESTS "Build the unit tests" ON)
option(NESTO_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

if(NOT NESTO_PLATFORM MATCHES "^(sim|nest)$")
  message(FATAL_ERROR "NESTO_PLATFORM must be sim or nest, not '${NESTO_PLATFORM}'")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

include(cmake/Dependencies.cmake)

# Warnings and sanitizers apply to our own code only: LVGL keeps its own flags,
# so its findings don't drown ours.
set(NESTO_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
function(nesto_target_options target)
  target_compile_options(${target} PRIVATE ${NESTO_WARNINGS})
  if(NESTO_SANITIZE)
    target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=address,undefined)
  endif()
endfunction()

# Platform-independent logic, linked into both the program and the tests.
add_library(nesto_core STATIC
  src/core/InputScript.cpp
  src/core/Log.cpp
  src/core/PressGestures.cpp
  src/core/Png.cpp
)
target_include_directories(nesto_core PUBLIC src)
nesto_target_options(nesto_core)

set(NESTO_SOURCES
  src/main.cpp
  src/app/App.cpp
  src/app/Ui.cpp
)
if(NESTO_PLATFORM STREQUAL "sim")
  list(APPEND NESTO_SOURCES src/hal/sim/SimHal.cpp)
else()
  list(APPEND NESTO_SOURCES src/hal/nest/NestHal.cpp)
endif()

add_executable(nesto ${NESTO_SOURCES})
target_link_libraries(nesto PRIVATE nesto_core lvgl::lvgl)
nesto_target_options(nesto)
target_compile_definitions(nesto PRIVATE
  NESTO_VERSION="${PROJECT_VERSION}"
  $<$<STREQUAL:${NESTO_PLATFORM},sim>:NESTO_PLATFORM_SIM>
  $<$<STREQUAL:${NESTO_PLATFORM},nest>:NESTO_PLATFORM_NEST>
)

if(NESTO_PLATFORM STREQUAL "sim" AND SDL2_FOUND)
  target_link_libraries(nesto PRIVATE PkgConfig::SDL2)
  target_compile_definitions(nesto PRIVATE NESTO_HAVE_SDL)
endif()

if(NESTO_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
