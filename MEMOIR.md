---
name:          "MEMOIR.md"
description:   "Project design decisions and development history"
created_date:  "2026/06/18 10:00:00"
modified_date: "2026/06/18 10:00:00"
project_version: "0.1.0"
document_version: "1.0.0"
agent_sign: ['human/mimas', 'gemini cli/gemini-2.0-flash']
---

# Development Memoir

## Design Decisions
- **VAD Choice**: Switched from `esp-sr` to custom RMS-based VAD due to memory limitations of ESP32-WROOM (no PSRAM). This ensured system stability and reduced boot-up time.
- **Recording Buffer**: Audio is buffered in RAM (96KB) to avoid SPIFFS/SD card latency issues which caused glitches in early versions.
- **Auto-Calibration**: Implemented a 10-second noise floor calibration at boot to adapt to different environments.

## Lessons Learned
- ESP32-WROOM heap is very tight when running WiFi and I2S together.
- `esp-sr` models require significant heap or PSRAM, making them unsuitable for base WROOM modules.
- Bit shifting `>> 11` is critical for converting 24-bit I2S data to 16-bit PCM without clipping or extreme gain loss.
