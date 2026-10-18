#ifndef KARSHIPTA_GATEWAY_LOG_DOWNLOAD_EXECUTOR_H
#define KARSHIPTA_GATEWAY_LOG_DOWNLOAD_EXECUTOR_H

#include <karshipta/v1/log_file.pb.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "transport.h"
#include "ward_actions.h"

// Executes GetLogEntries/DownloadLogFile requests against exactly one
// ward's WardActions on its own worker thread (karshipta#249), the same
// "Transport threads only enqueue, blocking MAVSDK calls run elsewhere"
// shape CommandExecutor already established for Command.action and
// WardMission for mission upload/download. Both request kinds share one
// worker/queue here, not two: a log file transfer can be several MB and
// take seconds to tens of seconds, categorically too slow for the
// network thread, and keeping entry-listing on the same dedicated thread
// (rather than calling WardActions::get_log_entries() directly from
// whatever thread received the request, the way WardManager's own
// handle_get_params() does for a bounded parameter fetch) avoids
// repeating that same network-thread-blocking tradeoff for a feature
// area specifically about large, slow transfers.
//
// Deliberately a separate class from CommandExecutor: GetLogEntries/
// DownloadLogFile are not Command.action-shaped (no ward_id-scoped
// executor-busy semantics, no ACCEPTED/EXECUTING/SUCCESS/REJECTED
// CommandAck lifecycle) - same reasoning WardManager::handle_get_params()
// already established for not routing parameter access through
// CommandExecutor either.
class LogDownloadExecutor {
public:
    // Every callback is also handed the requesting client's id so the
    // result can be sent only to them (Transport::send), not broadcast:
    // unlike CommandAck/mission results, which represent shared fleet state
    // every console should see, a log listing or download is meaningful
    // only to whoever asked for it - the same reasoning GetParams/SetParam
    // already use (WardManager::handle_get_params()'s caller in main.cpp
    // sends ParamList only to the requester).
    //
    // Receives every LogEntryList this executor produces (one per
    // GetLogEntries job). Invoked from the worker thread; must be
    // thread-safe, which Transport::send already is.
    using EntryListCallback = std::function<void(Transport::ClientId, const karshipta::v1::LogEntryList&)>;
    // Receives the immediate accept/reject for a DownloadLogFile request,
    // before any chunk is sent. Same thread-safety contract as
    // EntryListCallback.
    using DownloadAckCallback =
        std::function<void(Transport::ClientId, const karshipta::v1::DownloadLogFileAck&)>;
    // Receives each chunk of an accepted download, in chunk_index order,
    // or a single REJECTED chunk if the transfer aborts mid-stream after
    // already being accepted. Same thread-safety contract as
    // EntryListCallback.
    using ChunkCallback = std::function<void(Transport::ClientId, const karshipta::v1::LogFileChunk&)>;

    LogDownloadExecutor(WardActions& actions, EntryListCallback on_entry_list,
                         DownloadAckCallback on_download_ack, ChunkCallback on_chunk);
    // worker_ is the last member, so it stops and joins before anything the
    // worker uses is destroyed; run() drops whatever is still queued at
    // shutdown (never acks a leftover job - same tradeoff WardMission's own
    // destructor comment documents for its job queue, unlike
    // CommandExecutor's queue, which does ack leftovers: a GetLogEntries/
    // DownloadLogFile the console is still waiting on at shutdown settles
    // via its own REQUEST_TIMEOUT_MS instead, the same as any other
    // request this console makes that the gateway never answers).
    ~LogDownloadExecutor() = default;

    LogDownloadExecutor(const LogDownloadExecutor&) = delete;
    LogDownloadExecutor& operator=(const LogDownloadExecutor&) = delete;
    LogDownloadExecutor(LogDownloadExecutor&&) = delete;
    LogDownloadExecutor& operator=(LogDownloadExecutor&&) = delete;

    // Non-blocking, safe from any thread.
    void enqueue_get_entries(Transport::ClientId requester, karshipta::v1::GetLogEntries request);
    // Non-blocking, safe from any thread.
    void enqueue_download(Transport::ClientId requester, karshipta::v1::DownloadLogFile request);

private:
    // One queued unit of work: exactly one of list/download is set.
    struct Job {
        Transport::ClientId requester;
        std::optional<karshipta::v1::GetLogEntries> list;
        std::optional<karshipta::v1::DownloadLogFile> download;
    };

    void run(const std::stop_token& stop_token);
    void execute_list(Transport::ClientId requester, const karshipta::v1::GetLogEntries& request) const;
    // Needs stop_token to check between chunk sends (see its own .cpp
    // comment) - a large log on a slow link must not hold up gateway
    // shutdown indefinitely once a download is already streaming.
    void execute_download(Transport::ClientId requester, const karshipta::v1::DownloadLogFile& request,
                           const std::stop_token& stop_token) const;

    WardActions& actions_;
    EntryListCallback on_entry_list_;
    DownloadAckCallback on_download_ack_;
    ChunkCallback on_chunk_;

    std::mutex mutex_;
    std::condition_variable_any queue_changed_;
    std::deque<Job> queue_;
    // Last: its loop calls into actions_ on every job, so it must stop and
    // join before actions_ (owned by this ward's ManagedWard, destroyed
    // around the same time) is torn down. Mirrors CommandExecutor::worker_'s
    // declared-last rule.
    std::jthread worker_;
};

#endif  // KARSHIPTA_GATEWAY_LOG_DOWNLOAD_EXECUTOR_H
