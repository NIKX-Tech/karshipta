#include "calibration_executor.h"

#include <spdlog/spdlog.h>

namespace {

bool is_known_calibration_type(const karshipta::v1::CalibrationType type) {
    switch (type) {
        case karshipta::v1::CALIBRATION_TYPE_GYRO:
        case karshipta::v1::CALIBRATION_TYPE_ACCELEROMETER:
        case karshipta::v1::CALIBRATION_TYPE_MAGNETOMETER:
        case karshipta::v1::CALIBRATION_TYPE_LEVEL_HORIZON:
            return true;
        case karshipta::v1::CALIBRATION_TYPE_UNSPECIFIED:
        default:
            return false;
    }
}

// Terminal (non-Next) Calibration::Result -> wire status. Cancelled is its
// own status, not folded into FAILED: the operator asked for it, same
// reasoning WardActions::log_result()'s own Calibration::Result overload
// uses to log it as info, not error.
karshipta::v1::CalibrationStatus terminal_status(const mavsdk::Calibration::Result result) {
    switch (result) {
        case mavsdk::Calibration::Result::Success:
            return karshipta::v1::CALIBRATION_STATUS_SUCCESS;
        case mavsdk::Calibration::Result::Cancelled:
            return karshipta::v1::CALIBRATION_STATUS_CANCELLED;
        default:
            return karshipta::v1::CALIBRATION_STATUS_FAILED;
    }
}

}  // namespace

CalibrationExecutor::CalibrationExecutor(WardActions& actions, ProgressCallback on_progress)
    : actions_(actions),
      on_progress_(std::move(on_progress)),
      worker_([this](const std::stop_token& stop_token) { run(stop_token); }) {}

void CalibrationExecutor::enqueue_start(const Transport::ClientId requester,
                                         karshipta::v1::StartCalibration request) {
    const auto send_rejected = [&](const std::string& reason) {
        karshipta::v1::CalibrationProgress progress;
        progress.set_request_id(request.request_id());
        progress.set_ward_id(request.ward_id());
        progress.set_calibration_type(request.calibration_type());
        progress.set_status(karshipta::v1::CALIBRATION_STATUS_REJECTED);
        progress.set_message(reason);
        spdlog::warn("start_calibration for {} rejected: {}", request.ward_id(), reason);
        if (on_progress_) on_progress_(requester, progress);
    };

    if (!is_known_calibration_type(request.calibration_type())) {
        send_rejected("unspecified or unknown calibration_type");
        return;
    }

    std::lock_guard lock(mutex_);
    if (running_request_id_.has_value()) {
        send_rejected("a calibration is already running for this ward, retry once it finishes");
        return;
    }
    // Reserve the slot immediately (not just "enqueue and let run() claim
    // it later"): without this, two StartCalibration requests arriving
    // back to back would both see running_request_id_ unset and both
    // enqueue, defeating the one-at-a-time guarantee this class exists to
    // provide.
    running_request_id_ = request.request_id();
    queue_.push_back(Job{requester, std::move(request)});
    queue_changed_.notify_one();
}

void CalibrationExecutor::cancel(const Transport::ClientId requester,
                                  const karshipta::v1::CancelCalibration& request) {
    {
        std::lock_guard lock(mutex_);
        if (running_request_id_ != request.calibration_request_id()) {
            spdlog::warn(
                "cancel_calibration for {} ignored: calibration_request_id '{}' is not the "
                "currently running job",
                request.ward_id(), request.calibration_request_id());
            return;
        }
    }
    // Runs on the calling thread, not the worker (which may be blocked
    // deep inside whichever calibrate_* call is running) - MAVSDK's
    // cancel() is its own short blocking request/response, same "quick
    // enough to call directly" treatment push_geofence()/
    // handle_get_params() already give other short MAVLink round trips.
    // Its own result isn't reported back on this path - a successful
    // cancel surfaces as the running job's own terminal
    // CalibrationProgress{CANCELLED} shortly after; a failed cancel
    // simply leaves that job running, observable as it continuing to
    // send RUNNING updates. (requester is unused here for that reason -
    // kept in the signature for symmetry with enqueue_start() and in
    // case a future caller wants a direct ack.)
    (void)requester;
    // Result intentionally unused beyond what WardActions::cancel_calibration()
    // already logs itself (via log_result()) - see this method's own doc
    // comment for why no further reporting happens on this path.
    const auto result = actions_.cancel_calibration();
    (void)result;
}

