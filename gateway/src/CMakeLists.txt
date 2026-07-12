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
            MAVSDK::mavsdk
            spdlog::spdlog
            vehicle
            transport
            karshipta_proto
            karshipta_warnings
)
