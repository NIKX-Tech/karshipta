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
    "${CMAKE_SOURCE_DIR}/third_party/vcpkg/installed/x64-windows"
  )
endif()
find_package(MAVSDK REQUIRED)
find_package(spdlog REQUIRED)
find_package(Protobuf CONFIG REQUIRED)
find_package(ixwebsocket CONFIG REQUIRED)

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
    ARGS --cpp_out "${KARSHIPTA_PROTO_GEN_DIR}" -I "${KARSHIPTA_PROTO_DIR}" "${proto_file}"
    DEPENDS "${proto_file}" protobuf::protoc
    COMMENT "Generating protobuf C++ for karshipta/v1/${proto_name}.proto"
    VERBATIM
  )
  list(APPEND KARSHIPTA_PROTO_GENERATED_SRCS "${gen_src}")
endforeach()

add_library(karshipta_proto STATIC ${KARSHIPTA_PROTO_GENERATED_SRCS})
target_include_directories(karshipta_proto PUBLIC "${KARSHIPTA_PROTO_GEN_DIR}")
target_link_libraries(karshipta_proto PUBLIC protobuf::libprotobuf)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/libs/vehicle)
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
    tests/vehicle/vehicle_connection_test.cpp
  )

  add_executable(karshipta_gateway_tests ${KARSHIPTA_TEST_SOURCES})

  target_link_libraries(karshipta_gateway_tests PRIVATE
    GTest::gtest_main
    MAVSDK::mavsdk
    spdlog::spdlog
    vehicle
    karshipta_warnings
  )

  if(WIN32)
    add_custom_command(TARGET karshipta_gateway_tests POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_RUNTIME_DLLS:karshipta_gateway_tests>
      $<TARGET_FILE_DIR:karshipta_gateway_tests>
      COMMAND_EXPAND_LISTS
    )
    # See the matching comment in src/CMakeLists.txt: vcpkg's zlib DLL is missed by
    # $<TARGET_RUNTIME_DLLS:...> and needs an explicit copy.
    add_custom_command(TARGET karshipta_gateway_tests POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_SOURCE_DIR}/third_party/vcpkg/installed/x64-windows/bin/z.dll"
      $<TARGET_FILE_DIR:karshipta_gateway_tests>
    )
  endif()

  include(GoogleTest)
  gtest_discover_tests(karshipta_gateway_tests PROPERTIES TIMEOUT 60)
endif()
