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
            transport
        PRIVATE
            karshipta_warnings
            SQLite::SQLite3
)

# fleet_manager.h only forward-declares WardManager; handle_fleet_mission_assignment
# (the one method that actually needs it, to fan a mission out) only exists
# in KARSHIPTA_GATEWAY_ENABLE_MAVLINK=ON builds, so `ward` (and MAVSDK) is
# only needed - and only exists as a target, see root CMakeLists.txt - then.
# PUBLIC compile definition: main.cpp (which links fleet) needs to see this
# same macro to decide whether to call that method at all.
if(KARSHIPTA_GATEWAY_ENABLE_MAVLINK)
    target_link_libraries(fleet PUBLIC ward)
    target_compile_definitions(fleet PUBLIC KARSHIPTA_GATEWAY_ENABLE_MAVLINK)
endif()
