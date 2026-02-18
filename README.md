# PRIDE_V2

PRIDE_V2 is an ESP32-S3 (`Guition JC8048W550`) vehicle dashboard project.
It integrates LVGL UI, BLE OBD-II (ELM327), Wi-Fi/NTP clock sync, and DFPlayer Mini audio with a manager-based architecture.

## Features

- 800x480 LVGL dashboard UI
- SD splash GIF playback (`/anim/splash.gif`) with PSRAM preload
- BLE OBD-II (ELM327) connect/reconnect and PID polling
  - RPM (fast heartbeat), coolant temp, battery voltage, distance
- Goodbye screen + sound on OBD link loss
- Gauge screen reboot sequence after OBD recovery
- Wi-Fi icon updates by RSSI (`wifi_off`, `wifi_1`, `wifi_2`, `wifi_3`, `wifi_full`)
- NTP sync every hour with local `HH:MM` clock updates
- DFPlayer Mini:
  - Welcome track (`1`) once after successful init
  - Goodbye track (`2`) on stable-session link loss

## Hardware / Software Stack

- MCU: ESP32-S3
- Display: Guition JC8048W550 RGB panel (800x480)
- Framework: Arduino (PlatformIO)
- GUI: LVGL 8.3.11
- Display driver: Arduino_GFX
- OBD: NimBLE + ELMDuino
- Audio: DFRobotDFPlayerMini

## Repository Layout

```text
include/
  CommonApi.h        # Event bus + shared state (SystemAPI)
  DisplayMgr.h
  StorageMgr.h
  BluetoothMgr.h
  ObdMgr.h
  Mp3Mgr.h
  WifiMgr.h
src/
  main.cpp           # Manager creation/init, serial command handling
  *Mgr.cpp           # Manager implementations
  ui/                # LVGL generated C-array images and UI code
```

## Boot Sequence (Summary)

1. `StorageMgr`: mount SD, scan files, preload `/anim/splash.gif` to PSRAM
2. `Mp3Mgr`: init DFPlayer, play welcome track
3. `BluetoothMgr`: init NimBLE
4. `ObdMgr`: start OBD event loop and connection flow
5. `DisplayMgr`: init RGB/LVGL, play splash, switch to gauge UI
6. `WifiMgr`: connect AP, start time/status worker tasks

## Architecture

`SystemAPI` singleton provides:

- Manager registration
- Event subscriber queues (`bt`, `sound`, `display`, `storage`)
- Shared UI state (`UiSharedState`) publish/snapshot
- GIF/LVGL/UI mutexes

UI writes are centralized through `DisplayMgr` (LVGL task). Other managers publish state/events.

## Task / Core Mapping

Current mapping separates UI and comm workloads:

- Core 1:
  - `GifTask` (reused as `LvglTask`)
  - `DisplaySub`
  - `QueryOBDData`
- Core 0:
  - `ObdEventTask`, `ConnectBTTask`
  - `BT_Sub`, `ConnectOBDTask`
  - `WifiConnectTask`, `TimeUpdateTask`, `WifiStatusUpdateTask`
  - `Mp3EventSubscriber`
  - `StorageEventSubscriber`

## OBD Policy

- Polling in `QueryOBDData`:
  - RPM: every `500 ms`
  - Voltage/Coolant/Distance: every `30 s`
- Value is applied only on `ELM_SUCCESS`
- On stable-session link loss:
  - `DISPLAY_SHOW_GOODBYE`
  - Play track `2`
  - BLE disconnect and reconnect loop
- RPM first-success is used as session recovery signal

## Wi-Fi / Time Policy

- Before connect: `wifi_off`
- After connect: RSSI bucket icon update (`-70` to `-40 dBm` range)
- NTP servers: `pool.ntp.org`, `time.nist.gov`
- Timezone: KST (`configTzTime("KST-9", ...)`)
- NTP sync interval: 1 hour
- Clock label update: minute-level from local RTC time

## Build Environment

From `platformio.ini`:

- platform: `https://github.com/pioarduino/platform-espressif32.git#55.03.37`
- framework: `arduino`
- board: `Guition_JC8048W550`
- key flags:
  - `LVGL_TFT_DISPLAY_WIDTH=800`
  - `LVGL_TFT_DISPLAY_HEIGHT=480`
  - `LVGL_TFT_DISPLAY_BUFFER_SIZE=8192`
  - `LV_GIF_SPEED_FACTOR=3`
  - `ARDUINO_GFX_RGB_PANEL_NUM_FBS=2`

## Build / Upload

```bash
pio run -e Guition_JC8048W550
pio run -e Guition_JC8048W550 -t upload
pio device monitor -b 115200
```

## SD Card Requirement

At minimum:

```text
/anim/splash.gif
```

## Wi-Fi Credentials (Local, Not in Source)

Wi-Fi credentials are loaded from a local text file at project root:

- `wifi_credentials.txt`

Format:

```text
SSID1=your_primary_ssid
PASSWORD1=your_primary_password
SSID2=your_secondary_ssid
PASSWORD2=your_secondary_password
```

Retry behavior:

- `WifiMgr` tries credential set `#1` first
- if failed, it tries `#2`
- then repeats in round-robin order (`1 -> 2 -> 1 -> 2 ...`)

Build flow:

- `extra_scripts = pre:scripts/load_wifi_credentials.py` reads `wifi_credentials.txt`
- Generates `include/WifiCredentialsLocal.h` before compile
- `WifiMgr` reads generated macros (`WIFI_CRED_SSID`, `WIFI_CRED_PASSWORD`)

Security:

- `wifi_credentials.txt` and `include/WifiCredentialsLocal.h` are git-ignored
- Credentials are no longer hardcoded in `include/WifiMgr.h`

## Serial Commands

Handled in `loop()`:

- `reset`: reboot device
- `outtemp`: query OBD `ambientAirTemp()`
  - `ELM_SUCCESS`: print temperature
  - otherwise (`NO_DATA`, etc.): print `not supported query`

## Configuration Points

- Wi-Fi SSID/PW: `wifi_credentials.txt` (project root)
- OBD reconnect interval: `include/ObdMgr.h` (`OBD_RECONNECT_INTERVAL_MS`)
- OBD poll intervals: `src/ObdMgr.cpp` (`QueryOBDData()`)
- GIF/UI tuning: `src/DisplayMgr.cpp`, `lv_conf.h`

## Log Prefixes

- `[Main]`
- `[SystemAPI]`
- `[DisplayMgr]`
- `[ObdMgr]`
- `[BluetoothMgr]`
- `[WifiMgr]`
- `[StorageMgr]`
- `[Mp3Mgr]`

## Notes

- Hardcoded Wi-Fi credentials are a security risk.
- ELM327 clone quality strongly affects latency/stability.
- SD card quality/speed affects splash startup latency.