void CalibrationExecutor::run(const std::stop_token& stop_token) {
    while (true) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            if (!queue_changed_.wait(lock, stop_token, [this] { return !queue_.empty(); })) {
                break;  // stop requested and nothing runnable
            }
            if (stop_token.stop_requested()) {
                break;
            }
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        execute(job.requester, job.request);
        std::lock_guard lock(mutex_);
        running_request_id_.reset();
    }
    // Drop, not reject, whatever was still queued at shutdown - same
    // reasoning as LogDownloadExecutor::run()'s own shutdown comment: no
    // "accepted" ack was ever sent for a dropped job, so the console's
    // own request timeout is what settles it.
    std::lock_guard lock(mutex_);
    queue_.clear();
    running_request_id_.reset();
}

void CalibrationExecutor::execute(const Transport::ClientId requester,
                                   const karshipta::v1::StartCalibration& request) const {
    spdlog::info("executing {} calibration for {} (request_id={})",
                 karshipta::v1::CalibrationType_Name(request.calibration_type()), request.ward_id(),
                 request.request_id());

    const auto on_mavsdk_progress = [&](const mavsdk::Calibration::ProgressData progress) {
        karshipta::v1::CalibrationProgress wire_progress;
        wire_progress.set_request_id(request.request_id());
        wire_progress.set_ward_id(request.ward_id());
        wire_progress.set_calibration_type(request.calibration_type());
        wire_progress.set_status(karshipta::v1::CALIBRATION_STATUS_RUNNING);
        wire_progress.set_has_progress(progress.has_progress);
        wire_progress.set_progress(progress.progress);
        wire_progress.set_has_instruction(progress.has_status_text);
        wire_progress.set_instruction(progress.status_text);
        if (on_progress_) on_progress_(requester, wire_progress);
    };

    mavsdk::Calibration::Result result;
    switch (request.calibration_type()) {
        case karshipta::v1::CALIBRATION_TYPE_GYRO:
            result = actions_.calibrate_gyro(on_mavsdk_progress);
            break;
        case karshipta::v1::CALIBRATION_TYPE_ACCELEROMETER:
            result = actions_.calibrate_accelerometer(on_mavsdk_progress);
            break;
        case karshipta::v1::CALIBRATION_TYPE_MAGNETOMETER:
            result = actions_.calibrate_magnetometer(on_mavsdk_progress);
            break;
        case karshipta::v1::CALIBRATION_TYPE_LEVEL_HORIZON:
            result = actions_.calibrate_level_horizon(on_mavsdk_progress);
            break;
        case karshipta::v1::CALIBRATION_TYPE_UNSPECIFIED:
        default:
            // enqueue_start() already rejects anything but the four known
            // types before this job is ever queued; unreachable in
            // practice, kept exhaustive rather than falling through
            // silently.
            result = mavsdk::Calibration::Result::Unsupported;
            break;
    }

    karshipta::v1::CalibrationProgress terminal;
    terminal.set_request_id(request.request_id());
    terminal.set_ward_id(request.ward_id());
    terminal.set_calibration_type(request.calibration_type());
    terminal.set_status(terminal_status(result));
    if (result != mavsdk::Calibration::Result::Success) {
        terminal.set_message(WardActions::result_name(result));
    }
    if (on_progress_) on_progress_(requester, terminal);
}
