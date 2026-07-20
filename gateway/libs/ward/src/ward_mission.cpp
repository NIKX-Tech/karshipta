#include "ward_mission.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <utility>

namespace {

// Rejects what translate() has no way to signal on its own (its signature is
// fixed by the header): an empty ward_id, an empty item list, or an RTL
// item anywhere but last. Runs before translate()/ensure_mission()/
// upload_mission() so a malformed mission never reaches MAVSDK at all.
std::optional<std::string> validate_mission(const karshipta::v1::Mission& mission) {
    if (mission.ward_id().empty()) {
        return "mission has no ward_id";
    }
    if (mission.items_size() == 0) {
        return "mission has no items";
    }
    for (int i = 0; i < mission.items_size() - 1; ++i) {
        if (mission.items(i).action() == karshipta::v1::MISSION_ACTION_RTL) {
            return "RTL item must be the last item in the mission";
        }
    }
    return std::nullopt;
}

// Used only when a download's mission has no prior last_progress_.mission_id()
// to reuse (this process never uploaded anything to this ward this
// session). Not a real UUID, same rationale as MissionImporter's own
// synthesized ids: nothing downstream validates the format.
std::string synthesize_download_mission_id() {
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
    return "downloaded-" + std::to_string(now_ms);
}

}  // namespace

// MAVSDK only offers operator<< for Mission::Result (see WardActions::result_name
// for the same fmt 8 vs 9 note: fmt::streamed needs fmt 9, Ubuntu 22.04's spdlog
// bundles fmt 8).
std::string WardMission::result_name(const mavsdk::Mission::Result result) {
    std::ostringstream stream;
    stream << result;
    return stream.str();
}

WardMission::WardMission(WardConnection& connection)
    : connection_(connection),
      worker_([this](const std::stop_token& stop_token) { run_worker(stop_token); }) {}

WardMission::~WardMission() {
    // Runs before worker_ (declared last) stops and joins via its own
    // jthread destructor, so this can race a job still in flight on mission_;
    // relies on MAVSDK's plugin methods tolerating concurrent calls, the same
    // implicit assumption TelemetryInfo's destructor already makes.
    if (!mission_) return;
    if (progress_handle_) {
        mission_->unsubscribe_mission_progress(*progress_handle_);
    }
}

bool WardMission::ensure_mission() const {
    std::lock_guard lock(init_mutex_);
    if (mission_) return true;
    if (!connection_.is_connected()) return false;
    mavsdk_keepalive_ = connection_.get_mavsdk();
    mission_ = std::make_unique<mavsdk::Mission>(connection_.get_system());
    // ensure_mission() must stay const (start()/pause()/get_progress() all
    // call it), so `this` here is const WardMission*; handle_progress()
    // genuinely mutates state_mutex_-guarded fields, unlike TelemetryInfo's
    // subscriptions, which register from non-const public methods instead of
    // from inside ensure_telemetry() itself.
    progress_handle_ = mission_->subscribe_mission_progress(
        [self = const_cast<WardMission*>(this)](const mavsdk::Mission::MissionProgress progress) {
            self->handle_progress(progress);
        });
    spdlog::info("mission plugin created");
    return true;
}

mavsdk::Mission::Result WardMission::log_result(const std::string& label,
                                                    const mavsdk::Mission::Result result) {
    if (result != mavsdk::Mission::Result::Success) {
        spdlog::error("{} failed: {}", label, result_name(result));
        return result;
    }
    spdlog::info("{} succeeded", label);
    return result;
}

mavsdk::Mission::Result WardMission::start() const {
    if (!ensure_mission()) return mavsdk::Mission::Result::NoSystem;
    return log_result("start mission", mission_->start_mission());
}

mavsdk::Mission::Result WardMission::pause() const {
    if (!ensure_mission()) return mavsdk::Mission::Result::NoSystem;
    return log_result("pause mission", mission_->pause_mission());
}

void WardMission::start_async(mavsdk::Mission::ResultCallback callback) const {
    if (!ensure_mission()) {
        callback(mavsdk::Mission::Result::NoSystem);
        return;
    }
    mission_->start_mission_async(
        [callback = std::move(callback)](const mavsdk::Mission::Result result) {
            callback(log_result("start mission", result));
        });
}

void WardMission::pause_async(mavsdk::Mission::ResultCallback callback) const {
    if (!ensure_mission()) {
        callback(mavsdk::Mission::Result::NoSystem);
        return;
    }
    mission_->pause_mission_async(
        [callback = std::move(callback)](const mavsdk::Mission::Result result) {
            callback(log_result("pause mission", result));
        });
}

void WardMission::enqueue_upload(karshipta::v1::Mission mission) {
    {
        std::lock_guard lock(queue_mutex_);
        job_queue_.push_back(Job{false, std::move(mission)});
    }
    queue_changed_.notify_one();
}

