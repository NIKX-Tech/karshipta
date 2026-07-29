add_library(transport
        src/websocket_transport.cpp
)

# RelayTransport wraps relayly's real C++ SDK (gateway/docs/relay-transport.md),
# only buildable when KARSHIPTA_GATEWAY_ENABLE_RELAY provided that dependency
# (see root gateway/CMakeLists.txt for why it defaults off). PUBLIC, not
# PRIVATE: relay_transport.h's public API (request_pair_code/accept_pair)
# returns relayly:: types directly, so consumers of `transport` (including
# the test binary) need relayly's headers on their own include path too.
if(KARSHIPTA_GATEWAY_ENABLE_RELAY)
  target_sources(transport PRIVATE src/relay_transport.cpp)
  target_compile_definitions(transport PUBLIC KARSHIPTA_GATEWAY_ENABLE_RELAY)
  target_link_libraries(transport PUBLIC relayly::relayly)
endif()

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
