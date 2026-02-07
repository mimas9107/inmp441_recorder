# INMP441 Recorder for ESP32 DevKit V1

## Overview

This project records audio from an **INMP441 I2S microphone** using an ESP32 (DevKit V1) and saves the recording as a standard 16‑bit PCM WAV file in **SPIFFS**. After recording, the WAV file is sent over the serial console as a **base64‑encoded** stream, which can be captured and decoded on a PC with the supplied Python script.

The intent is to provide a quick way to verify that the INMP441 is correctly picking up sound before you integrate a Voice‑Activity‑Detection (VAD) or speech‑recognition engine.

---

## Hardware Connections

| INMP441 Pin | ESP32 DevKit V1 Pin | Function (ESP‑IDF name) | Notes |
|------------|---------------------|------------------------|-------|
| **VDD**    | 3.3 V                | –                      | Power the microphone with 3.3 V. |
| **GND**    | GND                  | –                      | Common ground. |
| **SCK**    | GPIO 26              | **BCLK** (Bit Clock)   | Clock supplied by ESP32. |
| **WS**     | GPIO 25              | **WS** (Word Select)   | Left/Right channel select. Default is **left** (L/R tied to GND). |
| **SD**     | GPIO 22              | **DIN** (Data In)       | Microphone data line (output). |
| **L/R**    | GND (or 3.3 V)       | –                      | Tie to **GND** for left channel, **VDD** for right channel. The code defaults to left channel; change the slot mask if you need the right channel. |

> **Tip:** Keep the I²S lines as short as possible and add pull‑up resistors on the WS line if you see noisy output.

---

## Project Structure

```
inmp441_recorder/
├─ CMakeLists.txt          # Top‑level CMake file for the project
├─ partitions.csv          # Partition table (adds a 1 MiB SPIFFS partition)
├─ sdkconfig.defaults     # Default configuration values (GPIO pins, sample‑rate, etc.)
├─ receive_wav.py          # Python script that reads the base64 stream and writes a WAV file
└─ main/
   ├─ CMakeLists.txt      # Component registration for the "main" component
   ├─ Kconfig.projbuild   # Menuconfig options (GPIO pins, sample rate, record length)
   └─ main.c               # Application code – I²S init, record, SPIFFS write, serial output
```

---

## Prerequisites

1. **ESP‑IDF** (tested with v6.0) – follow the official installation guide to get the `idf.py` command in your `$PATH`.
2. **Python 3** with the `pyserial` package (for the receiver script):
   ```bash
   pip install pyserial
   ```
3. A **USB‑to‑UART** connection to the ESP32 (e.g., `/dev/ttyUSB0`).

---

## Build & Flash

```bash
# 1. Open a terminal and source the ESP‑IDF environment
source $HOME/esp/esp-idf/export.sh

# 2. Navigate to the project directory
cd /home/mimas/esp/inmp441_recorder

# 3. (Optional) Adjust GPIO pins / sample rate via menuconfig
idf.py menuconfig   # → "INMP441 Recorder Configuration"

# 4. Build the firmware
idf.py build

# 5. Flash and start the monitor
idf.py -p /dev/ttyUSB0 flash monitor
```

The monitor will show ESP‑IDF logs and, after the recording finishes, the base64‑encoded WAV block surrounded by the markers:
```
===WAV_START===
SIZE:xxxxx
<base64 data>
===WAV_END===
```

---

## Recording Flow (what the firmware does)
1. **Initialises SPIFFS** – creates `/spiffs` mount point.
2. **Initialises I²S (STD mode)** with the GPIO pins defined in `sdkconfig.defaults` (or `menuconfig`).
3. **Records** `RECORD_SECONDS` (default 5 s) at `SAMPLE_RATE` (default 16 kHz) into a temporary 32‑bit buffer.
4. **Converts** the 24‑bit left‑justified samples from the INMP441 to 16‑bit PCM (`>> 14`).
5. **Writes** a proper WAV header + PCM data to `/spiffs/record.wav`.
6. **Disables** the I²S driver and deletes the channel.
7. **Streams** the file over the serial port as base64, wrapped with the start/end markers.

---

## Receiving & Decoding on the PC

Run the helper script while the ESP32 is printing the base64 block:

```bash
cd /home/mimas/esp/inmp441_recorder
python3 receive_wav.py -p /dev/ttyUSB0 -o captured.wav
```

The script will:
* Open the serial port.
* Wait for the `===WAV_START===` marker.
* Read the declared size.
* Accumulate the base64 payload.
* Decode it into the original WAV file (`captured.wav`).
* Print a short summary (sample rate, bits per sample, duration).

You can then play or analyse the file:
```bash
aplay captured.wav                      # Linux ALSA player
ffplay captured.wav                     # FFmpeg player (cross‑platform)
# Open in Audacity for visual inspection
```

---

## Customising the Project

| Setting | Where to change | Description |
|---------|-----------------|-------------|
| GPIO pins | `sdkconfig.defaults` **or** `idf.py menuconfig` → *INMP441 Recorder Configuration* | Change `I2S_BCK_GPIO`, `I2S_WS_GPIO`, `I2S_DIN_GPIO` to match your wiring. |
| Sample rate | Same location as above (`CONFIG_SAMPLE_RATE`) | Common values: 8000, 16000, 44100, 48000. |
| Recording length | `CONFIG_RECORD_SECONDS` | Number of seconds to capture (max limited by SPIFFS space). |
| Channel (left/right) | In `main.c` – `std_cfg.slot_cfg.slot_mask` | Use `I2S_STD_SLOT_LEFT` (default) or `I2S_STD_SLOT_RIGHT` if your microphone's L/R pin is tied to VDD. |
| SPIFFS size | `partitions.csv` → `spiffs` entry (`0x100000` = 1 MiB) | Increase if you need longer recordings. |

---

## Troubleshooting

| Symptom | Likely Cause | Fix |
|---------|--------------|-----|
| No data printed / all zeros | Incorrect wiring (BCK, WS, or DIN), or microphone not powered. | Verify each connection, ensure 3.3 V is supplied, and that the L/R pin is tied to GND for left channel. |
| Distorted or noisy waveform | Clock jitter or missing pull‑ups on WS line. | Add 10 kΩ pull‑up resistors on the WS pin, keep wires short, and avoid crossing with high‑speed lines. |
| ESP32 resets during recording | SPIFFS out of space (record longer than the 1 MiB partition). | Reduce `RECORD_SECONDS` or increase the SPIFFS partition size in `partitions.csv`. |
| Python script never finishes | Markers not received – maybe serial port wrong or the ESP32 never reached the end of recording. | Make sure you are using the correct UART device (`/dev/ttyUSB0`), and that the ESP32 logs show `===WAV_START===` and `===WAV_END===`. |
| Audio is silent | L/R pin tied to VDD (right channel) while code reads left. | Change `std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_RIGHT;` in `main.c` (or re‑wire L/R to GND). |

---

## License

The code in this repository is released under the **MIT License**. See the `LICENSE` file for details.

---

## Acknowledgements

* Based on the ESP‑IDF `i2s_recorder` example.
* INMP441 data‑format handling references the **INMP441 datasheet** (24‑bit left‑justified in a 32‑bit word).
* Python receiver script is a minimal wrapper around `pyserial` and the standard `base64` library.

---

**Happy recording!** If you run into any issues, feel free to open an issue or ask for help.