void WardMission::enqueue_download() {
    {
        std::lock_guard lock(queue_mutex_);
        job_queue_.push_back(Job{true, {}});
    }
    queue_changed_.notify_one();
}

mavsdk::Mission::MissionPlan WardMission::translate(const karshipta::v1::Mission& mission,
                                                        bool& ends_with_rtl) {
    // Precondition: validate_mission() has already accepted `mission` (non-empty
    // ward_id, at least one item, RTL only ever at the last index). This
    // function does not re-check any of that.
    ends_with_rtl = mission.items_size() > 0 &&
                    mission.items(mission.items_size() - 1).action() == karshipta::v1::MISSION_ACTION_RTL;

    mavsdk::Mission::MissionPlan plan;
    const int item_count = ends_with_rtl ? mission.items_size() - 1 : mission.items_size();
    plan.mission_items.reserve(static_cast<std::size_t>(item_count));
    for (int i = 0; i < item_count; ++i) {
        const auto& item = mission.items(i);
        mavsdk::Mission::MissionItem out;
        out.latitude_deg = item.position().latitude_deg();
        out.longitude_deg = item.position().longitude_deg();
        out.relative_altitude_m = item.position().altitude_rel_m();
        // command.proto comments speed_m_s as "0 = default"; MAVSDK's own
        // default (NaN, from MissionItem's member initializer) already means
        // "no explicit speed for this item", so only override it for a real
        // request, mirroring command_executor.cpp's kNoSpeedRequested handling
        // of GotoCommand.speed_m_s.
        if (item.speed_m_s() > 0.0f) {
            out.speed_m_s = item.speed_m_s();
        }
        out.loiter_time_s = item.hold_time_s();
        out.acceptance_radius_m = item.acceptance_radius_m();
        switch (item.action()) {
            case karshipta::v1::MISSION_ACTION_TAKEOFF:
                out.vehicle_action = mavsdk::Mission::MissionItem::VehicleAction::Takeoff;
                break;
            case karshipta::v1::MISSION_ACTION_LAND:
                out.vehicle_action = mavsdk::Mission::MissionItem::VehicleAction::Land;
                break;
            default:
                // WAYPOINT, HOLD (expressed via loiter_time_s, not a vehicle_action
                // - MAVSDK's VehicleAction enum has no Hold member), UNSPECIFIED.
                out.vehicle_action = mavsdk::Mission::MissionItem::VehicleAction::None;
                break;
        }
        plan.mission_items.push_back(out);
    }
    return plan;
}

karshipta::v1::Mission WardMission::translate_back(const mavsdk::Mission::MissionPlan& plan) {
    karshipta::v1::Mission mission;
    std::uint32_t seq = 0;
    for (const auto& raw : plan.mission_items) {
        auto* item = mission.add_items();
        item->set_seq(seq++);
        switch (raw.vehicle_action) {
            case mavsdk::Mission::MissionItem::VehicleAction::Takeoff:
                item->set_action(karshipta::v1::MISSION_ACTION_TAKEOFF);
                break;
            case mavsdk::Mission::MissionItem::VehicleAction::Land:
                item->set_action(karshipta::v1::MISSION_ACTION_LAND);
                break;
            case mavsdk::Mission::MissionItem::VehicleAction::None:
                // loiter_time_s > 0 mirrors translate()'s own encoding of
                // MISSION_ACTION_HOLD (NaN, MAVSDK's "unset", compares false
                // here, so it falls through to WAYPOINT correctly).
                item->set_action(raw.loiter_time_s > 0.0f ? karshipta::v1::MISSION_ACTION_HOLD
                                                            : karshipta::v1::MISSION_ACTION_WAYPOINT);
                break;
            default:
                // TransitionToFw/TransitionToMc: no MissionAction equivalent.
                // Best-effort (see header comment): approximated as a
                // waypoint rather than failing the whole download.
                item->set_action(karshipta::v1::MISSION_ACTION_WAYPOINT);
                break;
        }
        auto* position = item->mutable_position();
        position->set_latitude_deg(raw.latitude_deg);
        position->set_longitude_deg(raw.longitude_deg);
        position->set_altitude_rel_m(raw.relative_altitude_m);
        if (!std::isnan(raw.speed_m_s)) {
            item->set_speed_m_s(raw.speed_m_s);
        }
        if (!std::isnan(raw.loiter_time_s)) {
            item->set_hold_time_s(raw.loiter_time_s);
        }
        if (!std::isnan(raw.acceptance_radius_m)) {
            item->set_acceptance_radius_m(raw.acceptance_radius_m);
        }
    }
    return mission;
}

