add_library(fleet
        src/fleet_zone_store.cpp
        src/fleet_manager.cpp
)

target_include_directories(fleet
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(fleet
        PUBLIC
            karshipta_proto
            # fleet_manager.h includes ward_manager.h (handle_fleet_mission_assignment
            # takes a WardManager&) and transport.h directly, so both are part
            # of fleet's own public interface, not just its implementation.
            ward
            transport
        PRIVATE
            karshipta_warnings
            SQLite::SQLite3
)
