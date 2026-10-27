# EtwCppConsumer

## Overview

A native C++ real-time ETW consumer that subscribes to the Microsoft-Windows-Kernel-Process provider (process start/stop events), decodes each event with TDH (Trace Data Helper), and measures how long events spend in each stage between being logged and being processed. It is a C++ counterpart practice project to the C# ETWWPFApp.

## Design

- **Minimal work in the callback.** The ETW event callback only copies the fields it needs (event header, buffer context, UserData and its length) into a preallocated, fixed-size slot. It does not call TDH, does not print, and does not allocate memory.
- **Bounded single-producer / single-consumer queue.** Slots live in a lock-free ring buffer (default capacity 1024 slots). When the queue is full, the event is dropped and counted; the callback does not block.
- **Oversized events.** An event whose UserData is larger than the slot (4096 bytes, compile-time constant `kMaxUserDataBytes`) is dropped and counted. ExtendedData is not copied.
- **Decoding on the consumer thread.** A consumer thread rebuilds an `EVENT_RECORD` from the slot copy and decodes it with TDH.
- **Counter identities.** `received = enqueued + dropped_full + dropped_oversize` and `enqueued = consumed`. They are evaluated only after both worker threads have finished.
- **Shutdown and timeouts.** On Ctrl+C the program calls `ControlTraceW(STOP)`, waits up to 5 seconds for the `ProcessTrace` thread, signals the consumer that no more events will arrive, waits up to 5 seconds for the consumer, then calls `CloseTrace`. If the `ProcessTrace` thread does not finish in time, the exit code is 2 (if the consumer thread does not finish in time, the exit code is 4). When a thread is still running, the process ends with `ExitProcess` and the identities are reported as `NOT_EVALUATED`.
- **Leftover sessions.** If a session with the same name already exists at startup (`ERROR_ALREADY_EXISTS`), it is stopped and the session is started again.
- **Timestamps.** The session uses the QPC clock and the consumer opens the trace with `PROCESS_TRACE_MODE_RAW_TIMESTAMP`, so event timestamps arrive as raw QPC ticks. The consumer converts them back to FILETIME (using one QPC/system-time pair taken at startup) before passing the record to the parser.
- **Waiting for events.** `--wake poll` (default) makes the consumer poll with a short timed wait. `--wake notify` makes the callback signal an auto-reset event after each successful enqueue; the consumer waits on that event and keeps a 50 ms safety timeout.

## Directory structure

```
kernel_process_consumer/
  CMakeLists.txt        Build definition for the consumer, the core library and the tests
  main.cpp              Command-line parsing, session setup, consumer thread, shutdown, summary
  src/                  Core library shared by the program and the tests
    spsc_ring.h         Lock-free single-producer / single-consumer ring buffer
    event_slot.h/.cpp   Queue slot layout; copy from EVENT_RECORD and rebuild from the copy
    event_callback.h/.cpp  ETW callback, counters, queue context
    event_parser.h/.cpp TDH decoding and event text formatting
    etw_raii.h/.cpp     RAII wrappers for handles, the ETW session, the trace handle, console mode
    latency_stat.h      Count / sum / max accumulator used for latency statistics
  tests/                Unit and stress tests run by CTest
    integration/        Helper programs and PowerShell scripts for manual end-to-end runs
```

## Build and run

Requirements: Windows, MinGW-w64 GCC, CMake 3.20 or later, Ninja.

From the repository root:

```
cmake -S kernel_process_consumer -B kernel_process_consumer/build -G Ninja
cmake --build kernel_process_consumer/build
```

This builds `kernel_process_consumer.exe`, the four test programs, and the integration helpers `sendctrlc.exe` and `getmode.exe`, all in `kernel_process_consumer/build`.

Starting a real-time ETW session requires administrator rights. Run the program from an elevated PowerShell window:

```
.\kernel_process_consumer\build\kernel_process_consumer.exe [options]
```

Press Ctrl+C to stop. Without administrator rights, `StartTraceW` fails and the program prints `ERROR_ACCESS_DENIED: run as Administrator`.

The program has no `--help` option. The usage text below is printed when an argument is invalid (copied verbatim, including the non-English description line):