void WardMission::run_worker(const std::stop_token& stop_token) {
    while (true) {
        Job job;
        {
            std::unique_lock lock(queue_mutex_);
            if (!queue_changed_.wait(lock, stop_token, [this] { return !job_queue_.empty(); })) {
                break;
            }
            if (stop_token.stop_requested()) {
                break;
            }
            job = std::move(job_queue_.front());
            job_queue_.pop_front();
        }

        if (job.is_download) {
            if (!ensure_mission()) {
                spdlog::error("mission download failed: mission plugin not available");
                std::lock_guard state_lock(state_mutex_);
                pending_download_result_ = DownloadResult{
                    std::nullopt, "mission download failed: mission plugin not available"};
                continue;
            }
            const auto [result, plan] = mission_->download_mission();
            if (result != mavsdk::Mission::Result::Success) {
                spdlog::error("mission download failed: {}", result_name(result));
                std::lock_guard state_lock(state_mutex_);
                pending_download_result_ = DownloadResult{std::nullopt, result_name(result)};
                continue;
            }
            spdlog::info("mission download succeeded");
            auto downloaded = translate_back(plan);
            std::lock_guard state_lock(state_mutex_);
            downloaded.set_mission_id(!last_progress_.mission_id().empty()
                                           ? last_progress_.mission_id()
                                           : synthesize_download_mission_id());
            pending_download_result_ = DownloadResult{std::move(downloaded), ""};
            continue;
        }

        mavsdk::Mission::Result result;
        bool ends_with_rtl = false;
        mavsdk::Mission::MissionPlan plan;
        if (const auto rejection = validate_mission(job.mission)) {
            spdlog::error("mission upload rejected for {}: {}", job.mission.ward_id(), *rejection);
            result = mavsdk::Mission::Result::InvalidArgument;
        } else if (!ensure_mission()) {
            spdlog::error("mission upload failed: mission plugin not available");
            result = mavsdk::Mission::Result::NoSystem;
        } else {
            plan = translate(job.mission, ends_with_rtl);
            // Only takes effect on the *next* upload per MAVSDK's own doc, so
            // this must run before every upload_mission() call, not just once.
            mission_->set_return_to_launch_after_mission(false);
            result = log_result("mission upload", mission_->upload_mission(plan));
        }

        {
            std::lock_guard state_lock(state_mutex_);
            if (result == mavsdk::Mission::Result::Success) {
                passes_remaining_ = job.mission.repeat_count();
                ends_with_return_to_launch_ = ends_with_rtl;
                interrupted_ = false;
                last_progress_.set_ward_id(job.mission.ward_id());
                last_progress_.set_mission_id(job.mission.mission_id());
                last_progress_.set_current_seq(0);
                last_progress_.set_total_items(static_cast<std::uint32_t>(plan.mission_items.size()));
                last_progress_.set_finished(false);
            }
            pending_upload_result_ = UploadResult{job.mission.mission_id(), result};
        }
    }

    // Reject nothing, just drop: leftovers still queued at shutdown never run,
    // and nothing consumes take_upload_result()/take_download_result() for
    // them (same tradeoff CommandExecutor documents for its own queue).
    std::lock_guard lock(queue_mutex_);
    job_queue_.clear();
}

void WardMission::handle_progress(const mavsdk::Mission::MissionProgress progress) {
    std::lock_guard lock(state_mutex_);
    last_progress_.set_current_seq(static_cast<std::uint32_t>(progress.current));
    last_progress_.set_total_items(static_cast<std::uint32_t>(progress.total));

    if (progress.current != progress.total) return;  // pass not finished yet
    if (interrupted_) return;                          // no re-trigger, no finished, no pending RTL

    if (passes_remaining_ > 0) {
        --passes_remaining_;
        mission_->start_mission_async([](const mavsdk::Mission::Result result) {
            if (result != mavsdk::Mission::Result::Success) {
                spdlog::error("mission re-trigger failed: {}", WardMission::result_name(result));
            }
        });
        return;
    }

    last_progress_.set_finished(true);
    if (ends_with_return_to_launch_) {
        pending_return_to_launch_ = true;
    }
}

karshipta::v1::MissionProgress WardMission::get_progress() const {
    std::lock_guard lock(state_mutex_);
    return last_progress_;
}

bool WardMission::notify_interrupted() {
    std::lock_guard lock(state_mutex_);
    const bool mission_active =
        !interrupted_ && !last_progress_.mission_id().empty() && !last_progress_.finished();
    if (!mission_active) return false;
    interrupted_ = true;
    return true;
}

bool WardMission::take_pending_return_to_launch() {
    std::lock_guard lock(state_mutex_);
    const bool value = pending_return_to_launch_;
    pending_return_to_launch_ = false;
    return value;
}

std::optional<WardMission::UploadResult> WardMission::take_upload_result() {
    std::lock_guard lock(state_mutex_);
    auto result = std::move(pending_upload_result_);
    pending_upload_result_.reset();
    return result;
}

std::optional<WardMission::DownloadResult> WardMission::take_download_result() {
    std::lock_guard lock(state_mutex_);
    auto result = std::move(pending_download_result_);
    pending_download_result_.reset();
    return result;
}
