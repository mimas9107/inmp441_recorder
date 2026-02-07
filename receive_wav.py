#!/usr/bin/env python3
"""
INMP441 WAV Receiver
Receives base64-encoded WAV file from ESP32 via Serial
"""

import serial
import base64
import sys
import time
import argparse


def receive_wav(port, baudrate=115200, output_file="received.wav"):
    """Receive WAV file from ESP32"""
    print(f"Opening {port} at {baudrate} baud...")

    try:
        ser = serial.Serial(port, baudrate, timeout=1)
    except serial.SerialException as e:
        print(f"Error opening port: {e}")
        sys.exit(1)

    print("Waiting for WAV data...")
    print("(Reset ESP32 or wait for recording to complete)")
    print("-" * 50)

    receiving = False
    base64_data = ""
    file_size = 0

    try:
        while True:
            line = ser.readline().decode("utf-8", errors="ignore").strip()

            if line:
                # Print ESP32 log messages
                if not receiving and "===" not in line:
                    print(f"[ESP32] {line}")

                # Start marker
                if line == "===WAV_START===":
                    print("\n>>> Receiving WAV file...")
                    receiving = True
                    base64_data = ""
                    continue

                # File size
                if receiving and line.startswith("SIZE:"):
                    file_size = int(line.split(":")[1])
                    print(f">>> Expected size: {file_size} bytes")
                    continue

                # End marker
                if line == "===WAV_END===":
                    print(">>> Transfer complete!")
                    receiving = False
                    break

                # Base64 data
                if receiving:
                    base64_data += line

    except KeyboardInterrupt:
        print("\nInterrupted by user")
        ser.close()
        sys.exit(0)

    ser.close()

    if not base64_data:
        print("No data received!")
        sys.exit(1)

    # Decode base64
    print(f">>> Decoding {len(base64_data)} base64 characters...")
    try:
        wav_data = base64.b64decode(base64_data)
    except Exception as e:
        print(f"Error decoding base64: {e}")
        sys.exit(1)

    print(f">>> Decoded size: {len(wav_data)} bytes")

    # Save to file
    with open(output_file, "wb") as f:
        f.write(wav_data)

    print(f">>> Saved to: {output_file}")
    print("-" * 50)

    # Verify WAV header
    if len(wav_data) >= 44:
        print("WAV Header Info:")
        riff = wav_data[0:4].decode("ascii", errors="ignore")
        wave = wav_data[8:12].decode("ascii", errors="ignore")
        audio_format = int.from_bytes(wav_data[20:22], "little")
        channels = int.from_bytes(wav_data[22:24], "little")
        sample_rate = int.from_bytes(wav_data[24:28], "little")
        bits_per_sample = int.from_bytes(wav_data[34:36], "little")
        data_size = int.from_bytes(wav_data[40:44], "little")

        print(f"  RIFF: {riff}")
        print(f"  WAVE: {wave}")
        print(f"  Format: {'PCM' if audio_format == 1 else audio_format}")
        print(f"  Channels: {channels}")
        print(f"  Sample Rate: {sample_rate} Hz")
        print(f"  Bits per Sample: {bits_per_sample}")
        print(f"  Data Size: {data_size} bytes")
        print(
            f"  Duration: {data_size / (sample_rate * channels * bits_per_sample // 8):.2f} seconds"
        )

    print("-" * 50)
    print(f"Play with: aplay {output_file}")
    print(f"Or: ffplay {output_file}")
    print(f"View waveform: audacity {output_file}")


def main():
    parser = argparse.ArgumentParser(description="Receive WAV file from ESP32")
    parser.add_argument(
        "-p",
        "--port",
        default="/dev/ttyUSB0",
        help="Serial port (default: /dev/ttyUSB0)",
    )
    parser.add_argument(
        "-b", "--baudrate", type=int, default=115200, help="Baud rate (default: 115200)"
    )
    parser.add_argument(
        "-o",
        "--output",
        default="received.wav",
        help="Output file (default: received.wav)",
    )

    args = parser.parse_args()
    receive_wav(args.port, args.baudrate, args.output)


if __name__ == "__main__":
    main()
