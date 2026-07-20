add_library(ward
        src/ward_connection.cpp
        src/telemetry.cpp
        src/ward_actions.cpp
        src/ward_mission.cpp
        src/mission_importer.cpp
        src/command_executor.cpp
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
