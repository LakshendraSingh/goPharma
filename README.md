# goPharma – Smart Medication Carousel

An ESP32-based pill dispenser that rotates a servo-driven carousel at prescribed times, plays an audio/visual alert, and is managed through a built-in web dashboard. The ESP32 creates its own Wi-Fi hotspot, so no home network or internet connection is needed.

## Features

- **Self-hosted Wi-Fi hotspot** (SoftAP) with an embedded web dashboard
- **Two portals**: Patient (view only) and Doctor (add / delete prescriptions, test dispense)
- **Up to 7 scheduled prescriptions**, stored in EEPROM so they survive power loss
- **Automatic dispensing** at the scheduled time, once per day per prescription
- **Alert on dispense**: buzzer jingle plus a flashing LED
- **Timekeeping**: uses a DS3231 RTC if connected, otherwise an internal timer synced from the phone/browser
- **Dispense status** shown live in the dashboard ("Pending" / "Dispensed") along with the last dispense log

## Hardware

| Component | Notes |
|---|---|
| ESP32 dev board | Any ESP32 supported by the Arduino core |
| SG90 servo | Drives the carousel (see calibration note below) |
| Piezo buzzer | Alert jingle |
| LED | Driven with PWM at low duty to protect the pin (see warning below) |
| DS3231 RTC module | Optional, keeps time across power loss |

### Wiring

| Signal | ESP32 Pin |
|---|---|
| Servo PWM | GPIO 18 |
| Buzzer (+) | GPIO 19 |
| LED anode (+) | GPIO 23 |
| DS3231 SDA | GPIO 21 |
| DS3231 SCL | GPIO 22 |

Connect all grounds together, power the servo from a suitable 5 V supply (sharing ground with the ESP32), and power the DS3231 from 3.3 V or 5 V per your module.

> **LED warning:** the sketch is written for an LED wired *without* a series resistor, limiting brightness with PWM (`SAFE_LED_DUTY = 7`). Adding a proper resistor (about 220 Ω to 330 Ω) is strongly recommended. If you add one, you can raise the duty value.

## Software Requirements

- Arduino IDE (or PlatformIO) with the **ESP32 board package** (both Arduino core 2.x and 3.x are supported)
- Libraries:
  - `ESP32Servo`
  - `ArduinoJson` (v6 API is used: `StaticJsonDocument`, `createNestedArray`)
  - `WiFi`, `WebServer`, `Wire`, `EEPROM` (bundled with the ESP32 core)

## Getting Started

1. Install the board package and libraries above.
2. Open `goPharma.ino` and select your ESP32 board and port.
3. (Recommended) Change the default Wi-Fi password and doctor passcode (see [Configuration](#configuration)).
4. Upload the sketch.
5. Open the Serial Monitor at **115200 baud**. You should see the access point IP address (normally `192.168.4.1`).
6. On your phone or laptop, connect to the Wi-Fi network:
   - **SSID:** `goPharma`
   - **Password:** `Pharmacy@123`
7. Browse to `http://192.168.4.1`.

## Using the Dashboard

1. Choose **Patient Portal** or **Doctor Portal**. The doctor portal asks for a passcode (default `1234`).
2. On login, the dashboard syncs the device clock to your phone/laptop time automatically.
3. **Doctors** can:
   - Enter a medicine name and time, then tap **Save Prescription**
   - Delete prescriptions with the **Del** button
   - Tap **Test 150ms Dispense** to trigger the carousel manually
4. Everyone can see the system clock, last dispensed entry, and today's prescription list with status badges. The list refreshes every 3 seconds.

## How It Works

- **Scheduling:** once per second the main loop checks the clock. When the seconds value is `0`, it compares the current hour and minute against each prescription and dispenses any that match and haven't already fired today.
- **Daily reset:** the "dispensed" flags reset when the day value changes.
- **Dispensing:** the servo is driven to `180` for 150 ms and then back to `90` (stop), followed by the buzzer/LED jingle. This assumes a **continuous-rotation servo** calibrated so that 150 ms of rotation advances the carousel by one slot. Adjust the value in `dispense()` to match your mechanism.
- **Storage:** the schedule count is stored at EEPROM address 0, followed by the `ScheduleItem` records (medicine name up to 19 characters, hour, minute).
- **Time source:** the DS3231 is auto-detected at I2C address `0x68` on boot. Without it, the device keeps time from `millis()` after the last browser sync, so the **time resets on power loss until someone opens the dashboard again**.

## HTTP API

All endpoints use `GET`.

| Endpoint | Parameters | Description |
|---|---|---|
| `/` | – | Serves the dashboard |
| `/status` | – | JSON with `time`, `lastDispensed`, and `items[]` (`name`, `h`, `m`, `done`) |
| `/sync` | `hour`, `minute`, `second`, `year`, `month`, `day` | Sets the device (and RTC) time |
| `/add` | `name`, `h`, `m` | Adds a prescription (max 7) |
| `/del` | `idx` | Deletes the prescription at the given index |
| `/dispense-manual` | – | Triggers a dispense immediately |

## Configuration

Edit these values near the top of the sketch:

| Setting | Default | Purpose |
|---|---|---|
| `ssid` / `password` | `goPharma` / `Pharmacy@123` | Hotspot credentials (password must be at least 8 characters) |
| `MAX_SCHEDULES` | `7` | Maximum number of prescriptions |
| `SAFE_LED_DUTY` | `7` | LED brightness (0 to 255) |
| Pin constants | See wiring table | Change if your wiring differs |
| Doctor passcode | `1234` | Hardcoded in the dashboard JavaScript (`login()`) |

## Known Limitations

- **Security is minimal.** The doctor passcode is checked only in browser JavaScript, and the API endpoints have no authentication, so anyone connected to the hotspot can add, delete, or dispense by calling the URLs directly. Change the defaults, and don't rely on this for safety-critical use.
- **Medicine names are limited to 19 characters.**
- **Without an RTC, time is lost on reboot** until the dashboard is opened again.
- **The day value differs by time source:** the DS3231 returns the day of the month, while the fallback timer returns days since the epoch. Both work for the daily reset, but the reset may not fire if the clock is changed in a way that leaves the day value the same.
- **Dispense blocks the main loop** for about 1.5 seconds (servo delay plus jingle), so the web server briefly stops responding.
- **Not a medical device.** This is a hobby/educational project and has not been validated for real medication management.
