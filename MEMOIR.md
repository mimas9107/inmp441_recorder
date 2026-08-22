---
name:          "MEMOIR.md"
description:   "Project design decisions and development history"
created_date:  "2026/06/18 10:00:00"
modified_date: "2026/08/22 00:00:00"
project_version: "0.2.1"
document_version: "1.0.3"
agent_sign: ['human/mimas', 'gemini cli/gemini-2.0-flash', 'opencode/ox-alpha']
---

# Development Memoir

## Design Decisions
- **VAD Choice**: Switched from `esp-sr` to custom RMS-based VAD due to memory limitations of ESP32-WROOM (no PSRAM). This ensured system stability and reduced boot-up time.
- **Recording Buffer**: Audio is buffered in RAM (96KB) to avoid SPIFFS/SD card latency issues which caused glitches in early versions.
- **Auto-Calibration**: Implemented a 10-second noise floor calibration at boot to adapt to different environments.
- **IDF v6 Migration (0.1.1)**: ESP-IDF v6.0 removed the legacy I2S driver (`driver/i2s.h`), so `main.c` was ported to `esp_driver_i2s` standard mode (`i2s_new_channel` + `I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG`, 32-bit mono-left). Post-upload DMA flush now uses channel disable/enable instead of the removed `i2s_zero_dma_buffer`. Chose stable tag `v6.0.2` over `master` to avoid a moving API target.
- **LED Indicator (0.2.0)**: Adopted the GPIO2 onboard-LED recording indicator from the `feature01a` branch, extended with a standby heartbeat so a silent device is distinguishable from a dead one. Ported manually instead of cherry-picking because feature01a's main.c evolved into a different variant (web server/watchdog).

## Lessons Learned
- ESP32-WROOM heap is very tight when running WiFi and I2S together.
- `esp-sr` models require significant heap or PSRAM, making them unsuitable for base WROOM modules.
- Bit shifting `>> 11` is critical for converting 24-bit I2S data to 16-bit PCM without clipping or extreme gain loss.
- After switching ESP-IDF versions, stale submodules break CMake configure (e.g. missing `mbedtls/tf-psa-crypto`) — always run `git submodule update --init --recursive` after checkout.
- A leftover `IDF_PYTHON_ENV_PATH` pointing at an env built for a different IDF version silently fails package constraint checks; source `export.sh` from a clean shell.
