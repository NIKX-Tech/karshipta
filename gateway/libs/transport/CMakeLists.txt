add_library(transport
        src/websocket_transport.cpp
        src/relay_transport.cpp
)

target_include_directories(transport
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(transport
        PUBLIC
            spdlog::spdlog
        PRIVATE
            ixwebsocket::ixwebsocket
            yaml-cpp::yaml-cpp
)

if(WIN32)
  # mbedtls (pulled in transitively by ixwebsocket's TLS support) needs
  # BCryptGenRandom from bcrypt.lib for its entropy source.
  target_link_libraries(transport PRIVATE bcrypt)
endif()
