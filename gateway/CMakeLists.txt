cmake_minimum_required(VERSION 3.20)
project(karshipta_gateway LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra)
endif()

# On Linux, MAVSDK/spdlog are installed system-wide via apt (libmavsdk-dev,
# libspdlog-dev). On Windows, fall back to vendored copies under third_party/
# (vcpkg is not used here: it requires PowerShell Core, which this machine
# does not have and could not auto-download).
if(WIN32)
  list(APPEND CMAKE_PREFIX_PATH
    "${CMAKE_SOURCE_DIR}/third_party/mavsdk"
    "${CMAKE_SOURCE_DIR}/third_party/spdlog"
  )
endif()
find_package(MAVSDK REQUIRED)
find_package(spdlog REQUIRED)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/libs/transport)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/libs/vehicle)
add_subdirectory(${CMAKE_SOURCE_DIR}/src)

option(KARSHIPTA_GATEWAY_BUILD_TESTS "Build gateway unit tests" OFF)

if(KARSHIPTA_GATEWAY_BUILD_TESTS)
  find_package(GTest REQUIRED)
  enable_testing()

  add_executable(karshipta_gateway_tests
    tests/vehicle/vehicle_connection_test.cpp
    tests/vehicle/vehicle_manager_test.cpp
    tests/transport/websocket_transport_test.cpp
  )

  target_link_libraries(karshipta_gateway_tests PRIVATE
    GTest::gtest_main
    MAVSDK::mavsdk
    spdlog::spdlog
    vehicle
    transport
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
  gtest_discover_tests(karshipta_gateway_tests)
endif()
