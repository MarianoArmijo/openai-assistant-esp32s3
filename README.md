# Open RealtimeAPI Embedded SDK

# Table of Contents

- [Installation](#installation)
- [Wake Word (SUKU)](#wake-word-suku)

## Platform/Device Support

This SDK has been developed tested on a `esp32s3`. You need INMP441 + MAX98357A hardware
to run this SDK. Recommended board: Freenove ESP32-S3-WROOM (16 MB flash, 8 MB PSRAM).

## Installation

* `git submodule update --init --recursive`

Call `set-target` with the platform you are targetting. Today only `linux` and `esp32s3` are supported.
* `idf.py set-target esp32s3`

Set your Wifi SSID + Password as env variables
* `export WIFI_SSID=wifi_name`
* `export WIFI_PASSWORD=wifi_pass`
* `export OPENAI_API_KEY=token_value`

Or use the interactive helper script (must be sourced, not executed):
* `source ./setup-env.sh`


Build
* `idf.py build`

Flash & monitor (also writes `srmodels.bin` to the `model` partition)
* `idf.py flash monitor`

## Wake Word (SUKU)

Audio is only sent to OpenAI after you say the wake word **SUKU**, and streaming stops when you finish speaking (local VAD, ~800 ms of silence). Server VAD is disabled so OpenAI never receives ambient audio, which keeps token usage near zero while idle.

### Flow

1. **Idle** – device listens locally for "SUKU" (ESP-SR Multinet7 English). Nothing is sent to OpenAI.
2. **Listening** – after "SUKU", PCM16 @24 kHz is streamed as `input_audio_buffer.append` until ~800 ms of silence.
3. **Responding** – the client sends `input_audio_buffer.commit` + `response.create`, mic upload pauses, and the assistant replies through MAX98357A.
4. **Idle** – when `response.done` arrives, the device clears any leftover input buffer and waits for "SUKU" again. Saying "SUKU" during playback acts as barge-in.

### Tuning

In `src/main.h`:

| Define | Default | Purpose |
|--------|---------|---------|
| `WAKE_WORD_PHRASE` | `"suku"` | Multinet command (try `"su ku"` if detection is poor) |
| `VAD_SILENCE_END_MS` | `800` | Silence duration that closes a turn |

Detection threshold lives in `src/wake_word.cpp` (`MULTINET_THRESHOLD`, default `0.2`).

### Requirements

- 16 MB flash (the partition table includes a 3 MB `model` SPIFFS region for ESP-SR).
- PSRAM enabled (`sdkconfig.defaults`).
- `espressif/esp-sr ~2.3.0` dependency (declared in `src/idf_component.yml`).