```
Usage: kernel_process_consumer.exe [--capacity N] [--summary-file PATH] [--quiet] [--wake poll|notify] [--buffer-kb N] [--min-buffers N] [--max-buffers N] [--flush-timer-sec N] [--test-callback-delay-ms N]   (default N=1024, wake=poll)
  --test-callback-delay-ms N  TEST ONLY: 人為延遲 callback，用來驗證遺失計數，不代表效能 (0..1000, default 0)
```

## Options

| Option | Accepted values | Default | Description |
|---|---|---|---|
| `--capacity N` | power of two, 1 to 65536 | 1024 | Queue capacity in slots. |
| `--summary-file PATH` | non-empty path | none | At exit, also write the summary as `key=value` lines (UTF-8) to this file. A write failure prints a warning and does not change the exit code. |
| `--quiet` | (flag) | off | Do not print a detail block for each event. Decoding, timestamp conversion and latency statistics still run. |
| `--wake poll\|notify` | `poll` or `notify` | `poll` | How the consumer waits for new events (see Design). |
| `--buffer-kb N` | 4 to 16384 | chosen by ETW | `EVENT_TRACE_PROPERTIES.BufferSize`, in KB. |
| `--min-buffers N` | 1 or more | chosen by ETW | `MinimumBuffers`. |
| `--max-buffers N` | 1 or more, not less than `--min-buffers` | chosen by ETW | `MaximumBuffers`. |
| `--flush-timer-sec N` | 1 or more | 1 | `FlushTimer`, in seconds. |
| `--test-callback-delay-ms N` | 0 to 1000 | 0 | TEST ONLY: adds an artificial delay to the callback to exercise the loss counters; numbers produced with it are not performance data. A warning line is printed at startup when N > 0. |

An invalid value prints an error and the usage text, and the program exits with code 11. Unrecognized arguments print a warning and are ignored. After the session starts, the effective buffer settings are read back with `ControlTraceW(QUERY)`, printed, and written to the summary file as `etw_buffer_kb`, `etw_min_buffers`, `etw_max_buffers` and `etw_flush_timer_sec`.

## Tests

Run the unit and stress tests from the repository root:

```
ctest --test-dir kernel_process_consumer/build --output-on-failure
```

| Test | What it checks |
|---|---|
| `queue_basic` | Ring buffer with capacity 4: empty and full behavior, FIFO order, index wrap-around, rejection of capacities that are not a power of two. |
| `queue_full` | Capacity 8, feeding synthetic records through the real callback: queue-full and oversize drop counts, the oversize boundary, other-provider events, and the counter identities; also a concurrent run with 1,000,000 records. |
| `queue_stress` | 5,000,000 synthetic records through the real callback with a producer and a consumer thread; each record carries a sequence number and a checksum. Checks for duplicates, corruption and ordering, the counter identities, and that the set of enqueued records equals the set of consumed records. |
| `parse_copy` | Starts a real ETW session, starts 10 `cmd.exe` processes, and compares TDH decoding done directly in the callback with decoding done on the consumer thread from the slot copy; all fields must match. Requires administrator rights; without them it returns 77, which CTest reports as skipped. |

