add_executable(karshipta_gateway main.cpp)

if(WIN32)
    add_custom_command(TARGET karshipta_gateway POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:karshipta_gateway>
            $<TARGET_FILE_DIR:karshipta_gateway>
            COMMAND_EXPAND_LISTS
    )
endif()

target_link_libraries(karshipta_gateway
        PRIVATE
            spdlog::spdlog
            yaml-cpp::yaml-cpp
            fleet
            herald
            transport
            karshipta_proto
            karshipta_warnings
)

# Lets main.cpp #ifdef around RelayTransport: relay_transport.cpp/.h only exist
# in the transport library's build when this option is on (see root
# CMakeLists.txt), so main.cpp needs its own compile-time signal to match -
# referencing RelayTransport unconditionally would fail to link when the
# option is off.
if(KARSHIPTA_GATEWAY_ENABLE_RELAY)
    target_compile_definitions(karshipta_gateway PRIVATE KARSHIPTA_GATEWAY_ENABLE_RELAY)
endif()

# Lets main.cpp #ifdef around WardManager construction and the MAVLink-only
# envelope cases (also propagated transitively via fleet/herald's own PUBLIC
# compile definition of the same macro - redefined here too, explicitly,
# matching the KARSHIPTA_GATEWAY_ENABLE_RELAY precedent above).
if(KARSHIPTA_GATEWAY_ENABLE_MAVLINK)
    target_link_libraries(karshipta_gateway PRIVATE MAVSDK::mavsdk ward)
    target_compile_definitions(karshipta_gateway PRIVATE KARSHIPTA_GATEWAY_ENABLE_MAVLINK)
endif()
