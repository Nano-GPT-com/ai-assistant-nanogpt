# ai-assistant-nanogpt

**AI Assistant (NanoGPT)** · v1.1.0

Voice-to-text AI assistant — NanoGPT for speech-to-text and the reply. NanoGPT can call device tools and optional web search.

**Hardware:** Waveshare ESP32-S3 1.8" AMOLED Touch

**Tags:** `#ai` `#voice`

Voice-to-text assistant that uses **NanoGPT** for both speech-to-text and the reply. Hold **BOOT** to speak; release to get an answer.

Sibling to [**AI Chat**](/apps/ai-chat) — same UI, same controls. The NanoGPT variant can call **device + web tools** to give grounded answers (battery, time, weather, notes, web search) instead of guessing.

**Cost.** Depends on the NanoGPT model you select. Tool round-trips add another model call; web search has its own search charge. Check current pricing at [nano-gpt.com/pricing](https://nano-gpt.com/pricing).

## Tools the assistant can call

- **Device state** — get time, battery, uptime, WiFi signal, orientation
- **Device control** — set brightness, play beep, restart, power off
- **Weather** — current conditions for `LOCATION_1` (no API key needed)
- **Notes** — save / list notes on SD card (`/notes/YYYY-MM-DD.txt`)
- **Web search** — NanoGPT-hosted; toggled with PWR on the splash

## `setup.txt` keys

**Mandatory**
- `SSID` / `PASSWORD` — WiFi
- `NANOGPT_KEY` — NanoGPT API key from [nano-gpt.com/api](https://nano-gpt.com/api) — speech-to-text and chat reply

**Optional**
- `LANGUAGE` — ISO-639-1 hint for Whisper (`de`, `en`, …). Omit for auto-detect.
- `NANOGPT_MODEL` — NanoGPT model id. Defaults to `openai/gpt-chat-latest`.
- `NANOGPT_STT_MODEL` — NanoGPT speech-to-text model id. Defaults to `Whisper-Large-V3`.
- `TIMEZONE` — POSIX TZ string for the time tool (e.g. `CET-1CEST,M3.5.0,M10.5.0/3`). Defaults to UTC.
- `LOCATION_1` — city name for the `get_weather` tool (geocoded on first use).
- `NANOGPT_WEBSEARCH` — `0`/`off`/`no`/`false` disables web search by default. Anything else (incl. missing) leaves it on.

## How to get a NanoGPT API key (2 minutes)

1. Go to [**nano-gpt.com/api**](https://nano-gpt.com/api).
2. Sign in or create an account.
3. Create an API key. Name it something like `esp32-app-pixels` so you can rotate it later.
4. Paste it into `/setup/setup.txt` on the SD card as:
   ```
   NANOGPT_KEY = your_nano_gpt_api_key
   ```

## Controls

- **BOOT** (hold) — talk; release to send.
- **BOOT** (tap) — scroll long answers half a page; tap past the bottom returns to the top.
- **PWR** (short) — on the splash: toggle web search on/off. After the first reply: start a new conversation.

## Editing `setup.txt`

The device reads `/setup/setup.txt` from the SD card on boot. [Download a working sample](https://sosbxffigpteqilpgxwn.supabase.co/storage/v1/object/public/app-assets/setup/setup.txt) — covers every app — and edit the keys you need.

Don't want to eject the card? Use the [**USB Stick**](/apps/usb-stick) app (mounts the SD card as a USB drive over USB-C) or the [**Filehub**](/apps/filehub) app (edit over WiFi).

## Build

1. Install [arduino-cli](https://arduino.github.io/arduino-cli/) or Arduino IDE 2.x.
2. Add the ESP32 board package (≥ 3.1.0):

   ```
   arduino-cli core update-index --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
   arduino-cli core install esp32:esp32 --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```

3. Install the required Arduino libraries:

   - ArduinoJson (bblanchon)
   - GFX Library for Arduino (moononournation)
   - SensorLib (lewishe)
   - XPowersLib (lewishe)

4. Compile and upload:

   ```
   FQBN='esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,PSRAM=opi,FlashSize=16M,FlashMode=qio,PartitionScheme=app3M_fat9M_16MB,UploadSpeed=921600,LoopCore=1,EventsCore=1'
   arduino-cli compile -b "$FQBN" --build-path /tmp/ai-assistant-nanogpt_build .
   arduino-cli upload  -b "$FQBN" --input-dir /tmp/ai-assistant-nanogpt_build -p /dev/ttyACM0 .
   ```

   For browser flashing without a build environment, use the [pre-built binary](https://www.app-pixels.com/apps/ai-assistant-nanogpt).

## License

MIT — see [LICENSE](LICENSE). Do whatever you want with it.

---

Part of the [app-pixels.com](https://www.app-pixels.com) catalogue · live listing: https://www.app-pixels.com/apps/ai-assistant-nanogpt
