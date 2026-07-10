add_library(vehicle
        src/vehicle_connection.cpp
        src/telemetry.cpp
        src/vehicle_actions.cpp
)

target_include_directories(vehicle
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(vehicle
        PUBLIC
            MAVSDK::mavsdk
            spdlog::spdlog
        PRIVATE
            karshipta_warnings
)
