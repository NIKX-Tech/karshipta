# LogDownloadExecutor

`libs/ward/include/log_download_executor.h`, `libs/ward/src/log_download_executor.cpp`

## Overview

`LogDownloadExecutor` implements flight log download (karshipta#249):
`GetLogEntries` and `DownloadLogFile` go in, `WardActions`'s `LogFiles` calls
happen on a worker thread, and the result (a `LogEntryList`, a
`DownloadLogFileAck`, and the chunks following an accepted download) is sent
back only to the client that asked. It never touches the wire itself;
`main.cpp` decodes Envelopes and hands them to `WardManager::
handle_get_log_entries`/`handle_download_log_file`, which route to the right
ward's `LogDownloadExecutor` and turn its callbacks back into Envelopes
through the `Transport`.

One worker thread and one queue serve both request kinds for a ward, not two:
a multi-MB log transfer can take seconds to tens of seconds, categorically too
slow for the thread that received the request, and keeping entry-listing on
the same dedicated thread (rather than calling `WardActions::
get_log_entries()` directly from the network thread, the way `WardManager`'s
own `handle_get_params()` does for a bounded parameter fetch) avoids repeating
that network-thread-blocking tradeoff for a feature area specifically about
large, slow transfers.

## Responsibilities

- Resolve `GetLogEntries` into a `LogEntryList` via `WardActions::
  get_log_entries()`.
- Resolve `DownloadLogFile` into a `DownloadLogFileAck` (immediate accept or
  reject) followed, only on accept, by `chunk_count` `LogFileChunk` messages
  carrying the file's bytes in order.
- Keep transport threads free: `enqueue_get_entries()`/`enqueue_download()`
  are non-blocking; `WardActions::get_log_entries()`/`download_log_file()`
  (both blocking MAVSDK round trips) run on the executor's own `std::jthread`.
- Route every result to the requesting client only, never broadcast: unlike
  `CommandAck`/mission results, which represent shared fleet state every
  console should see, a log listing or download is meaningful only to
  whoever asked for it, the same reasoning `WardManager::handle_get_params()`
  already uses for `ParamList`.
- Chunk the downloaded file at `kLogChunkBytes` (896 KiB), comfortably under
  `main.cpp`'s `kMaxEnvelopeBytes` (1 MiB) once `LogFileChunk`'s other fields
  and the surrounding `Envelope` are accounted for.
- Clean up its own temp file (`<tmp>/karshipta-log-<request_id>.bin`) once a
  download finishes, succeeds or fails, best-effort (a removal failure is
  logged, not fatal: the OS temp directory is cleaned up eventually anyway).

## Explicitly out of scope

- **Decoding bytes.** `main.cpp` parses the Envelope and routes by payload
  case.
- **Ward routing.** `WardManager::handle_get_log_entries`/
  `handle_download_log_file` own routing by `ward_id`, including the two
  synchronous rejections (unknown ward, not connected) that never reach this
  class at all - see `ward-manager.md`.
- **Resolving which log_id to download against metadata the console already
  has.** `execute_download()` always re-fetches the current entry list from
  the vehicle and resolves `log_id` against that, not against anything the
  request itself claims about size or date: the vehicle is the only source
  of truth for which logs still exist.
- **Retry or resume of a failed/interrupted download.** A `REJECTED`
  `DownloadLogFileAck` or a `REJECTED` `LogFileChunk` mid-stream both mean
  "nothing more is coming for this `request_id`"; the console issues a fresh
  `DownloadLogFile` to try again, there is no partial-resume protocol.

## Lifetime: never retired, unlike CommandExecutor

`ManagedWard::executor` is retired (set to null) and rebuilt across
`stop()`/`force_stop()`/`remove_ward()`, because those exist to safely ground
and disarm a ward before taking it offline. `ManagedWard::log_executor` is
built once, in `add_ward_impl()`, and lives for as long as the ward's own
`WardActions` does - log access is read-only and orthogonal to flight safety,
so there is no reason to interrupt an in-flight log listing or download just
because the ward is being stopped. `WardManager::handle_get_log_entries()`/
`handle_download_log_file()` accordingly check only `ward->connection->
is_connected()`, not `ward->executor`/`ward->busy`, before enqueuing.

## Ack/response semantics

| Moment | Message |
|---|---|
| `GetLogEntries`, `get_entries()` returns Success | `LogEntryList{SUCCESS}` with every entry |
| `GetLogEntries`, `get_entries()` returns anything else | `LogEntryList{REJECTED}` with `WardActions::result_name()` text |
| `DownloadLogFile`, re-listing entries fails | `DownloadLogFileAck{REJECTED}` |
| `DownloadLogFile`, `log_id` not found in the fresh list | `DownloadLogFileAck{REJECTED, "unknown log_id ..."}` |
| `DownloadLogFile`, `download_log_file()` returns anything but Success | `DownloadLogFileAck{REJECTED}` |
| `DownloadLogFile` accepted | `DownloadLogFileAck{SUCCESS, total_size_bytes, chunk_count}`, then `chunk_count` `LogFileChunk{SUCCESS}` messages (0 chunks for an empty file) |
| reading the temp file fails mid-stream (after acceptance) | one `LogFileChunk{REJECTED}`, no further chunks |
| gateway shutdown with jobs still queued | dropped, not acked - the console's own request timeout is what settles it (no "accepted" ack was ever sent for a dropped job to retract) |

## Threading

`enqueue_get_entries()`/`enqueue_download()` run on whatever thread the
transport delivers frames from (via `WardManager`) and only take the queue
mutex. The worker is a `std::jthread` waiting on a `std::condition_variable_
any` with the thread's `stop_token`, same shape as `CommandExecutor::run()`.
`worker_` is the last member: it stops and joins before `actions_` (and
anything else it uses) is destroyed. Every callback fires from the worker
thread and takes the requesting client's `Transport::ClientId`, so the
callback can `Transport::send()` just that client; `Transport::send` is
thread-safe, same as `broadcast`.

A multi-chunk transfer checks `stop_token.stop_requested()` between chunks
(not just once at the top of the job), so a slow transfer in progress does
not hold up gateway shutdown waiting out the whole file - same reasoning as
`CommandExecutor::dispatch_start_mission()`'s own stop_token checks.

## Automated tests

`gateway/tests/ward/ward_manager_test.cpp`, against a never-connected ward
(no autopilot, no Docker) - the only cases testable without a live
connection, since a successful listing/download needs a real vehicle with
real log files:

- `HandleGetLogEntriesRejectsUnknownWardWithReason`
- `HandleGetLogEntriesRejectsUnconnectedWardWithReason`
- `HandleDownloadLogFileRejectsUnknownWardWithReason`
- `HandleDownloadLogFileRejectsUnconnectedWardWithReason`
- `RejectViewerEnvelopeRejectsGetLogEntriesWithResponse`
- `RejectViewerEnvelopeRejectsDownloadLogFileWithAck`

## Manual verification

With PX4 SITL running (it logs every flight to its own `rootfs/log/` by
default) and the console connected: request the log list for a ward that has
flown at least once, confirm a non-empty entry list comes back; request a
download of one entry and confirm the chunks reassemble into a file MAVSDK's
own log file tooling (e.g. `pyulog`) can open. Requesting entries/download for
a ward that has never connected this session (or is currently disconnected)
should reject immediately with "ward is not connected", no worker thread
round trip.
