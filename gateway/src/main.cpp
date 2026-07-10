#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

#include <karshipta/v1/envelope.pb.h>

#include "telemetry.h"
#include "transport.h"
#include "vehicle_connection.h"
#include "websocket_transport.h"

namespace {
constexpr auto kConnectionUrl = "udp://:14540";
constexpr float kTelemetryRateHz = 5.0f;  // BRIEF.md M2: VehicleState published at ~5 Hz
constexpr auto kWebSocketHost = "0.0.0.0";
constexpr uint16_t kWebSocketPort = 8765;
constexpr auto kVehicleId = "sitl-1";
constexpr auto kAutopilotName = "PX4";  // only autopilot this milestone connects to (SITL)
const std::chrono::milliseconds kStatePublishInterval{
    static_cast<int64_t>(1000.0f / kTelemetryRateHz)};

// mavsdk::Telemetry::FixType has more granularity than karshipta.v1.GpsFixType; collapse
// the extra values (NoGps, FixDgps, RtkFloat) onto their nearest proto equivalent.
karshipta::v1::GpsFixType to_proto_fix_type(const mavsdk::Telemetry::FixType fix_type) {
    switch (fix_type) {
        case mavsdk::Telemetry::FixType::NoGps:
        case mavsdk::Telemetry::FixType::NoFix:
            return karshipta::v1::GPS_FIX_TYPE_NO_FIX;
        case mavsdk::Telemetry::FixType::Fix2D:
            return karshipta::v1::GPS_FIX_TYPE_FIX_2D;
        case mavsdk::Telemetry::FixType::Fix3D:
        case mavsdk::Telemetry::FixType::FixDgps:
            return karshipta::v1::GPS_FIX_TYPE_FIX_3D;
        case mavsdk::Telemetry::FixType::RtkFloat:
        case mavsdk::Telemetry::FixType::RtkFixed:
            return karshipta::v1::GPS_FIX_TYPE_RTK;
        default:
            return karshipta::v1::GPS_FIX_TYPE_UNSPECIFIED;
    }
}

// mavsdk::Telemetry::FlightMode has more granularity than karshipta.v1.FlightMode; modes
// with no proto equivalent (Ready, FollowMe, Altctl, Acro, Stabilized, Rattitude) map to
// FLIGHT_MODE_UNKNOWN rather than UNSPECIFIED, since a mode IS active, it just isn't one
// the schema names yet.
karshipta::v1::FlightMode to_proto_flight_mode(const mavsdk::Telemetry::FlightMode flight_mode) {
    switch (flight_mode) {
        case mavsdk::Telemetry::FlightMode::Manual:
            return karshipta::v1::FLIGHT_MODE_MANUAL;
        case mavsdk::Telemetry::FlightMode::Hold:
            return karshipta::v1::FLIGHT_MODE_HOLD;
        case mavsdk::Telemetry::FlightMode::Mission:
            return karshipta::v1::FLIGHT_MODE_MISSION;
        case mavsdk::Telemetry::FlightMode::ReturnToLaunch:
            return karshipta::v1::FLIGHT_MODE_RETURN;
        case mavsdk::Telemetry::FlightMode::Takeoff:
            return karshipta::v1::FLIGHT_MODE_TAKEOFF;
        case mavsdk::Telemetry::FlightMode::Land:
            return karshipta::v1::FLIGHT_MODE_LAND;
        case mavsdk::Telemetry::FlightMode::Offboard:
            return karshipta::v1::FLIGHT_MODE_OFFBOARD;
        case mavsdk::Telemetry::FlightMode::Posctl:
            return karshipta::v1::FLIGHT_MODE_POSITION;
        default:
            return karshipta::v1::FLIGHT_MODE_UNKNOWN;
    }
}

uint64_t unix_epoch_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
                                      .count());
}

std::vector<uint8_t> serialize_envelope(const karshipta::v1::Envelope& envelope) {
    std::vector<uint8_t> bytes(envelope.ByteSizeLong());
    envelope.SerializeToArray(bytes.data(), static_cast<int>(bytes.size()));
    return bytes;
}

