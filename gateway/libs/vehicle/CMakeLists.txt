add_library(vehicle
        src/vehicle_connection.cpp
        src/telemetry.cpp
        src/vehicle_actions.cpp
        src/command_executor.cpp
        src/vehicle_manager.cpp
)

target_include_directories(vehicle
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(vehicle
        PUBLIC
            MAVSDK::mavsdk
            spdlog::spdlog
            karshipta_proto
        PRIVATE
            karshipta_warnings
            transport
)
