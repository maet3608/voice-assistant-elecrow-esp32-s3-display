# Elecrow ESP32-S3 AI Voice Assistant


This software is for an AI voice assistant that runs on a Elecrow 3.5 inch ESP32-S3 display board. 

The assistant will be activated by touching the screen. You can then ask a question out loud, 
it gest transcribed and answered by an OpenAI chat model. Question and answer are displayed 
and the board also speaks the answer through a small loudspeaker.


## Requirements

> **Before you build:** you must create `include/credentials.h` yourself with
> valid constant values (your Wi-Fi SSID/password and an OpenAI API key). It is
> intentionally **not committed** and the firmware will not compile without it.
> See [Credentials](#includecredentials-h-required-not-committed) below for the
> exact contents. Everything else is configured in `src/app_config.h`, the main
> configuration file for the firmware.

Touch the screen to run one complete voice turn:

1. **STT** – record the spoken phrase with the onboard microphone and transcribe
   it with the OpenAI transcription API.
2. **LLM** – send the transcription to a chat model to get an answer.
3. **TTS** – synthesize the answer with the OpenAI speech API and play it on the
   onboard speaker.
4. **Display** – show the transcription and the answer on the 320x480 display.

## Source layout

| File | Responsibility |
|---|---|
| `src/main.cpp` | `setup()` / `loop()` and the STT → LLM → TTS turn |
| `src/app_config.h` | **Main configuration file** — pins, endpoints, models, timing and buffer sizes |
| `src/display_ui.*` | Display/touch init and all rendering |
| `src/audio_io.*` | I2S + ES8311 codec, capture (`recordSpeech`) and playback (`playPcmMono`) |
| `src/openai_http.*` | Shared HTTPS transport for the three OpenAI calls |
| `src/stt.*` | WAV/multipart upload and transcript parsing |
| `src/llm.*` | Chat completion request/response |
| `src/tts.*` | Speech synthesis and speaker playback |

## Configuration

### `include/credentials.h` (required)

Create `include/credentials.h` and fill in your own values. The project uses
three constants from it — `SSID`, `PASSWORD` and `OPENAI_API_KEY`:

```cpp
#pragma once

// Wi-Fi network the board should join (2.4 GHz only — the ESP32-S3 has no
// 5 GHz radio).
const char *SSID = "your-wifi-ssid";
const char *PASSWORD = "your-wifi-password";

// OpenAI API key with access to the transcription, chat and speech endpoints.
const char *OPENAI_API_KEY = "sk-proj-...";
```


### `src/app_config.h` (main configuration file)

`src/app_config.h` is the **main file for configuration**. Keep
`credentials.h` for secrets only and change everything else here:

- I2S/ES8311 pins and the amplifier enable pin
- `SAMPLE_RATE` and speaker output level
- STT capture limits and voice-activity (VAD) thresholds
- LLM/TTS model names, the system prompt, and token/temperature limits
- OpenAI host and endpoint paths

## Build and upload

```bat
pio run -e elecrow-esp32-s3
pio run -e elecrow-esp32-s3 -t upload
pio device monitor -b 115200
```

Expected startup output: `WiFi connected, IP: ...`, `Audio buffer: N bytes
(~M ms)`, `TTS buffer: N bytes (~30 s)`, then `Touch to speak` on the display.

Each interaction logs the outcome of every stage:

```text
[DIAG] speech=1 peakRMS=... samples=...
Transcript: <what you said>
Answer: <the reply>
Spoke N samples (~N ms)
```

Failures log the request path and the HTTP status, for example
`POST /v1/audio/speech failed (401)`. Successful requests are not logged, so the
console stays readable.

## Notes and tuning

- The ES8311 codec is brought up at 16 kHz by `lib/ES8311`, so the synthesized
  24 kHz PCM returned by OpenAI is resampled to 16 kHz in `audio_io.cpp` before
  playback.
- TLS certificate validation is disabled (`setInsecure()`) for development.
- Speech is captured only after a touch; audio is never written to storage.
