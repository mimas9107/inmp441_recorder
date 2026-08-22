---
name:          "SPEC.md"
description:   "Technical specifications and hardware/software requirements"
created_date:  "2026/06/18 10:00:00"
modified_date: "2026/08/22 00:00:00"
project_version: "0.2.0"
document_version: "1.0.2"
agent_sign: ['human/mimas', 'gemini cli/gemini-2.0-flash', 'opencode/ox-alpha']
---

# Technical Specification

## Hardware Requirements
- **Microcontroller**: ESP32 (DevKit V1)
- **Microphone**: INMP441 (I2S)
- **Power**: 3.3V

## Software Stack
- **Framework**: ESP-IDF v6.0.2 (stable tag；legacy I2S driver 已於 v6 移除，使用 `esp_driver_i2s`)
- **Server**: Flask (Python 3.11+, 以 uv 管理依賴)
- **Audio Processing**: RMS-based VAD

## Pin Mapping
| INMP441 Pin | ESP32 GPIO |
|------------|-----------|
| VDD        | 3.3V      |
| GND        | GND       |
| SCK        | GPIO 32   |
| WS         | GPIO 25   |
| SD         | GPIO 33   |
| L/R        | GND       |

## Audio Format
- **Sample Rate**: 16000 Hz
- **Bit Depth**: 16-bit PCM
- **Channels**: Mono

## Indicator LED
- **Pin**: GPIO2 (DevKit V1 onboard LED)
- **States**: 2 s heartbeat = calibrating/idle; solid on = recording; off = uploading
