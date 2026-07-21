cmake_minimum_required(VERSION 3.20)
project(karshipta_gateway LANGUAGES CXX)

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
find_package(MAVSDK REQUIRED)
find_package(spdlog REQUIRED)
# Homebrew/vcpkg protobuf ships a CMake config; apt's ships only the classic
# find module. Try config mode first, fall back to module mode, which also
# defines protobuf::libprotobuf and protobuf::protoc.
find_package(Protobuf CONFIG QUIET)
if(NOT Protobuf_FOUND)
  find_package(Protobuf REQUIRED)
endif()

# IXWebSocket: vcpkg/Homebrew installs provide a CMake config; when absent
# (Linux CI, bare dev machines) fall back to a pinned FetchContent build.
# TLS stays off: the local transport is plain ws://, the encrypted path is
# the M4 relay transport. Zlib (permessage-deflate compression) stays off
# too: it is optional and not vendored anywhere, so skip the dependency.
find_package(ixwebsocket CONFIG QUIET)
if(NOT ixwebsocket_FOUND)
  include(FetchContent)
  set(USE_TLS OFF CACHE BOOL "" FORCE)
  set(USE_ZLIB OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    ixwebsocket
    URL https://github.com/machinezone/IXWebSocket/archive/refs/tags/v11.4.5.zip
  )
  FetchContent_MakeAvailable(ixwebsocket)
  if(NOT TARGET ixwebsocket::ixwebsocket)
    add_library(ixwebsocket::ixwebsocket ALIAS ixwebsocket)
  endif()
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
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/libs/ward)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/libs/fleet)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/libs/herald)
add_subdirectory(${CMAKE_SOURCE_DIR}/src)

option(KARSHIPTA_GATEWAY_BUILD_TESTS "Build gateway unit tests" OFF)

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
  set(KARSHIPTA_TEST_SOURCES
    tests/ward/ward_connection_test.cpp
    tests/ward/telemetry_test.cpp
    tests/ward/command_executor_test.cpp
    tests/ward/ward_mission_test.cpp
    tests/ward/mission_importer_test.cpp
    tests/ward/ward_manager_test.cpp
    tests/transport/websocket_transport_test.cpp
    tests/transport/relay_transport_test.cpp
    tests/fleet/fleet_zone_store_test.cpp
    tests/fleet/fleet_manager_test.cpp
    tests/herald/herald_ward_manager_test.cpp
  )

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