karshipta::v1::VehicleInfo build_vehicle_info(const VehicleConnection& vehicle) {
    karshipta::v1::VehicleInfo info;
    info.set_vehicle_id(kVehicleId);
    info.set_type(karshipta::v1::VEHICLE_TYPE_MULTIROTOR);
    info.set_autopilot(kAutopilotName);
    info.set_mavlink_system_id(vehicle.get_system()->get_system_id());
    return info;
}

karshipta::v1::VehicleState build_vehicle_state(const VehicleConnection& vehicle,
                                                 const TelemetryInfo& telemetry) {
    karshipta::v1::VehicleState state;
    state.set_vehicle_id(kVehicleId);
    state.set_timestamp_ms(unix_epoch_ms());

    const auto position = telemetry.get_position();
    auto* proto_position = state.mutable_position();
    proto_position->set_latitude_deg(position.latitude_deg);
    proto_position->set_longitude_deg(position.longitude_deg);
    proto_position->set_altitude_msl_m(position.absolute_altitude_m);
    proto_position->set_altitude_rel_m(position.relative_altitude_m);

    const auto velocity = telemetry.get_velocity_ned();
    auto* proto_velocity = state.mutable_velocity();
    proto_velocity->set_north_m_s(velocity.north_m_s);
    proto_velocity->set_east_m_s(velocity.east_m_s);
    proto_velocity->set_down_m_s(velocity.down_m_s);

    state.set_heading_deg(telemetry.get_heading_deg());

    const auto battery = telemetry.get_battery();
    auto* proto_battery = state.mutable_battery();
    proto_battery->set_voltage_v(battery.voltage_v);
    proto_battery->set_remaining_pct(battery.remaining_percent);

    const auto gps = telemetry.get_gps_info();
    auto* proto_gps = state.mutable_gps();
    proto_gps->set_fix_type(to_proto_fix_type(gps.fix_type));
    proto_gps->set_num_satellites(static_cast<uint32_t>(gps.num_satellites));

    state.set_flight_mode(to_proto_flight_mode(telemetry.get_flight_mode()));
    state.set_armed(telemetry.is_armed());
    state.set_in_air(telemetry.is_in_air());
    state.set_health_ok(telemetry.is_health_ok());
    state.set_connected(vehicle.is_connected());

    return state;
}
}  // namespace

int main() {
    auto mavsdk = VehicleConnection::create_shared_core();
    VehicleConnection vehicle(mavsdk, kConnectionUrl);

    if (vehicle.connect() != VehicleConnection::ConnectResult::kSuccess) {
        spdlog::error("failed to connect to {}", vehicle.get_connection_url());
        return EXIT_FAILURE;
    }

    spdlog::info("connected to {}", vehicle.get_connection_url());

    TelemetryInfo telemetry(vehicle);
    telemetry.set_telemetry_rate(kTelemetryRateHz);
    telemetry.subscribe_position();
    telemetry.subscribe_battery();

    WebsocketTransport transport(kWebSocketHost, kWebSocketPort);

    transport.on_connect([&vehicle, &transport](const Transport::ClientId client) {
        karshipta::v1::Envelope envelope;
        *envelope.mutable_vehicle_info() = build_vehicle_info(vehicle);
        transport.send(client, serialize_envelope(envelope));
    });
    transport.on_receive([](const Transport::ClientId client, const std::vector<uint8_t>& bytes) {
        // Command/Mission envelopes land here starting BRIEF.md M3; for now the
        // gateway is publish-only, so incoming frames are only observable, not acted on.
        spdlog::info("received {} byte frame from client {} (not yet handled)", bytes.size(), client);
    });
    transport.start();

    while (vehicle.is_connected()) {
        karshipta::v1::Envelope envelope;
        *envelope.mutable_vehicle_state() = build_vehicle_state(vehicle, telemetry);
        transport.broadcast(serialize_envelope(envelope));
        std::this_thread::sleep_for(kStatePublishInterval);
    }

    transport.stop();
    spdlog::warn("link lost, exiting");
    return EXIT_SUCCESS;
}
