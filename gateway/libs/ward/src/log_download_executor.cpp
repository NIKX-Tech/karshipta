#include "log_download_executor.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

namespace {

// Envelope frames cap at 1MB (main.cpp's kMaxEnvelopeBytes); this stays
// comfortably under that to leave room for the rest of LogFileChunk's own
// fields and the surrounding Envelope/oneof wrapper, which this class has
// no visibility into from here.
constexpr std::size_t kLogChunkBytes = 896 * 1024;

std::filesystem::path temp_download_path(const std::string& request_id) {
    // request_id is console-generated and unique per request (same
    // assumption CommandExecutor and WardMission already make about
    // command_id/mission ids), so it alone is enough to avoid colliding
    // with a concurrent download for a different request on this ward.
    return std::filesystem::temp_directory_path() / ("karshipta-log-" + request_id + ".bin");
}

}  // namespace

LogDownloadExecutor::LogDownloadExecutor(WardActions& actions, EntryListCallback on_entry_list,
                                          DownloadAckCallback on_download_ack,
                                          ChunkCallback on_chunk)
    : actions_(actions),
      on_entry_list_(std::move(on_entry_list)),
      on_download_ack_(std::move(on_download_ack)),
      on_chunk_(std::move(on_chunk)),
      worker_([this](const std::stop_token& stop_token) { run(stop_token); }) {}

void LogDownloadExecutor::enqueue_get_entries(Transport::ClientId requester,
                                               karshipta::v1::GetLogEntries request) {
    {
        std::lock_guard lock(mutex_);
        queue_.push_back(Job{requester, std::move(request), std::nullopt});
    }
    queue_changed_.notify_one();
}

void LogDownloadExecutor::enqueue_download(Transport::ClientId requester,
                                            karshipta::v1::DownloadLogFile request) {
    {
        std::lock_guard lock(mutex_);
        queue_.push_back(Job{requester, std::nullopt, std::move(request)});
    }
    queue_changed_.notify_one();
}

void LogDownloadExecutor::run(const std::stop_token& stop_token) {
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
        if (job.list) {
            execute_list(job.requester, *job.list);
        } else {
            execute_download(job.requester, *job.download, stop_token);
        }
    }
    // Drop, not reject, whatever was still queued at shutdown: unlike
    // CommandExecutor's queue, there is no immediate "accepted" ack already
    // sent for these two request kinds, so the console has nothing to
    // retract here - it simply settles via its own request timeout, the
    // same as any other request the gateway never answers at all.
    std::lock_guard lock(mutex_);
    queue_.clear();
}

void LogDownloadExecutor::execute_list(Transport::ClientId requester,
                                        const karshipta::v1::GetLogEntries& request) const {
    spdlog::info("fetching log entries for {} (request_id={})", request.ward_id(),
                 request.request_id());
    karshipta::v1::LogEntryList response;
    response.set_request_id(request.request_id());
    response.set_ward_id(request.ward_id());

    const auto [result, entries] = actions_.get_log_entries();
    if (result != mavsdk::LogFiles::Result::Success) {
        response.set_status(karshipta::v1::LOG_ENTRY_LIST_STATUS_REJECTED);
        response.set_message(WardActions::result_name(result));
        spdlog::warn("get log entries for {} rejected: {}", request.ward_id(), response.message());
        if (on_entry_list_) on_entry_list_(requester, response);
        return;
    }

    response.set_status(karshipta::v1::LOG_ENTRY_LIST_STATUS_SUCCESS);
    for (const auto& entry : entries) {
        auto* out = response.add_entries();
        out->set_log_id(entry.id);
        out->set_date(entry.date);
        out->set_size_bytes(entry.size_bytes);
    }
    if (on_entry_list_) on_entry_list_(requester, response);
}

