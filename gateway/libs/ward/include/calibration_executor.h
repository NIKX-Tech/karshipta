#ifndef KARSHIPTA_GATEWAY_CALIBRATION_EXECUTOR_H
#define KARSHIPTA_GATEWAY_CALIBRATION_EXECUTOR_H

#include <karshipta/v1/calibration.pb.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "transport.h"
#include "ward_actions.h"

// Executes StartCalibration/CancelCalibration against exactly one ward's
// WardActions on its own worker thread (karshipta#248), the same
// "Transport threads only enqueue, blocking MAVSDK calls run elsewhere"
// shape CommandExecutor/LogDownloadExecutor already established. A
// calibration is a guided, operator-paced, multi-step process (MAVSDK's
// Calibration plugin streams progress and instruction text, not a single
// request/response) that can run for as long as the operator takes to
// physically rotate the vehicle through each orientation - categorically
// too slow and too interactive for the network thread.
//
// Unlike LogDownloadExecutor, at most one job is ever actually running at
// once by design (a vehicle can't usefully calibrate two sensors
// simultaneously): enqueue_start() rejects immediately, without touching
// the worker thread at all, if a calibration is already in flight for
// this ward.
class CalibrationExecutor {
public:
    // Receives every CalibrationProgress update for a StartCalibration
    // (zero or more RUNNING messages, then exactly one terminal
    // SUCCESS/FAILED/CANCELLED/REJECTED), sent to the requesting client
    // only - same "meaningful just to the requester" reasoning
    // LogDownloadExecutor's own callbacks already use, not broadcast.
    // Invoked from the worker thread; must be thread-safe, which
    // Transport::send already is.
    using ProgressCallback = std::function<void(Transport::ClientId, const karshipta::v1::CalibrationProgress&)>;

    CalibrationExecutor(WardActions& actions, ProgressCallback on_progress);
    // worker_ is the last member, so it stops and joins before anything
    // the worker uses is destroyed; run() drops whatever is still queued
    // at shutdown (never acks a leftover job - same tradeoff
    // LogDownloadExecutor's own destructor comment documents: a
    // StartCalibration the console is still waiting on at shutdown
    // settles via its own request timeout instead).
    ~CalibrationExecutor() = default;

    CalibrationExecutor(const CalibrationExecutor&) = delete;
    CalibrationExecutor& operator=(const CalibrationExecutor&) = delete;
    CalibrationExecutor(CalibrationExecutor&&) = delete;
    CalibrationExecutor& operator=(CalibrationExecutor&&) = delete;

    // Non-blocking, safe from any thread. Rejects immediately (via
    // on_progress, CALIBRATION_STATUS_REJECTED, no worker-thread round
    // trip) if a calibration is already running for this ward, or if
    // calibration_type is unspecified/unrecognized.
    void enqueue_start(Transport::ClientId requester, karshipta::v1::StartCalibration request);

    // Non-blocking, safe from any thread. Calls WardActions::
    // cancel_calibration() directly on the CALLING thread (never the
    // worker thread, which may itself be blocked deep inside whichever
    // calibrate_* call is running) - MAVSDK's cancel() is its own short
    // blocking request/response call, unrelated to the worker's.
    // request.calibration_request_id must match the currently running
    // job's own request_id, or this is a no-op (stale/mistargeted
    // cancel - the job it named already finished or was never this
    // one).
    void cancel(Transport::ClientId requester, const karshipta::v1::CancelCalibration& request);

private:
    struct Job {
        Transport::ClientId requester;
        karshipta::v1::StartCalibration request;
    };

    void run(const std::stop_token& stop_token);
    void execute(Transport::ClientId requester, const karshipta::v1::StartCalibration& request) const;

    WardActions& actions_;
    ProgressCallback on_progress_;

    std::mutex mutex_;
    std::condition_variable_any queue_changed_;
    std::deque<Job> queue_;
    // The request_id of the job currently running (if any), so
    // enqueue_start() can reject a second concurrent attempt immediately
    // and cancel() can tell a stale request apart from the real one.
    // Guarded by mutex_; set just before dequeuing a job onto the worker,
    // cleared just after that job's terminal CalibrationProgress is sent.
    std::optional<std::string> running_request_id_;
    // Last: its loop calls into actions_ on every job, so it must stop and
    // join before actions_ (owned by this ward's ManagedWard, destroyed
    // around the same time) is torn down. Mirrors CommandExecutor::worker_'s
    // declared-last rule.
    std::jthread worker_;
};

#endif  // KARSHIPTA_GATEWAY_CALIBRATION_EXECUTOR_H
