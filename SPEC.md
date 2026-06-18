---
name:          "SPEC.md"
description:   "Technical specifications and hardware/software requirements"
created_date:  "2026/06/18 10:00:00"
modified_date: "2026/06/18 10:00:00"
project_version: "0.1.0"
document_version: "1.0.0"
agent_sign: ['human/mimas', 'gemini cli/gemini-2.0-flash']
---

# Technical Specification

## Hardware Requirements
- **Microcontroller**: ESP32 (DevKit V1)
- **Microphone**: INMP441 (I2S)
- **Power**: 3.3V

## Software Stack
- **Framework**: ESP-IDF (v5.x recommended)
- **Server**: Flask (Python 3.11+)
- **Audio Processing**: RMS-based VAD, Whisper (for transcription)

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
