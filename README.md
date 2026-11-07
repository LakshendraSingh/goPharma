# goPharma Smart Medication Carousel

An ESP32-based automatic pill dispenser. A 28BYJ-48 stepper rotates a carousel one slot at a time at prescribed times, while a buzzer and LED alert the patient. The device hosts its own Wi-Fi hotspot and a mobile-friendly web dashboard with separate Patient, Doctor, and Technician portals, so no router or internet connection is needed.

![Poster](./goPharma.png)

## Features

- **Scheduled dispensing**: up to 14 daily prescriptions (name + time), triggered exactly at `HH:MM:00`.
- **Audible and visual alert**: a three-note jingle (played twice) with a synchronized LED blink after each dispense.
- **Standalone Wi-Fi hotspot** (SoftAP) with an embedded single-page dashboard.
- **Role-based portals**: Patient (view only), Doctor (manage prescriptions), Technician (motor calibration).
- **Persistent storage**: schedules and calibration are saved to EEPROM and survive power loss.
- **Accurate timekeeping**: uses a DS3231 RTC if present, otherwise falls back to the ESP32's internal timer.
- **Auto time sync**: the dashboard sets the device clock from the connected phone or laptop on login.
- **Daily reset**: "Dispensed" flags reset automatically at midnight.

## Hardware

| Component | Notes |
|---|---|
| ESP32 dev board | Any ESP32 with the Arduino core |
| 28BYJ-48 stepper motor | Half-step mode |
| ULN2003A driver board | Stepper driver |
| DS3231 RTC module | Optional, recommended for reliable time |
| Piezo buzzer | Active or passive |
| LED + resistor | Alert indicator (use ~220 Ω current-limiting resistor) |

### Wiring

| Function | ESP32 GPIO |
|---|---|
| ULN2003 IN1 | 18 |
| ULN2003 IN2 | 5 |
| ULN2003 IN3 | 17 |
| ULN2003 IN4 | 16 |
| Buzzer (+) | 19 |
| LED anode (+) | 23 |
| DS3231 SDA | 21 |
| DS3231 SCL | 22 |

