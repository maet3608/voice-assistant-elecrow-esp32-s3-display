# Elecrow ESP32-S3 AI Voice Assistant

Touch the screen to record a spoken phrase; the audio is transcribed with the
OpenAI transcription API and the result is shown on the 320x480 display.


## Build and upload

```bat
pio run -e elecrow-esp32-s3
pio run -e elecrow-esp32-s3 -t upload
pio device monitor -b 115200
```

Expected startup output: `WiFi connected, IP: ...`, `Audio buffer: N bytes
(~M ms)`, then `Touch to speak` on the display. Each interaction logs
`[DIAG] speech=... peakRMS=... samples=...` followed by
`--- OpenAI response (status 200, N bytes) ---` and the transcript.
