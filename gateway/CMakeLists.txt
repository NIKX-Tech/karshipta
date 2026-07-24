cmake_minimum_required(VERSION 3.20)
# C is enabled alongside CXX because cpp-httplib's own CMakeLists.txt
# (fetched below for Herald ingestion, github issue #101) probes for
# optional C dependencies (zlib, brotli) via enable_language(C) inside a
# nested scope; without C already configured at this top-level project()
# call, that nested enable_language() call leaves CMAKE_C_COMPILE_OBJECT
# and friends unset ("CMake Generate step failed", seen building the
# gateway Docker image on a fresh toolchain with no prior CMake cache).
project(karshipta_gateway LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Warnings are errors (repo rule), applied per-target through this interface
# library so fetched third-party code (GoogleTest) is not held to our flags.
# MSVC is a local Windows development convenience; CI enforces gcc and clang.
add_library(karshipta_warnings INTERFACE)
if(MSVC)
  target_compile_options(karshipta_warnings INTERFACE /W4)
else()
  target_compile_options(karshipta_warnings INTERFACE -Wall -Wextra -Werror)
endif()

# karshipta-gateway (drones, MAVLink) and karshipta-herald (generic tags/GPS
# devices via Herald ingest) are two separate, fixed release artifacts built
# from this one codebase - not a runtime toggle either product exposes, a
# build-time switch used only when producing the two artifacts. Verified
# directly before adding this: FleetManager (Fleet/Zone CRUD) and
# HeraldWardManager/HeraldHttpServer link zero MAVSDK; only the `ward`
# library (and its one touchpoint into `fleet`,
# FleetManager::handle_fleet_mission_assignment) does. When OFF, MAVSDK is
# not linked and `ward` is not built at all - a livestock-tag-only user
# should never have to build/run a MAVSDK-linked binary. Default ON: the
# flagship drone demo is still the primary, default artifact.
option(KARSHIPTA_GATEWAY_ENABLE_MAVLINK
  "Build the MAVSDK-backed ward/fleet-mission-assignment path (karshipta-gateway). OFF builds the Herald-only artifact (karshipta-herald)."
  ON)

# On Linux, MAVSDK comes from the prebuilt release .deb and spdlog from apt
# (libspdlog-dev); macOS gets both from Homebrew. On Windows, fall back to
# vendored copies under third_party/ (kept in sync with vcpkg.json for anyone
# using vcpkg instead).
if(WIN32)
  list(APPEND CMAKE_PREFIX_PATH
    "${CMAKE_SOURCE_DIR}/third_party/mavsdk"
    "${CMAKE_SOURCE_DIR}/third_party/spdlog"
    "${CMAKE_SOURCE_DIR}/third_party/protobuf"
    "${CMAKE_SOURCE_DIR}/third_party/vcpkg/installed/x64-windows"
  )
endif()
if(KARSHIPTA_GATEWAY_ENABLE_MAVLINK)
  find_package(MAVSDK REQUIRED)
endif()
find_package(spdlog REQUIRED)
# Homebrew/vcpkg protobuf ships a CMake config; apt's ships only the classic
# find module. Try config mode first, fall back to module mode, which also
# defines protobuf::libprotobuf and protobuf::protoc.
find_package(Protobuf CONFIG QUIET)
if(NOT Protobuf_FOUND)
  find_package(Protobuf REQUIRED)
endif()

# relayly: the M4 relay transport's real C++ SDK (gateway/docs/relay-transport.md).
# Declared before ixwebsocket below because enabling it changes how ixwebsocket
# itself must be built (see the BUILD_SHARED_LIBS note in that block).
# OFF by default everywhere, not just Windows: it pulls in a new dependency tree
# (libsodium via libsodium-cmake, nlohmann/json) on top of relayly itself, and is
# explicitly optional for v0.1 (ROADMAP.md) - enabling it by default would silently
# grow CI build time/surface. relayly's C++ SDK v1 does not support Windows at all
# (sdk/cpp/README.md's Requirements), so forcing it on there is a hard error, not a
# silent no-op, to avoid a confusing "I turned it on and nothing happened".
option(KARSHIPTA_GATEWAY_ENABLE_RELAY
  "Build RelayTransport against relayly's real C++ SDK (BRIEF.md M4, optional for v0.1)"
  OFF)
if(KARSHIPTA_GATEWAY_ENABLE_RELAY AND WIN32)
  message(FATAL_ERROR
    "KARSHIPTA_GATEWAY_ENABLE_RELAY=ON is not supported on Windows: relayly's C++ "
    "SDK v1 does not build there yet (see relayly's sdk/cpp/README.md Requirements).")
endif()

# IXWebSocket: vcpkg/Homebrew installs provide a CMake config; when absent
# (Linux CI, bare dev machines) fall back to a pinned FetchContent build.
# TLS is on only when relay is: the local transport is plain ws:// on a
# LAN/localhost, no TLS needed there, but the relay transport dials out to a
# relay server over the open internet, almost always reached as wss:// in
# any real deployment (TLS terminated by a reverse proxy in front of the
# relay server, the standard way public WebSocket services are run). Noise
# (relayly's own handshake) encrypts the actual payload either way, but
# without transport-level TLS too, connection metadata (device_id, timing,
# message sizes) is visible to anyone on the network path, and there is no
# protection against interference with the connection before the Noise
# handshake completes - real gap for something framed as a security
# promise, found by actually trying to connect a relay-enabled build to a
# wss:// URL and hitting "TLS support is not enabled on this platform".
# Enabling USE_TLS picks ixwebsocket's own per-platform default backend with
# no extra configuration needed here: Apple's native SecureTransport on
# macOS (zero new dependencies), OpenSSL elsewhere (already present on
# standard Linux CI images) - see ixwebsocket's own CMakeLists.txt. This
# only affects wss:// connections; ws:// (the local transport) is
# unaffected either way, ixwebsocket picks TLS per-connection by URL scheme.
# Zlib (permessage-deflate compression) stays off regardless: optional and
# not vendored anywhere, so skip the dependency.
find_package(ixwebsocket CONFIG QUIET)
if(NOT ixwebsocket_FOUND)
  include(FetchContent)
  if(KARSHIPTA_GATEWAY_ENABLE_RELAY)
    set(USE_TLS ON CACHE BOOL "" FORCE)
  else()
    set(USE_TLS OFF CACHE BOOL "" FORCE)
  endif()
  set(USE_ZLIB OFF CACHE BOOL "" FORCE)
  # When relay is enabled, relayly's own CMake also links ixwebsocket (into
  # librelayly.so, a genuine shared library - see relay-transport.md's "Why
  # shared?" reference). ixwebsocket defaults to a static library, so without
  # this override both librelayly.so and this executable would each embed
  # their own independent copy of ixwebsocket's global state (confirmed via
  # `nm -D librelayly.so` showing ix::initNetSystem/uninitNetSystem as
  # exported symbols there too) - two copies of the same reference-counted
  # init/deinit machinery in one process reliably double-frees at exit.
  # Forcing a single shared ixwebsocket means both link the same runtime
  # instance instead. Scoped to only around this one FetchContent_MakeAvailable
  # call, as a plain (non-CACHE) variable restored right after: relayly's own
  # Dependencies.cmake fetches libsodium/nlohmann_json later in this same
  # configure and deliberately builds those static to privately absorb into
  # librelayly.so (its "Why shared?" doc) - a persistent CACHE FORCE here
  # leaked BUILD_SHARED_LIBS=ON into that too and broke its install(EXPORT).
  if(KARSHIPTA_GATEWAY_ENABLE_RELAY)
    set(_karshipta_prev_build_shared_libs "${BUILD_SHARED_LIBS}")
    set(BUILD_SHARED_LIBS ON)
  endif()
  FetchContent_Declare(
    ixwebsocket
    URL https://github.com/machinezone/IXWebSocket/archive/refs/tags/v11.4.5.zip
  )
  FetchContent_MakeAvailable(ixwebsocket)
  if(KARSHIPTA_GATEWAY_ENABLE_RELAY)
    set(BUILD_SHARED_LIBS "${_karshipta_prev_build_shared_libs}")
    unset(_karshipta_prev_build_shared_libs)
  endif()
  if(NOT TARGET ixwebsocket::ixwebsocket)
    add_library(ixwebsocket::ixwebsocket ALIAS ixwebsocket)
  endif()
endif()

if(KARSHIPTA_GATEWAY_ENABLE_RELAY)
  include(FetchContent)
  FetchContent_Declare(
    relayly
    GIT_REPOSITORY https://github.com/NIKX-Tech/relayly.git
    GIT_TAG main
    SOURCE_SUBDIR sdk/cpp
  )
  # Don't build relayly's own unit/integration test suite as part of ours.
  set(RELAYLY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(relayly)
endif()

# yaml-cpp: vcpkg/Homebrew installs provide a CMake config; when absent (Linux
# CI, bare dev machines) fall back to a pinned FetchContent build. Used by
# WardManager to persist the fleet to disk (crash recovery).
find_package(yaml-cpp CONFIG QUIET)
if(NOT yaml-cpp_FOUND)
  include(FetchContent)
  FetchContent_Declare(
    yaml-cpp
    URL https://github.com/jbeder/yaml-cpp/archive/refs/tags/0.8.0.zip
  )
  # yaml-cpp 0.8.0's own CMakeLists.txt declares cmake_minimum_required()
  # below what CMake 4.x still accepts; this only relaxes the floor, it
  # doesn't force old policy behavior on anything that asks for newer.
  set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
  FetchContent_MakeAvailable(yaml-cpp)
  unset(CMAKE_POLICY_VERSION_MINIMUM)
  if(NOT TARGET yaml-cpp::yaml-cpp)
    add_library(yaml-cpp::yaml-cpp ALIAS yaml-cpp)
  endif()
endif()

# cpp-httplib: single-header HTTP server, used only by the Herald HTTP
# ingestion listener (github issue #101). vcpkg/Homebrew installs provide a
# CMake config; when absent (Linux CI, bare dev machines) fall back to a
# pinned FetchContent build, same pattern as ixwebsocket/yaml-cpp above.
# Header-only: cpp-httplib's own CMakeLists already declares an INTERFACE
# target named httplib::httplib, so the alias branch below is a safety net,
# not the normal path.
find_package(httplib CONFIG QUIET)
if(NOT httplib_FOUND)
  include(FetchContent)
  FetchContent_Declare(
    httplib
    URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.18.5.zip
  )
  FetchContent_MakeAvailable(httplib)
  if(NOT TARGET httplib::httplib)
    add_library(httplib::httplib ALIAS httplib)
  endif()
endif()

# SQLite: used by FleetZoneStore (gateway issue #85) to persist Fleet and
# Zone, a narrow, explicitly-signed-off exception to "no backend, no
# database in the MVP" (root CLAUDE.md), scoped to exactly those two
# saved-object types. find_package(SQLite3) is a CMake-bundled module (not a
# vendor-provided config), so it works against whatever system sqlite3 dev
# package is already installed (present by default on macOS; Ubuntu CI needs
# libsqlite3-dev, commonly already part of the runner image). When absent,
# fall back to building the single-file amalgamation via FetchContent - no
# system package required at all, same self-contained spirit as the
# yaml-cpp/ixwebsocket fallbacks above. The amalgamation ships no CMakeLists
# of its own (it is just sqlite3.c/.h), so the library is declared by hand.
find_package(SQLite3 QUIET)
if(NOT SQLite3_FOUND)
  include(FetchContent)
  FetchContent_Declare(
    sqlite3
    URL https://www.sqlite.org/2024/sqlite-amalgamation-3450100.zip
  )
  FetchContent_MakeAvailable(sqlite3)
  add_library(sqlite3_amalgamation STATIC "${sqlite3_SOURCE_DIR}/sqlite3.c")
  target_include_directories(sqlite3_amalgamation PUBLIC "${sqlite3_SOURCE_DIR}")
  if(NOT TARGET SQLite::SQLite3)
    add_library(SQLite::SQLite3 ALIAS sqlite3_amalgamation)
  endif()
endif()

# proto/karshipta/v1/*.proto is the single source of truth (root CLAUDE.md); protoc
# regenerates the C++ bindings into gen/ at configure time so `gateway/gen/` never
# needs to be hand-edited or committed (see repo .gitignore).
set(KARSHIPTA_PROTO_DIR "${CMAKE_SOURCE_DIR}/../proto")
set(KARSHIPTA_PROTO_GEN_DIR "${CMAKE_SOURCE_DIR}/gen")
file(MAKE_DIRECTORY "${KARSHIPTA_PROTO_GEN_DIR}")
file(GLOB KARSHIPTA_PROTO_FILES "${KARSHIPTA_PROTO_DIR}/karshipta/v1/*.proto")

set(KARSHIPTA_PROTO_GENERATED_SRCS "")
foreach(proto_file ${KARSHIPTA_PROTO_FILES})
  get_filename_component(proto_name ${proto_file} NAME_WE)
  set(gen_src "${KARSHIPTA_PROTO_GEN_DIR}/karshipta/v1/${proto_name}.pb.cc")
  set(gen_hdr "${KARSHIPTA_PROTO_GEN_DIR}/karshipta/v1/${proto_name}.pb.h")
  add_custom_command(
    OUTPUT "${gen_src}" "${gen_hdr}"
    COMMAND protobuf::protoc
    # --experimental_allow_proto3_optional: fleet.proto uses proto3 `optional`
    # fields (Zone's altitude bounds). Harmless no-op on protobuf 3.15+, where
    # the feature graduated out of experimental; required on older protoc
    # (e.g. Ubuntu 22.04's apt protobuf-compiler, still 3.12.x) or this flag
    # rejects the file outright instead of just warning.
    ARGS --cpp_out "${KARSHIPTA_PROTO_GEN_DIR}" -I "${KARSHIPTA_PROTO_DIR}"
         --experimental_allow_proto3_optional "${proto_file}"
    DEPENDS "${proto_file}" protobuf::protoc
    COMMENT "Generating protobuf C++ for karshipta/v1/${proto_name}.proto"
    VERBATIM
  )
  list(APPEND KARSHIPTA_PROTO_GENERATED_SRCS "${gen_src}")
endforeach()

# proto/herald/v0/herald.proto is vendored from the standalone NIKX-Tech/herald
# repo (github issue #101), not owned by this repo (root CLAUDE.md's "single
# source of truth" applies to karshipta/v1 only). Its own package (herald.v0)
# keeps it from colliding with anything under karshipta/v1; generated into the
# same karshipta_proto static library rather than a separate target, since
# there is no reason yet to version or expose it independently.
file(GLOB KARSHIPTA_HERALD_PROTO_FILES "${KARSHIPTA_PROTO_DIR}/herald/v0/*.proto")

foreach(proto_file ${KARSHIPTA_HERALD_PROTO_FILES})
  get_filename_component(proto_name ${proto_file} NAME_WE)
  set(gen_src "${KARSHIPTA_PROTO_GEN_DIR}/herald/v0/${proto_name}.pb.cc")
  set(gen_hdr "${KARSHIPTA_PROTO_GEN_DIR}/herald/v0/${proto_name}.pb.h")
  add_custom_command(
    OUTPUT "${gen_src}" "${gen_hdr}"
    COMMAND protobuf::protoc
    ARGS --cpp_out "${KARSHIPTA_PROTO_GEN_DIR}" -I "${KARSHIPTA_PROTO_DIR}"
         --experimental_allow_proto3_optional "${proto_file}"
    DEPENDS "${proto_file}" protobuf::protoc
    COMMENT "Generating protobuf C++ for herald/v0/${proto_name}.proto"
    VERBATIM
  )
  list(APPEND KARSHIPTA_PROTO_GENERATED_SRCS "${gen_src}")
endforeach()

add_library(karshipta_proto STATIC ${KARSHIPTA_PROTO_GENERATED_SRCS})
target_include_directories(karshipta_proto PUBLIC "${KARSHIPTA_PROTO_GEN_DIR}")
target_link_libraries(karshipta_proto PUBLIC protobuf::libprotobuf)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/libs/transport)
if(KARSHIPTA_GATEWAY_ENABLE_MAVLINK)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/libs/ward)
endif()
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/libs/fleet)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/libs/herald)
add_subdirectory(${CMAKE_SOURCE_DIR}/src)

