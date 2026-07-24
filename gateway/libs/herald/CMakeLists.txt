add_library(herald
        src/herald_ward_manager.cpp
        src/herald_http_server.cpp
        src/herald_field_mapper.cpp
        src/gt06_parser.cpp
        src/gt06_tcp_server.cpp
)

target_include_directories(herald
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(herald
        PUBLIC
            karshipta_proto
            transport
            # gt06_tcp_server.h publicly inherits from ix::SocketServer (the
            # raw TCP accept-loop/per-connection-thread base class already
            # vendored for the websocket transport, github issue #123) -
            # any consumer holding a Gt06TcpServer needs the base class's
            # full definition too, so this is PUBLIC, not PRIVATE like
            # transport's own ixwebsocket dependency (transport only uses
            # ix types in its .cpp, never in a public base class).
            ixwebsocket::ixwebsocket
            # herald_field_mapper.h includes nlohmann/json_fwd.hpp, and
            # herald_http_server.h (which every consumer of this library
            # includes, including main.cpp) includes herald_field_mapper.h
            # for HeraldFieldMapping - PUBLIC so that include path is
            # visible to consumers, not just herald's own .cpp files.
            nlohmann_json::nlohmann_json
        PRIVATE
            karshipta_warnings
            yaml-cpp::yaml-cpp
            httplib::httplib
)

# herald_ward_manager.h only forward-declares WardManager (a reference
# member, never dereferenced there); the real ward_manager.h is included from
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
