add_library(ward
        src/ward_connection.cpp
        src/telemetry.cpp
        src/ward_actions.cpp
        src/ward_mission.cpp
        src/mission_importer.cpp
        src/command_executor.cpp
        src/log_download_executor.cpp
        src/calibration_executor.cpp
        src/ward_manager.cpp
)

target_include_directories(ward
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(ward
        PUBLIC
            MAVSDK::mavsdk
            spdlog::spdlog
            karshipta_proto
        PRIVATE
            karshipta_warnings
            transport
            yaml-cpp::yaml-cpp
)

# MAVSDK 4 gates MavlinkPassthrough (used for set_home_position, no
# MavlinkDirect replacement exists) behind this macro. PUBLIC because
# ward_actions.h, which includes the passthrough header, is a public
# header of this target; any consumer including it transitively
# (command_executor.h, the tests) needs the same define, not just
# ward's own .cpp files. This file is only ever processed when
# KARSHIPTA_GATEWAY_ENABLE_MAVLINK is ON (see top-level CMakeLists.txt),
# so no further gating is needed here.
target_compile_definitions(ward PUBLIC MAVSDK_ENABLE_MAVLINK_C_API)
