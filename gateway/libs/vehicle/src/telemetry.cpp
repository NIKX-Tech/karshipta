//
// Created by amir abkhoshk on 09/07/2026.
//

#include "telemetry.h"

#include <future>
#include <thread>

#include <spdlog/spdlog.h>

TelemetryInfo::TelemetryInfo(VehicleConnection& d_connection) : connection_(d_connection) {}

TelemetryInfo::~TelemetryInfo() {
    if (!telemetry_) return;
    if (position_handle_) telemetry_->unsubscribe_position(*position_handle_);
    if (flight_mode_handle_) telemetry_->unsubscribe_flight_mode(*flight_mode_handle_);
    if (battery_handle_) telemetry_->unsubscribe_battery(*battery_handle_);
}

bool TelemetryInfo::ensure_telemetry() const {
    if (telemetry_) return true;
    if (!connection_.is_connected()) return false;
    mavsdk_keepalive_ = connection_.get_mavsdk();
    telemetry_ = std::make_unique<mavsdk::Telemetry>(connection_.get_system());
    spdlog::info("telemetry object created");
    return true;
}

void TelemetryInfo::set_telemetry_rate(const float rate) const {
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

void TelemetryInfo::subscribe_drone_position() const {
    if (!ensure_telemetry()) {
        spdlog::error("subscribe to position failed: telemetry not available");
        return;
    }
    position_handle_ = telemetry_->subscribe_position([](const mavsdk::Telemetry::Position& position) {
        spdlog::info(
            "altitude={:.1f}m latitude={:.6f} longitude={:.6f}",
            position.relative_altitude_m,
            position.latitude_deg,
            position.longitude_deg);
    });
    spdlog::info("subscribed to position");
}

void TelemetryInfo::unsubscribe_drone_position() const {
    if (!ensure_telemetry() || !position_handle_) return;
    telemetry_->unsubscribe_position(*position_handle_);
    position_handle_.reset();
    spdlog::info("unsubscribed from position");
}

void TelemetryInfo::subscribe_drone_flight_mode() const {
    if (!ensure_telemetry()) return;

    last_flight_mode_ = mavsdk::Telemetry::FlightMode::Unknown;
    flight_mode_handle_ = telemetry_->subscribe_flight_mode([this](mavsdk::Telemetry::FlightMode flight_mode) {
        if (last_flight_mode_.exchange(flight_mode) != flight_mode) {
            spdlog::info("flight mode changed: {}", static_cast<int>(flight_mode));
        }
    });
    spdlog::info("subscribed to flight mode");
}

void TelemetryInfo::unsubscribe_drone_flight_mode() const {
    if (!ensure_telemetry() || !flight_mode_handle_) return;
    telemetry_->unsubscribe_flight_mode(*flight_mode_handle_);
    flight_mode_handle_.reset();
    spdlog::info("unsubscribed from flight mode");
}

void TelemetryInfo::subscribe_drone_battery() const {
    if (!ensure_telemetry()) {
        spdlog::error("subscribe to battery failed: telemetry not available");
        return;
    }
    battery_handle_ = telemetry_->subscribe_battery([](const mavsdk::Telemetry::Battery& battery) {
        spdlog::info("battery remaining={:.0f}% voltage={:.2f}V", battery.remaining_percent, battery.voltage_v);
    });
    spdlog::info("subscribed to battery");
}

void TelemetryInfo::unsubscribe_drone_battery() const {
    if (!ensure_telemetry() || !battery_handle_) return;
    telemetry_->unsubscribe_battery(*battery_handle_);
    battery_handle_.reset();
    spdlog::info("unsubscribed from battery");
}

bool TelemetryInfo::check_drone_health() const {
    if (!ensure_telemetry()) return false;

    while (!telemetry_->health_all_ok()) {
        spdlog::info("vehicle not ready to arm, waiting on:");
        const mavsdk::Telemetry::Health current_health = telemetry_->health();
        if (!current_health.is_global_position_ok) {
            spdlog::info("  - GPS fix");
        }
        if (!current_health.is_local_position_ok) {
            spdlog::info("  - local position estimate");
        }
        if (!current_health.is_home_position_ok) {
            spdlog::info("  - home position to be set");
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return true;
}

void TelemetryInfo::async_check_drone_health() const {
    if (!ensure_telemetry()) return;

    spdlog::info("waiting for system to be ready");
    auto prom = std::make_shared<std::promise<void>>();
    auto fulfilled = std::make_shared<std::atomic_bool>(false);
    auto future_result = prom->get_future();

    // subscribe_health_all_ok() is a persistent subscription: the callback fires
    // every time health status is reported, not just once. Guard with `fulfilled`
    // so a second `true` after the first can never call set_value() twice (that
    // throws std::future_error on a MAVSDK-internal thread, crashing the process),
    // then unsubscribe below as soon as the wait is over.
    const auto handle = telemetry_->subscribe_health_all_ok([prom, fulfilled](bool result) {
        if (result && !fulfilled->exchange(true)) {
            prom->set_value();
        }
    });

    future_result.get();  // blocks until the promise is fulfilled
    telemetry_->unsubscribe_health_all_ok(handle);
    spdlog::info("system ready to arm");
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

void TelemetryInfo::check_current_takeoff_process(const float target_alt) const {
    float current_position = 0;
    while (current_position < target_alt) {
        current_position = get_relative_altitude_m();
        spdlog::info("takeoff altitude: {:.1f}m", current_position);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void TelemetryInfo::landing_state() const {
    if (!ensure_telemetry()) return;
    while (telemetry_->armed()) {
        const float current_position = get_relative_altitude_m();
        spdlog::info("landing altitude: {:.1f}m", current_position);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    spdlog::info("disarmed, exiting");
}