Connect all grounds together. Power the ULN2003 board from 5 V (the motor draws too much current for the ESP32's 3.3 V pin). The DS3231 uses I2C address `0x68`.

> The stepper is initialized as `AccelStepper(HALF_STEP, IN1, IN3, IN2, IN4)`, which is the correct coil order for the 28BYJ-48. Don't reorder the pins in code.

## Software Requirements

- Arduino IDE (or PlatformIO) with the **ESP32 board package**
- Libraries:
  - [`AccelStepper`](https://www.airspayce.com/mikem/arduino/AccelStepper/)
  - [`ArduinoJson`](https://arduinojson.org/) **v6.x** (the code uses `StaticJsonDocument` and `createNestedArray`, which are v6 APIs)
- Built-in with the ESP32 core: `WiFi`, `WebServer`, `Wire`, `EEPROM`

## Getting Started

1. Install the board package and libraries listed above.
2. Wire the hardware as shown.
3. Open the sketch, select your ESP32 board and port, then upload.
4. Open the Serial Monitor at **115200 baud**. You should see:
   ```
   DS3231 RTC connected.        (or "No RTC found: operating on internal timer.")
   Stepper Config: speed=700 slotSteps=134
   Access Point started. IP Address: 192.168.4.1
   Dispenser ready.
   ```
5. On your phone or laptop, connect to the Wi-Fi network:
   - **SSID:** `goPharma`
   - **Password:** `Pharmacy@123`
6. Browse to **http://192.168.4.1** and choose a portal.

## Using the Dashboard

| Portal | Passcode | What you can do |
|---|---|---|
| Patient | none | View the clock, last dispensed dose, and today's schedule with Pending/Dispensed status |
| Doctor | `1234` | Add or delete prescriptions, run a test dispense (1 slot + alert) |
| Technician | `8888` | Calibrate slot steps and motor speed, run a silent single-slot test |

On login the page syncs the device clock to the connecting device's time. If you're using the internal timer (no RTC), **time is lost on power-off** and must be re-synced by logging in again.

### Adding a prescription

1. Log in as Doctor.
2. Enter the medicine name (max 19 characters) and a time.
3. Tap **Save Prescription**. Up to 14 entries are supported.

Each dispense advances the carousel by **one slot**, so load the carousel so that slots line up with the order of your doses.

### Calibrating the carousel

1. Log in as Technician.
2. Tap **Test 1 Slot (Silent Advance)** and check whether the carousel lands exactly on the next slot.
3. Use **±1 / ±5 Steps** to fine-tune the steps per slot, and **Speed ±50** to adjust the motor speed.
4. Changes are saved to EEPROM immediately.

Defaults: **134 steps/slot**, **700 steps/s**. Valid ranges: speed 100–1200 steps/s; slot steps 10–2000.

## HTTP API

All endpoints use `GET` and are served by the ESP32.

| Endpoint | Parameters | Description |
|---|---|---|
| `/` | none | Dashboard HTML |
| `/status` | none | JSON: time, last dispense, calibration, and schedule list |
| `/sync` | `hour`, `minute`, `second`, `year`, `month`, `day` | Set device (and RTC) time |
| `/add` | `name`, `h`, `m` | Add a schedule |
| `/del` | `idx` | Delete schedule at index |
| `/dispense-manual` | none | Dispense one slot with alert |
| `/slot-test` | none | Advance one slot silently |
| `/nudge` | `d` (±steps) | Adjust steps per slot |
| `/setspeed` | `v` | Set max motor speed |

Example `/status` response:

```json
{
  "time": "08:00:12",
  "lastDispensed": "08:00 (Aspirin)",
  "slotSteps": 134,
  "speed": 700,
  "items": [
    { "name": "Aspirin", "h": 8, "m": 0, "done": true }
  ]
}
```

## EEPROM Layout (512 bytes)

| Address | Contents |
|---|---|
| `0` | Schedule count |
| `1` onward | Schedule entries (`ScheduleItem`: 20-byte name, hour, minute) |
| `480` | Calibration struct (`magic 0xB6`, speed, slotSteps) |

To force calibration back to defaults, change the `magic` byte (`0xB6`) in the code and re-flash.

## Project Structure

Single-file Arduino sketch containing:

- Pin and hardware configuration
- Calibration and schedule persistence (EEPROM)
- RTC / internal-clock time handling
- Stepper control and the alert jingle
- Embedded dashboard (`INDEX_HTML`, stored in flash)
- Web server routes and the main scheduling loop

## Known Limitations and Security Notes

This is a prototype and **should not be used as a certified medical device**.

- **Portal passcodes are checked in the browser only.** The API endpoints have no authentication, so anyone connected to the hotspot can call them directly. For real use, add server-side authentication.
- The Wi-Fi password and passcodes are hard-coded. Change them before deployment.
- Dispensing blocks the main loop while the motor moves and the jingle plays (a few seconds), so the web server is briefly unresponsive.
- The carousel position is tracked in RAM only (`slotsMoved`) and is not restored after a reset.
- There is no confirmation that a dose was actually taken, and no missed-dose handling.
- Without an RTC, the default date is derived from the connected device at sync time and is lost on power cycle.
- Schedules can be modified with simple `GET` requests, which makes them easy to trigger accidentally (for example, by link prefetching). Consider switching state-changing routes to `POST`.

## Ideas for Future Work

- Server-side authentication and HTTPS or token-based access
- Home-position sensor (hall/limit switch) for absolute carousel positioning
- Non-blocking dispense routine
- Missed-dose reminders and repeat alerts
- Multiple doses per medicine with slot mapping
- Optional Wi-Fi STA mode with NTP time sync and push notifications

## License

Add your preferred license here (e.g. MIT).
