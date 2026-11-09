# NDI Recorder

[中文](README.md) | **English**

A multi-channel NDI scheduled recorder for Windows x64: discover NDI sources on the network, record each channel with independent settings, with timer schedules, automatic segmentation, outage tolerance, live monitoring and 7×24 long-run diagnostics.

Built with C++20 / CMake / Win32 / D3D11 / Dear ImGui / FFmpeg / SQLite.

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows%2010%20%2F%2011%20x64-blue)](docs/ACCEPTANCE.md)
[![C++](https://img.shields.io/badge/C%2B%2B-20-00599C)](CMakeLists.txt)
[![Release](https://img.shields.io/badge/release-v20261002-green)](CMakeLists.txt)

![Main window](docs/screenshots/main-window.png)

> The application UI is Chinese. This document describes the same features in English.

## Features

### Channels and sources

- Automatic NDI source discovery on the LAN; paste a full source name (e.g. `HOST (Camera 1)`) or configure extra discovery IPs
- Per-channel settings: output codec, target bitrate, resolution, frame rate, channel pair, segment length
- Idle preview requests the lowest-bandwidth NDI stream (capped at 15 fps); the full receiver warms up only ~5 seconds before recording, reducing network load
- 16:9 letterboxed preview with dual audio level meters per channel

### Recording and encoding

- **H.264 / MP4**: 20 Mbps by default; encoder auto-probe in the order NVENC → QSV → AMF → Media Foundation; 1080i50 is converted to 1080p50; fragmented MP4 with segments cut on keyframes
- **FFV1 / MKV lossless**: keeps pixels and floating-point audio decoded by the NDI SDK, plus a per-frame `.checksums.json` verification digest
- **Timelapse**: N× speed or one frame every S seconds (H.264 only, no audio track)
- **Automatic segmentation**: every 60 minutes by default (1–1440 adjustable), organized as `channel/date/start-end_taskID_segment`, never overwriting existing files
- **Outage tolerance**: black frames for missing video and silence for missing audio; writing resumes after signal returns and gaps are recorded; limited task restart on source format changes
- **Clock correction**: continuous drift between source timecode and receive clock is corrected; more than 10 s of uninterrupted black-fill triggers a worker restart through the recovery policy

### Schedules and timeline

- One-shot / daily / weekly schedules with skip-once, disable and overnight support; overlap validation per channel
- "Record now" manual demand can coexist with schedule demand
- Unified timeline: schedules on the upper half, actual recordings on the lower half; drag to pan, wheel to zoom (1 minute to 14 days), hour/day/week views, date jump, auto-follow of the current time
- Status colors: blue = scheduled, red = recording, green = completed, amber = signal missing, purple = failed/interrupted, cyan = recovered, gray = disabled/missed
- Schedules still in range resume automatically after a program restart; missed periods are marked as missed

### Audio

- Per-channel record gain adjustable live from -24 to +24 dB (+6 dB ≈ double amplitude), even while recording
- Monitor volume 0–400%, independent of record gain; affects local playback only, never the recorded file
- One channel monitored at a time, muted by default; follows the selected channel pair and the running task's parameter snapshot
- H.264 audio uses the NDI SMPTE reference level at -20 dB (EBU -14 dB optional); FFV1 and timelapse apply no gain

### Monitoring and field diagnostics

- Per-channel "Monitor": SDK drops, receive-queue overflows, processing-cache overflows, field-pairing failures, black/silence fill counts and file write speed — without extra receivers or audio playback
- Live window (last 60 seconds) and per-task cumulative views; per-task aggregate across all segments available from recording history
- Built-in 24-hour diagnostics: `Start-24h-Debug.cmd` periodically samples process CPU/memory/handles/I/O and disk space, with online database backups
- Offline file inspection: `Inspect-Recording.cmd` fully decodes a recording to detect black frames, silent ranges and decode errors

### Reliability and engineering

- The main process never encodes: each channel records in its own worker process with automatic retry (2/4/8 s backoff, up to 3 attempts)
- Workers join a kill-on-close Job Object, so no orphaned recording processes survive the main process
- Bounded buffers everywhere: receive queue, video/audio caches and log queue all have hard limits; persistent encoder lag stops the worker
- Disk safety margin (10 GB by default) stops affected recordings when space runs low; recordings are never auto-deleted
- Configuration and history in SQLite (WAL + FULL sync); config export/import with selective merge and conflict checks
- Per-Monitor V2 high-DPI support plus an in-app 80%–150% scale on top of the system scale; Ctrl+0 / Shift-launch / `--reset-ui-scale` recovery
- Single instance, system tray, optional auto-start on login

## Download and run

Download `ndi-recorder-vYYYYMMDD-windows-x64.zip` from Releases, **extract it completely** and run `ndi-recorder.exe`; keep the bundled DLLs next to the exe. Requires Windows 10/11 x64 and the NDI runtime.

Configuration and history live in `data/recorder.db` next to the exe; recordings default to `recordings/`. Use writable directories. To upgrade, exit the old program, back up `data/`, then overwrite with the new exe and DLLs — never mix DLLs from different builds.

Basic flow: **Add source → pick an NDI source → configure the channel → save → record now or create a schedule**. See the [user guide](docs/USER_GUIDE.md) (Chinese).

## Build from source

Requires Visual Studio 2022 (C++ desktop workload), Windows SDK, CMake 3.24+, and the vendor dependencies prepared as described in [docs/BUILDING.md](docs/BUILDING.md) (NDI SDK, FFmpeg, Dear ImGui, SQLite, nlohmann JSON). Third-party dependencies are not distributed with this repository.

```powershell
# One step: Release build, regression tests, install tree and ZIP package
powershell -NoProfile -ExecutionPolicy Bypass -File tools/pack.ps1

# Or step by step
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel 6
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix dist/ndi-recorder-windows-x64
cpack --config build/CPackConfig.cmake -C Release
```

Versions use the `vYYYYMMDD` date format. Set `APP_RELEASE_DATE` in `CMakeLists.txt` when releasing; it drives the window title, EXE version resource, manifest and ZIP name.

## Project layout

```text
src/         # Application: core (model/schedules/SQLite), ndi (SDK glue), media (encode/mux), manager (worker processes), ui (interface)
tests/       # core / manager unit tests and the A/V sync checker
tools/       # Build & packaging scripts, smoke and diagnostics tools
docs/        # User guide, build instructions, architecture, acceptance records
assets/      # Application icon
vendor/      # Third-party dependencies (not committed, see docs/BUILDING.md)
```

## Documentation

- [User guide](docs/USER_GUIDE.md) (Chinese) — install, UI tour, channels, schedules, audio, monitoring, diagnostics, troubleshooting
- [Build dependencies](docs/BUILDING.md) — dependency versions, checksums, full build & packaging flow
- [Architecture](docs/ARCHITECTURE.md) — process model, media pipeline, bounded resources and failure policy
- [Acceptance records](docs/ACCEPTANCE.md) — executed tests, evidence and uncovered areas
- [Third-party notices](THIRD_PARTY_NOTICES.md)

## Current status

0.1 development preview (v20261002). Builds, unit/integration/UI regressions pass on the development machine (Windows 10); see the [acceptance records](docs/ACCEPTANCE.md). Four-channel 24/72-hour stress tests on target hardware, real-device multichannel/field-order acceptance and Windows 11 on-machine verification are still pending. This version provides no H.265, no Windows service and no multi-machine genlock.

## License

Project code is released under the [MIT License](LICENSE). Third-party components keep their respective licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). NDI is a registered trademark of Vizrt NDI AB. This project is not an official or certified NDI product.
