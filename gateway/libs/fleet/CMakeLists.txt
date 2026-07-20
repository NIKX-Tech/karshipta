add_library(fleet
        src/fleet_zone_store.cpp
)

target_include_directories(fleet
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(fleet
        PUBLIC
            karshipta_proto
        PRIVATE
            karshipta_warnings
            SQLite::SQLite3
)
