---
name:          "CHANGELOG.md"
description:   "INMP441 Dataset Collector Node - change history"
created_date:  "2026/02/09 00:00:00"
modified_date: "2026/08/22 00:00:00"
project_version: "0.1.1"
document_version: "1.0.1"
agent_sign: ['human/mimas', 'gemini cli/gemini-2.0-flash', 'opencode/ox-alpha']
---

# Changelog

All notable changes to this project will be documented in this file.

## [0.1.1] - 2026-08-22

### Changed
- **Repository Restructure**: Default branch moved from `master` to `feature01`; the esp-sr experiment was archived to the `waitfornewhardware` branch and `master` was retired. Branch table in README updated accordingly.

## [0.1.0] - 2026-08-22

本分支首個標準化版本（COLLECTOR 變體，文件基準線）。

### Added
- **ESP-IDF v6.0.2 移植**: I2S 自舊版 `driver/i2s.h` 遷移至新 `esp_driver_i2s` 標準模式 API，使本分支得以在 v6 工具鏈編譯與運行。
- **標準文件**: 新增 SPEC.md、MEMOIR.md，並將 README.md、CHANGELOG.md 升級為標準 YAML 標頭格式。

### Changed
- **I2S 初始化**: `i2s_driver_install` + `i2s_set_pin` 改為 `i2s_new_channel` + `i2s_channel_init_std_mode`（Philips 標準、32-bit mono、pin 於 init 一併套用）。
- **讀取超時**: `i2s_read(portMAX_DELAY)` 改為 `i2s_channel_read(1000 ms timeout)` + 空讀保護，確保 Stop 指令能即時生效。
- **DMA 清空**: `i2s_zero_dma_buffer` 以 channel `disable`/`enable` 循環取代。
- **元件依賴**: `REQUIRES driver` 改為 `esp_driver_i2s esp_driver_gpio`。

---

## 歷史記錄（標準化前）

## [feature01a 分支建立] - 2026-02-09

### Added
- **Continuous Collection Mode**: New loop-based recording logic for dataset gathering.
- **Sequential Saving**: Server saves files as `{label}.{count}.wav`.
- **Dataset Support**: Designed for Edge Impulse keyword training.

### Removed
- **VAD Logic / Auto-Calibration**: Not needed for raw collection.

### Later additions (pre-standardization)
- **Remote Control Architecture**: Device registration, connection watchdog, server-driven Start/Stop via device-hosted `/control` endpoint.
- **Indicator LED** (`3d30c9a`): GPIO2 onboard LED on while capturing, off during transmission/idle.
