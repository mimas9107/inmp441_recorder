---
name:          "CHANGELOG.md"
description:   "Project version history and change logs"
created_date:  "2026/02/09 00:00:00"
modified_date: "2026/06/18 10:00:00"
project_version: "0.1.0"
document_version: "1.0.0"
agent_sign: ['human/mimas', 'gemini cli/gemini-2.0-flash']
---

# Changelog

All notable changes to this project will be documented in this file.

## [0.1.0] - 2026-02-09 (Current Milestone)

### Added
- **WiFi Connectivity**: Added WiFi Station mode to connect to local AP.
- **HTTP Upload**: Implemented audio upload via HTTP POST to a remote Flask server.
- **Auto-Calibration VAD**: Added a 10-second warm-up phase on boot to detect environmental noise floor and set a dynamic RMS threshold.
- **RAM-based Recording**: Rewrote recording logic to use an internal RAM buffer (96KB), eliminating SPIFFS write latency issues (glitches).
- **Project Configuration**: Added `Kconfig.projbuild` for easy configuration of WiFi, Server URL, and VAD parameters via `menuconfig`.
- **Python Server**: Added `server/server.py` to receive and play uploaded audio.

### Changed
- **VAD Strategy**: Switched from heavy `esp-sr` (WakeNet) to a lightweight custom RMS-based implementation due to memory constraints on ESP32-WROOM.
- **I2S Configuration**: Optimized for stability with 4x256 DMA buffers.

## [0.0.1] - 2026-02-09

### Added
- **ESP-SR Integration (Attempted)**: Integrated `esp-sr` for WakeWord (WakeNet) detection.
- **Partition Update**: Added `model` partition (1MB) to store speech recognition models.

### Fixed (Before ESP-SR attempt)
- **Audio Quality**: Fixed clipping by adjusting bit shift from `>> 8` to `>> 11`.
- **Playback Speed**: Fixed "double speed" issue by ensuring correct I2S timing (Philips Standard).

### Known Issues
- **Memory Crash**: The `main` branch version crashes on ESP32-WROOM (no PSRAM) due to heap exhaustion during `esp-sr` initialization.