`tests/integration` contains two PowerShell scripts for manual end-to-end runs. They use `sendctrlc.exe` (sends Ctrl+C to the consumer's console) and `getmode.exe` (prints the console input mode), which the build places next to the consumer. They are not part of CTest and need administrator rights. Their output goes to `kernel_process_consumer/build/integration_out`.

- `run_test.ps1` starts the consumer (extra consumer arguments can be passed with `-ExeArgs`), starts two `cmd.exe` processes, sends Ctrl+C, then prints the consumer output and checks with `logman` that no session is left. `-CreateLeftover` creates a leftover session with the same name before starting; `-RedirectStdin` runs the consumer with stdin redirected from a file.
- `run_test_console.ps1` runs the consumer through `cmd` in a new console with a real console stdin, records the console input mode before and after the run, sends Ctrl+C, then prints the modes, the consumer output and the `logman` check. It is used to see that QuickEdit is turned off during the run and restored afterwards.

Both scripts only print results; they do not decide pass or fail. They exit with a non-zero code only when a required executable is missing.

`verify_pids.ps1` (repository root) compares a file of PIDs with a captured consumer log and prints how many of those PIDs were found as ProcessStart payload `ProcessID` values. It exits with 0 if all PIDs were found, 1 if some were not found, and 2 if an input file does not exist. Result 1 below was produced with it:

```
.\verify_pids.ps1 -PidsFile .\started_pids.txt -LogFile .\run1.txt
```

Save the script as UTF-8 with BOM so Windows PowerShell 5.1 reads it correctly.

## Results

All numbers are single runs on one Windows machine, from a loop that started 50 or 300 cmd.exe processes; they illustrate behavior and are not benchmarks. The raw log and summary files from these runs are not included in the repository.

1. **PID check.** With the default queue capacity, 50 `cmd.exe` processes were started; all 50 PIDs were found in the `ProcessID` field of the ProcessStart event payloads. The queue dropped no events and ETW reported 0 lost events.
2. **Small queue.** With `--capacity 8`, one run received 220 events: 81 were enqueued and 139 were dropped because the queue was full. The identities held and ETW reported 0 lost events.
3. **Header PID vs payload PID.** In ProcessStart events, the header `ProcessId` is the creating process, and the new process's PID is in the payload `ProcessID`. In ProcessStop events the two are the same.
4. **Timestamp conversion.** For 12 events, the converted timestamps were within 1 ms of the payload `CreateTime` / `ExitTime`.
5. **ETW delivery latency** (default buffers, 1-second flush timer): average about 1.3 to 1.6 s, single-event maximum about 2.0 to 2.5 s. In one short run with 16 KB buffers and a 2-second flush timer (40 events): average about 2.7 s, maximum about 3.6 s.
6. **Queue wait** (from enqueue to dequeue by the consumer):
   - Printing every event to the console: average about 2.9 s, maximum about 5.3 s.
   - `--quiet` with `--wake poll`: average about 12 to 17 ms, maximum about 18 to 20 ms.
   - `--quiet` with `--wake notify`: average about 1.07 ms, maximum about 3.5 ms.
   - These results are consistent with most of the wait coming from the granularity of the polling sleep. The source of the remaining ~1 ms was not investigated further.
7. **Processing time.** Decoding one event took about 50 to 90 µs on average (not including printing that event). The callback took about 0.3 to 1.1 µs on average, with a maximum of about 3 to 12 µs (slightly higher in notify mode).
8. **ETW buffers.** When not specified, the system used 64 KB buffers, 16 to 38 buffers, and a 1-second flush timer. A request for a minimum of 4 buffers was adjusted to 16 on this machine.
9. **Shutdown timeout path.** Running the 300-process loop with `--test-callback-delay-ms 100` and `300`, ETW reported 0 for both `events_lost` and `realtime_buffers_lost`, but ETW delivery latency grew to tens of seconds. The worker thread did not finish within 5 seconds, the program took the timeout path and exited with code 2, no session was left behind, and the identities were reported as `NOT_EVALUATED`.
10. **Queue unit and stress tests.** The 5,000,000-record stress test passed: the identities held and there were no duplicate or corrupted sequence numbers.

## Limitations

1. All measurements are single runs, with no repetition and no statistics.
2. The ETW lost-event counters are implemented and reported, but this project never produced lost events, so that path is not verified.
3. The timestamp-conversion base is taken only once at startup. If the system clock is adjusted while the program runs, the converted times will drift from the times in the event payload. This was not tested.
4. `ControlTraceW(STOP)` and `CloseTrace` have no timeout protection, so there is no guaranteed upper bound on shutdown time. The two bounded waits (up to 5 seconds for the `ProcessTrace` thread and up to 5 seconds for the consumer thread) add up to about 10 seconds on top of whatever those two calls take.
5. Writing the summary file on the timeout path, and partial or failed summary-file writes, were not tested; only the "folder does not exist" failure was tested.
6. AddressSanitizer and ThreadSanitizer were not run on the MinGW build.
7. Only the process keyword of Kernel-Process is subscribed; thread and image-load events are not included, and there is no PID filtering.
8. The two scripts in `tests/integration` only print results and must be read manually.
9. Run the program as administrator from a separate PowerShell window. In the VS Code integrated terminal, `--summary-file` once produced no file; the cause was not investigated.
10. The `parse_copy` test saw no events with ExtendedData, so decoding of such events from the slot copy is not tested (ExtendedData is not copied).
