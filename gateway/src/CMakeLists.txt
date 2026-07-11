add_executable(karshipta_gateway main.cpp)

if(WIN32)
    add_custom_command(TARGET karshipta_gateway POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:karshipta_gateway>
            $<TARGET_FILE_DIR:karshipta_gateway>
            COMMAND_EXPAND_LISTS
    )
    # vcpkg's zlib (pulled in transitively via protobuf/ixwebsocket) is resolved by
    # CMake's bundled FindZLIB module, which is not debug/release-aware here and always
    # links the release import lib (z.lib -> z.dll) regardless of CMAKE_BUILD_TYPE; that
    # DLL also lives in bin/, not next to the .lib, so $<TARGET_RUNTIME_DLLS:...> above
    # misses it and it needs to be copied explicitly.
    add_custom_command(TARGET karshipta_gateway POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/third_party/vcpkg/installed/x64-windows/bin/z.dll"
            $<TARGET_FILE_DIR:karshipta_gateway>
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
