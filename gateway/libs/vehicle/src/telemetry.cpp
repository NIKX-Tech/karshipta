#include "telemetry.h"

#include <spdlog/spdlog.h>

#include <future>
#include <thread>

namespace {

// Operators read logs; MAVSDK's enum values do not mean anything to them.
const char* flight_mode_name(const mavsdk::Telemetry::FlightMode mode) {
    switch (mode) {
        case mavsdk::Telemetry::FlightMode::Ready:
            return "ready";
        case mavsdk::Telemetry::FlightMode::Takeoff:
            return "takeoff";
        case mavsdk::Telemetry::FlightMode::Hold:
            return "hold";
        case mavsdk::Telemetry::FlightMode::Mission:
            return "mission";
        case mavsdk::Telemetry::FlightMode::ReturnToLaunch:
            return "return-to-launch";
        case mavsdk::Telemetry::FlightMode::Land:
            return "land";
        case mavsdk::Telemetry::FlightMode::Offboard:
            return "offboard";
        case mavsdk::Telemetry::FlightMode::FollowMe:
            return "follow-me";
        case mavsdk::Telemetry::FlightMode::Manual:
            return "manual";
        case mavsdk::Telemetry::FlightMode::Altctl:
            return "altitude-control";
        case mavsdk::Telemetry::FlightMode::Posctl:
            return "position-control";
        case mavsdk::Telemetry::FlightMode::Acro:
            return "acro";
        case mavsdk::Telemetry::FlightMode::Stabilized:
            return "stabilized";
        case mavsdk::Telemetry::FlightMode::Rattitude:
            return "rattitude";
        default:
            return "unknown";
    }
}

}  // namespace

TelemetryInfo::TelemetryInfo(VehicleConnection& connection) : connection_(connection) {}

TelemetryInfo::~TelemetryInfo() {
    std::lock_guard lock(mutex_);
    if (!telemetry_) return;
    if (position_handle_) telemetry_->unsubscribe_position(*position_handle_);
    if (flight_mode_handle_) telemetry_->unsubscribe_flight_mode(*flight_mode_handle_);
    if (battery_handle_) telemetry_->unsubscribe_battery(*battery_handle_);
}

bool TelemetryInfo::ensure_telemetry() const {
    std::lock_guard lock(mutex_);
    if (telemetry_) return true;
    if (!connection_.is_connected()) return false;
    mavsdk_keepalive_ = connection_.get_mavsdk();
    telemetry_ = std::make_unique<mavsdk::Telemetry>(connection_.get_system());
    spdlog::info("telemetry object created");
    return true;
}

void TelemetryInfo::set_telemetry_rate(const float rate) {
    if (!ensure_telemetry()) {
        spdlog::error("setting rate failed: telemetry not available");
        return;
    }
    const auto set_rate_result = telemetry_->set_rate_position(rate);  // rate = requested Hz
    if (set_rate_result != mavsdk::Telemetry::Result::Success) {
        spdlog::error("setting rate failed: result={}", static_cast<int>(set_rate_result));
        return;
    }
    spdlog::info("telemetry rate set successfully");
}

void TelemetryInfo::subscribe_position() {
    if (!ensure_telemetry()) {
        spdlog::error("subscribe to position failed: telemetry not available");
        return;
    }
    std::lock_guard lock(mutex_);
    position_handle_ =
        telemetry_->subscribe_position([](const mavsdk::Telemetry::Position& position) {
            spdlog::info("altitude={:.1f}m latitude={:.6f} longitude={:.6f}",
                         position.relative_altitude_m, position.latitude_deg,
                         position.longitude_deg);
        });
    spdlog::info("subscribed to position");
}

void TelemetryInfo::unsubscribe_position() {
    if (!ensure_telemetry()) return;
    std::lock_guard lock(mutex_);
    if (!position_handle_) return;
    telemetry_->unsubscribe_position(*position_handle_);
    position_handle_.reset();
    spdlog::info("unsubscribed from position");
}

void TelemetryInfo::subscribe_flight_mode() {
    if (!ensure_telemetry()) return;

    last_flight_mode_ = mavsdk::Telemetry::FlightMode::Unknown;
    std::lock_guard lock(mutex_);
    flight_mode_handle_ =
        telemetry_->subscribe_flight_mode([this](mavsdk::Telemetry::FlightMode flight_mode) {
            if (last_flight_mode_.exchange(flight_mode) != flight_mode) {
                spdlog::info("flight mode changed: {}", flight_mode_name(flight_mode));
            }
        });
    spdlog::info("subscribed to flight mode");
}