void LogDownloadExecutor::execute_download(Transport::ClientId requester,
                                            const karshipta::v1::DownloadLogFile& request,
                                            const std::stop_token& stop_token) const {
    spdlog::info("downloading log {} for {} (request_id={})", request.log_id(), request.ward_id(),
                 request.request_id());

    const auto send_rejected_ack = [&](const std::string& reason) {
        karshipta::v1::DownloadLogFileAck ack;
        ack.set_request_id(request.request_id());
        ack.set_ward_id(request.ward_id());
        ack.set_log_id(request.log_id());
        ack.set_status(karshipta::v1::LOG_DOWNLOAD_STATUS_REJECTED);
        ack.set_message(reason);
        spdlog::warn("download log {} for {} rejected: {}", request.log_id(), request.ward_id(),
                     reason);
        if (on_download_ack_) on_download_ack_(requester, ack);
    };

    // download_log_file_async() takes a path, not an Entry alone, but
    // MAVSDK's own Entry carries the metadata it actually keys the
    // transfer on - resolve the requested log_id against a fresh entry
    // list rather than trusting whatever the console last saw, since the
    // vehicle is the only source of truth for which logs still exist.
    const auto [list_result, entries] = actions_.get_log_entries();
    if (list_result != mavsdk::LogFiles::Result::Success) {
        send_rejected_ack("could not list log entries: " + WardActions::result_name(list_result));
        return;
    }
    const auto entry_it =
        std::find_if(entries.begin(), entries.end(),
                     [&](const mavsdk::LogFiles::Entry& entry) { return entry.id == request.log_id(); });
    if (entry_it == entries.end()) {
        send_rejected_ack("unknown log_id " + std::to_string(request.log_id()));
        return;
    }

    const auto temp_path = temp_download_path(request.request_id());
    const auto download_result = actions_.download_log_file(*entry_it, temp_path.string());
    if (download_result != mavsdk::LogFiles::Result::Success) {
        send_rejected_ack("download failed: " + WardActions::result_name(download_result));
        std::error_code remove_error;
        std::filesystem::remove(temp_path, remove_error);
        return;
    }

    std::error_code size_error;
    const auto file_size = std::filesystem::file_size(temp_path, size_error);
    if (size_error) {
        send_rejected_ack("could not read downloaded file: " + size_error.message());
        std::filesystem::remove(temp_path, size_error);
        return;
    }

    const std::uint32_t chunk_count =
        file_size == 0 ? 0
                       : static_cast<std::uint32_t>((file_size + kLogChunkBytes - 1) / kLogChunkBytes);

    {
        karshipta::v1::DownloadLogFileAck ack;
        ack.set_request_id(request.request_id());
        ack.set_ward_id(request.ward_id());
        ack.set_log_id(request.log_id());
        ack.set_status(karshipta::v1::LOG_DOWNLOAD_STATUS_SUCCESS);
        ack.set_total_size_bytes(static_cast<std::uint32_t>(file_size));
        ack.set_chunk_count(chunk_count);
        if (on_download_ack_) on_download_ack_(requester, ack);
    }

    if (chunk_count == 0) {
        std::error_code remove_error;
        std::filesystem::remove(temp_path, remove_error);
        return;
    }

    std::ifstream file(temp_path, std::ios::binary);
    std::vector<char> buffer(kLogChunkBytes);
    for (std::uint32_t chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
        // A multi-chunk transfer can run long on a slow link; check between
        // chunks so a shutdown doesn't have to wait out the whole thing -
        // same reasoning as dispatch_start_mission's own stop_token checks.
        if (stop_token.stop_requested()) {
            break;
        }
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto bytes_read = file.gcount();
        if (bytes_read <= 0 || (!file.good() && !file.eof())) {
            karshipta::v1::LogFileChunk chunk;
            chunk.set_request_id(request.request_id());
            chunk.set_ward_id(request.ward_id());
            chunk.set_log_id(request.log_id());
            chunk.set_status(karshipta::v1::LOG_DOWNLOAD_STATUS_REJECTED);
            chunk.set_message("reading downloaded file failed");
            spdlog::warn("log {} download for {} aborted mid-stream: read failed at chunk {}",
                         request.log_id(), request.ward_id(), chunk_index);
            if (on_chunk_) on_chunk_(requester, chunk);
            break;
        }

        karshipta::v1::LogFileChunk chunk;
        chunk.set_request_id(request.request_id());
        chunk.set_ward_id(request.ward_id());
        chunk.set_log_id(request.log_id());
        chunk.set_status(karshipta::v1::LOG_DOWNLOAD_STATUS_SUCCESS);
        chunk.set_chunk_index(chunk_index);
        chunk.set_data(buffer.data(), static_cast<std::size_t>(bytes_read));
        if (on_chunk_) on_chunk_(requester, chunk);
    }

    file.close();
    std::error_code remove_error;
    std::filesystem::remove(temp_path, remove_error);
    if (remove_error) {
        spdlog::warn("could not remove temp log file {}: {}", temp_path.string(),
                     remove_error.message());
    }
}
