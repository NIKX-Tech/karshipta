# Only meaningful when RelayTransport itself is built (see root
# CMakeLists.txt for why KARSHIPTA_GATEWAY_ENABLE_RELAY defaults off); this
# target simply doesn't exist otherwise, same gating relay_transport_test.cpp
# uses.
add_executable(karshipta_relay_pair main.cpp)

target_link_libraries(karshipta_relay_pair
        PRIVATE
            transport
            spdlog::spdlog
            yaml-cpp::yaml-cpp
            karshipta_warnings
)