void TelemetryInfo::unsubscribe_flight_mode() {
    if (!ensure_telemetry()) return;
    std::lock_guard lock(mutex_);
    if (!flight_mode_handle_) return;
    telemetry_->unsubscribe_flight_mode(*flight_mode_handle_);
    flight_mode_handle_.reset();
    spdlog::info("unsubscribed from flight mode");
}

void TelemetryInfo::subscribe_battery() {
    if (!ensure_telemetry()) {
        spdlog::error("subscribe to battery failed: telemetry not available");
        return;
    }
    std::lock_guard lock(mutex_);
    battery_handle_ = telemetry_->subscribe_battery([](const mavsdk::Telemetry::Battery& battery) {
        spdlog::info("battery remaining={:.0f}% voltage={:.2f}V", battery.remaining_percent,
                     battery.voltage_v);
    });
    spdlog::info("subscribed to battery");
}

void TelemetryInfo::unsubscribe_battery() {
    if (!ensure_telemetry()) return;
    std::lock_guard lock(mutex_);
    if (!battery_handle_) return;
    telemetry_->unsubscribe_battery(*battery_handle_);
    battery_handle_.reset();
    spdlog::info("unsubscribed from battery");
}

bool TelemetryInfo::check_for_calibration() const {
    if (!ensure_telemetry()) return false;

    const mavsdk::Telemetry::Health check_health = telemetry_->health();
    bool calibration_required = false;
    if (!check_health.is_gyrometer_calibration_ok) {
        spdlog::error("gyro requires calibration");
        calibration_required = true;
    }
    if (!check_health.is_accelerometer_calibration_ok) {
        spdlog::error("accelerometer requires calibration");
        calibration_required = true;
    }
    if (!check_health.is_magnetometer_calibration_ok) {
        spdlog::error("magnetometer (compass) requires calibration");
        calibration_required = true;
    }
    if (calibration_required) {
        return false;
    }
    spdlog::info("calibration check complete");
    return true;
}

float TelemetryInfo::get_relative_altitude_m() const {
    if (!ensure_telemetry()) return 0.0f;
    return telemetry_->position().relative_altitude_m;
}

float TelemetryInfo::get_battery_remaining_percent() const {
    if (!ensure_telemetry()) return 0.0f;
    return telemetry_->battery().remaining_percent;
}

mavsdk::Telemetry::Position TelemetryInfo::get_position() const {
    if (!ensure_telemetry()) return {};
    return telemetry_->position();
}

mavsdk::Telemetry::VelocityNed TelemetryInfo::get_velocity_ned() const {
    if (!ensure_telemetry()) return {};
    return telemetry_->velocity_ned();
}

float TelemetryInfo::get_heading_deg() const {
    if (!ensure_telemetry()) return 0.0f;
    return static_cast<float>(telemetry_->heading().heading_deg);
}

mavsdk::Telemetry::Battery TelemetryInfo::get_battery() const {
    if (!ensure_telemetry()) return {};
    return telemetry_->battery();
}

mavsdk::Telemetry::GpsInfo TelemetryInfo::get_gps_info() const {
    if (!ensure_telemetry()) return {};
    return telemetry_->gps_info();
}

mavsdk::Telemetry::RawGps TelemetryInfo::get_raw_gps() const {
    if (!ensure_telemetry()) return {};
    return telemetry_->raw_gps();
}

mavsdk::Telemetry::FlightMode TelemetryInfo::get_flight_mode() const {
    if (!ensure_telemetry()) return mavsdk::Telemetry::FlightMode::Unknown;
    return telemetry_->flight_mode();
}

bool TelemetryInfo::is_armed() const {
    if (!ensure_telemetry()) return false;
    return telemetry_->armed();
}

bool TelemetryInfo::is_in_air() const {
    if (!ensure_telemetry()) return false;
    return telemetry_->in_air();
}

bool TelemetryInfo::is_health_ok() const {
    if (!ensure_telemetry()) return false;
    return telemetry_->health_all_ok();
}