# Operator CLI for triggering relay pairing (gateway/docs/relay-transport.md);
# meaningless without RelayTransport itself.
if(KARSHIPTA_GATEWAY_ENABLE_RELAY)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/tools/relay_pair)
endif()

option(KARSHIPTA_GATEWAY_BUILD_TESTS "Build gateway unit tests" OFF)

if(KARSHIPTA_GATEWAY_BUILD_TESTS AND NOT KARSHIPTA_GATEWAY_ENABLE_MAVLINK)
  # herald_ward_manager_test.cpp and fleet_manager_test.cpp both construct
  # real WardManager instances to exercise the collision-check/mission-
  # assignment paths, so the existing suite only builds against the
  # MAVLink-enabled configuration today. A hard error here is more honest
  # than silently building a trimmed, under-covered suite for the Herald-only
  # artifact - expanding coverage for that build is a followup, not done yet.
  message(FATAL_ERROR
    "KARSHIPTA_GATEWAY_BUILD_TESTS=ON requires KARSHIPTA_GATEWAY_ENABLE_MAVLINK=ON: "
    "the test suite is not yet adapted for a MAVLink-disabled build.")
endif()

if(KARSHIPTA_GATEWAY_BUILD_TESTS)
  # GoogleTest is fetched and pinned rather than system-installed: hermetic on
  # CI and identical on every dev machine.
  include(FetchContent)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  enable_testing()

  # Stacked milestone branches append their own test sources to this list.
  # KARSHIPTA_GATEWAY_ENABLE_MAVLINK=ON is guaranteed above this point (the
  # FATAL_ERROR check), so every ward/fleet-mission-assignment test source
  # here is always safe to build.
  set(KARSHIPTA_TEST_SOURCES
    tests/ward/ward_connection_test.cpp
    tests/ward/telemetry_test.cpp
    tests/ward/command_executor_test.cpp
    tests/ward/ward_mission_test.cpp
    tests/ward/mission_importer_test.cpp
    tests/ward/ward_manager_test.cpp
    tests/transport/websocket_transport_test.cpp
    tests/fleet/fleet_zone_store_test.cpp
    tests/fleet/fleet_manager_test.cpp
    tests/herald/herald_ward_manager_test.cpp
    tests/herald/gt06_parser_test.cpp
  )
  if(KARSHIPTA_GATEWAY_ENABLE_RELAY)
    list(APPEND KARSHIPTA_TEST_SOURCES tests/transport/relay_transport_test.cpp)
  endif()

  add_executable(karshipta_gateway_tests ${KARSHIPTA_TEST_SOURCES})

  target_link_libraries(karshipta_gateway_tests PRIVATE
    GTest::gtest_main
    MAVSDK::mavsdk
    spdlog::spdlog
    ward
    fleet
    herald
    transport
    # the transport test drives the server with a real ix client
    ixwebsocket::ixwebsocket
    karshipta_warnings
  )

  if(WIN32)
    add_custom_command(TARGET karshipta_gateway_tests POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_RUNTIME_DLLS:karshipta_gateway_tests>
      $<TARGET_FILE_DIR:karshipta_gateway_tests>
      COMMAND_EXPAND_LISTS
    )
  endif()

  include(GoogleTest)
  gtest_discover_tests(karshipta_gateway_tests PROPERTIES TIMEOUT 60)
endif()
