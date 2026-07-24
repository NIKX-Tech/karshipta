add_library(herald
        src/herald_ward_manager.cpp
        src/herald_http_server.cpp
)

target_include_directories(herald
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(herald
        PUBLIC
            karshipta_proto
            transport
        PRIVATE
            karshipta_warnings
            yaml-cpp::yaml-cpp
            httplib::httplib
)

# herald_ward_manager.h only forward-declares WardManager (a pointer member,
# never dereferenced there); the real ward_manager.h is included from
# herald_ward_manager.cpp only inside a KARSHIPTA_GATEWAY_ENABLE_MAVLINK
# guard, so `ward` (and MAVSDK) is only needed - and only exists as a target,
# see root CMakeLists.txt - when that flag is on. PUBLIC compile definition:
# main.cpp (which links herald) needs to see this same macro to decide
# whether to construct HeraldWardManager with the WardManager& constructor
# or the MAVLink-free one.
if(KARSHIPTA_GATEWAY_ENABLE_MAVLINK)
    target_link_libraries(herald PUBLIC ward)
    target_compile_definitions(herald PUBLIC KARSHIPTA_GATEWAY_ENABLE_MAVLINK)
endif()
