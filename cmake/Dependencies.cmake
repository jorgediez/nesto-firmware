# Third-party code, pinned by version and checksum. Bump the version and the
# hash together; the hash is the SHA-256 of the release tarball.
include(FetchContent)

# LVGL (MIT): the graphics library.
set(LV_BUILD_CONF_PATH "${PROJECT_SOURCE_DIR}/src/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
set(LV_BUILD_INSTALL OFF CACHE BOOL "" FORCE)
set(LV_FETCH_DEPENDENCIES OFF CACHE BOOL "" FORCE)

FetchContent_Declare(lvgl
  URL https://github.com/lvgl/lvgl/archive/refs/tags/v9.6.0.tar.gz
  URL_HASH SHA256=b20ee3acc1bba13c62d854f9ebd62e4c51e0b443b1e0225892e86442defa84df
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(lvgl)

# Monocypher (BSD-2-Clause or CC0): X25519, ChaCha20-Poly1305 and, from its
# optional part, SHA-512, HMAC and HKDF. It has no CMake build of its own.
FetchContent_Declare(monocypher
  URL https://github.com/LoupVaillant/Monocypher/archive/refs/tags/4.0.3.tar.gz
  URL_HASH SHA256=a7cbae546fbdc489bca632c3747e1ceb8ca3d4bd39e2706a0916f28ccd280e50
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(monocypher)
add_library(monocypher STATIC
  ${monocypher_SOURCE_DIR}/src/monocypher.c
  ${monocypher_SOURCE_DIR}/src/optional/monocypher-ed25519.c
)
target_include_directories(monocypher SYSTEM PUBLIC
  ${monocypher_SOURCE_DIR}/src
  ${monocypher_SOURCE_DIR}/src/optional
)

# nlohmann/json (MIT): the protocol's messages.
FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
  URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(JSON_Install OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nlohmann_json)

# mdns (public domain): the DNS-SD responder that announces the panel
# (PROTOCOL.md section 2). A single header.
FetchContent_Declare(mdns
  URL https://github.com/mjansson/mdns/archive/refs/tags/1.4.3.tar.gz
  URL_HASH SHA256=be1fd8e35599cb7de179decbd0633c121d11a2dcb9cc193ff5c590bd0d480483
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_GetProperties(mdns)
if(NOT mdns_POPULATED)
  # Only the header is needed; its own CMake project builds examples.
  FetchContent_Populate(mdns)
endif()
add_library(mdns INTERFACE)
target_include_directories(mdns SYSTEM INTERFACE ${mdns_SOURCE_DIR})

# SDL2 (zlib licence): only for the simulator's window, and optional there.
if(NESTO_PLATFORM STREQUAL "sim")
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(SDL2 IMPORTED_TARGET sdl2)
  endif()
  if(NOT SDL2_FOUND)
    message(STATUS "SDL2 not found: the simulator will only run headless")
  endif()
endif()

# doctest (MIT): the unit test framework.
if(NESTO_BUILD_TESTS)
  FetchContent_Declare(doctest
    URL https://github.com/doctest/doctest/archive/refs/tags/v2.5.3.tar.gz
    URL_HASH SHA256=174ebc4e769928959614789c5b4e9c3d0a0f81a62bb608756b127bfebfb21331
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(doctest)
endif()
