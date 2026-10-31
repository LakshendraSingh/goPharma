#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <ESP32Servo.h>
#include <EEPROM.h>
#include <ArduinoJson.h>

// --- PIN DEFINITIONS ---
const int SERVO_PIN  = 18;  // SG90 Servo PWM signal
const int BUZZER_PIN = 19;  // Piezo Buzzer (+)
const int LED_PIN    = 23;  // Bare LED Anode (+)
const int SDA_PIN    = 21;  // DS3231 I2C SDA
const int SCL_PIN    = 22;  // DS3231 I2C SCL

// Bare LED safe brightness (0-255) to protect pin without resistor
const int SAFE_LED_DUTY = 7;

// --- WI-FI SETTINGS (SoftAP Hotspot) ---
const char* ssid     = "goPharma";
const char* password = "Pharmacy@123";

WebServer server(80);
Servo myServo;

// --- HARDWARE & CLOCK STATUS ---
bool rtcFound = false;
unsigned long timeOffset = 0;       // Epoch seconds baseline
unsigned long lastEpochSyncedAt = 0; // millis() when synced

// --- PRESCRIBED SCHEDULE DATA ---
struct ScheduleItem {
  char medName[20];
  int hour;
  int minute;
};

#define MAX_SCHEDULES 7
ScheduleItem schedules[MAX_SCHEDULES];
int scheduleCount = 0;
bool triggeredToday[MAX_SCHEDULES] = {false};
int lastTrackedDay = -1;
String lastDispenseLog = "None";

// --- EEPROM STORAGE ---
// Memory Layout: [0] = count, then 24 bytes per record
const int EEPROM_SIZE = 256;

void saveSchedulesToEEPROM() {
  EEPROM.write(0, (uint8_t)scheduleCount);
  for (int i = 0; i < scheduleCount; i++) {
    int addr = 1 + (i * sizeof(ScheduleItem));
    EEPROM.put(addr, schedules[i]);
  }
  EEPROM.commit();
}

void loadSchedulesFromEEPROM() {
  uint8_t count = EEPROM.read(0);
  if (count > MAX_SCHEDULES || count == 255) {
    scheduleCount = 0;
    return;
  }
  scheduleCount = count;
  for (int i = 0; i < scheduleCount; i++) {
    int addr = 1 + (i * sizeof(ScheduleItem));
    EEPROM.get(addr, schedules[i]);
    triggeredToday[i] = false;
  }
}

// --- SAFE PWM LED CONTROL ---
void setSafeLED(bool state) {
  #if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
    ledcWrite(LED_PIN, state ? SAFE_LED_DUTY : 0);
  #else
    ledcWrite(0, state ? SAFE_LED_DUTY : 0);
  #endif
}

// --- RTC (DS3231) DETECTION & ACCESS ---
bool checkRTCExists() {
  Wire.beginTransmission(0x68);
  return (Wire.endTransmission() == 0);
}

byte bcdToDec(byte val) { return ((val / 16 * 10) + (val % 16)); }
byte decToBcd(byte val) { return ((val / 10 * 16) + (val % 10)); }

void setHardwareTime(int hr, int min, int sec, int yr, int mth, int day) {
  // Update internal virtual clock
  struct tm t = {0};
  t.tm_year = yr - 1900;
  t.tm_mon  = mth - 1;
  t.tm_mday = day;
  t.tm_hour = hr;
  t.tm_min  = min;
  t.tm_sec  = sec;
  timeOffset = mktime(&t);
  lastEpochSyncedAt = millis();

  // Update physical RTC if plugged in
  if (rtcFound) {
    Wire.beginTransmission(0x68);
    Wire.write(0x00);
    Wire.write(decToBcd(sec));
    Wire.write(decToBcd(min));
    Wire.write(decToBcd(hr));
    Wire.write(decToBcd(1)); // Day of week placeholder
    Wire.write(decToBcd(day));
    Wire.write(decToBcd(mth));
    Wire.write(decToBcd(yr - 2000));
    Wire.endTransmission();
  }
}

void getSystemTime(int &hr, int &min, int &sec, int &day) {
  if (rtcFound) {
    Wire.beginTransmission(0x68);
    Wire.write(0x00);
    Wire.endTransmission();
    Wire.requestFrom(0x68, 7);
    if (Wire.available() >= 7) {
      sec = bcdToDec(Wire.read() & 0x7F);
      min = bcdToDec(Wire.read());
      hr  = bcdToDec(Wire.read() & 0x3F);
      Wire.read(); // Skip day of week
      day = bcdToDec(Wire.read());
      return;
    }
  }

  // Fallback: Virtual internal timer
  unsigned long currentEpoch = timeOffset + ((millis() - lastEpochSyncedAt) / 1000UL);
  sec = currentEpoch % 60;
  min = (currentEpoch / 60) % 60;
  hr  = (currentEpoch / 3600) % 24;
  day = (currentEpoch / 86400);
}

// --- ALERT PATTERN & DISPENSE ROUTINE ---
void playJingle() {
  int melody[] = {523, 659, 784};
  int durations[] = {150, 150, 200};
  
  for (int repeat = 0; repeat < 2; repeat++) {
    for (int i = 0; i < 3; i++) {
      setSafeLED(true);
      tone(BUZZER_PIN, melody[i], durations[i]);
      delay(durations[i]);
      noTone(BUZZER_PIN);
      setSafeLED(false);
      delay(30);
    }
    delay(150);
  }
}

void dispense(const char* medName) {
  Serial.printf("Dispensing medication: %s\n", medName);

  // Exact rotation from your calibration:
  myServo.write(180);
  delay(150);
  myServo.write(90);

  // Sound and visual alert
  playJingle();

  int h, m, s, d;
  getSystemTime(h, m, s, d);
  char buf[32];
  snprintf(buf, sizeof(buf), "%02d:%02d (%s)", h, m, medName);
  lastDispenseLog = String(buf);
}

// --- EMBEDDED DASHBOARD (HTML / JS / CSS) ---
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Smart Med Carousel</title>
  <style>
    body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; background: #121212; color: #e0e0e0; margin: 0; padding: 20px; }
    .card { background: #1e1e1e; padding: 20px; border-radius: 12px; box-shadow: 0 4px 15px rgba(0,0,0,0.5); max-width: 520px; margin: 0 auto 20px; }
    h2, h3, h4 { color: #00d9ff; margin-top: 0; }
    input, select, button { width: 100%; padding: 12px; margin: 8px 0; border-radius: 6px; box-sizing: border-box; font-size: 15px; border: 1px solid #333; }
    input, select { background: #2c2c2c; color: #fff; }
    button { background: #00d9ff; color: #000; font-weight: bold; cursor: pointer; border: none; }
    button:hover { background: #5ce6ff; }
    .danger { background: #ff4d4d; color: white; }
    .secondary { background: #444; color: white; }
    .hidden { display: none; }
    .med-row { display: flex; justify-content: space-between; align-items: center; padding: 10px 0; border-bottom: 1px solid #2a2a2a; }
    .badge { padding: 4px 8px; border-radius: 4px; font-size: 12px; font-weight: bold; }
    .badge-ok { background: #1e4620; color: #75b798; }
    .badge-pending { background: #4d3800; color: #ffda6a; }
    .status-text { font-size: 13px; color: #888; text-align: center; }
  </style>
</head>
<body>

  <!-- LOGIN VIEW -->
  <div id="loginCard" class="card">
    <h2>Med Carousel Login</h2>
    <p>Select your portal access:</p>
    <select id="userRole">
      <option value="patient">Patient Portal</option>
      <option value="doctor">Doctor Portal</option>
    </select>
    <div id="doctorPassGroup" class="hidden">
      <input type="password" id="docPass" placeholder="Doctor Passcode (default: 1234)">
    </div>
    <button onclick="login()">Enter Dashboard</button>
  </div>

  <!-- MAIN DASHBOARD -->
  <div id="dashboardCard" class="card hidden">
    <div style="display:flex; justify-content:space-between; align-items:center;">
      <h3 id="portalHeader">Portal</h3>
      <button class="secondary" style="width:auto; padding:6px 12px;" onclick="location.reload()">Logout</button>
    </div>
    <div class="status-text" id="syncStatus">Auto-syncing time...</div>
    <hr style="border: 0; border-top: 1px solid #333; margin: 15px 0;">

    <p><strong>System Clock:</strong> <span id="clock" style="color:#00d9ff;">--:--:--</span></p>
    <p><strong>Last Dispensed:</strong> <span id="lastDisp">None</span></p>

    <!-- DOCTOR CONTROLS -->
    <div id="doctorSection" class="hidden">
      <hr style="border: 0; border-top: 1px solid #333; margin: 15px 0;">
      <h4>Prescribe Medication</h4>
      <input type="text" id="prescName" placeholder="Medicine Name (e.g. Aspirin)">
      <input type="time" id="prescTime">
      <button onclick="addSchedule()">Save Prescription</button>
      <button class="secondary" onclick="manualDispense()">Test 150ms Dispense</button>
    </div>

    <!-- SCHEDULE LIST -->
    <hr style="border: 0; border-top: 1px solid #333; margin: 15px 0;">
    <h4>Today's Prescriptions</h4>
    <div id="scheduleContainer">Loading...</div>
  </div>

<script>
let role = 'patient';

document.getElementById('userRole').addEventListener('change', function() {
  document.getElementById('doctorPassGroup').className = this.value === 'doctor' ? '' : 'hidden';
});

function syncTime() {
  const d = new Date();
  const url = `/sync?hour=${d.getHours()}&minute=${d.getMinutes()}&second=${d.getSeconds()}&year=${d.getFullYear()}&month=${d.getMonth()+1}&day=${d.getDate()}`;
  fetch(url).then(() => {
    document.getElementById('syncStatus').innerText = 'Synced with device clock: ' + d.toLocaleTimeString();
  });
}

function login() {
  role = document.getElementById('userRole').value;
  if(role === 'doctor') {
    if(document.getElementById('docPass').value !== '1234') {
      alert('Invalid doctor passcode');
      return;
    }
    document.getElementById('doctorSection').className = '';
  }
  document.getElementById('loginCard').className = 'hidden';
  document.getElementById('dashboardCard').className = 'card';
  document.getElementById('portalHeader').innerText = (role === 'doctor' ? 'Doctor Portal' : 'Patient Portal');
  
  syncTime();
  fetchStatus();
}

function fetchStatus() {
  fetch('/status')
    .then(r => r.json())
    .then(data => {
      document.getElementById('clock').innerText = data.time;
      document.getElementById('lastDisp').innerText = data.lastDispensed;

      const container = document.getElementById('scheduleContainer');
      if (data.items.length === 0) {
        container.innerHTML = '<span class="status-text">No active schedules saved.</span>';
        return;
      }
      container.innerHTML = data.items.map((item, idx) => `
        <div class="med-row">
          <div>
            <strong>${item.name}</strong><br>
            <span style="font-size:13px; color:#aaa;">${String(item.h).padStart(2,'0')}:${String(item.m).padStart(2,'0')}</span>
          </div>
          <div>
            ${item.done ? '<span class="badge badge-ok">Dispensed</span>' : '<span class="badge badge-pending">Pending</span>'}
            ${role === 'doctor' ? `<button class="danger" style="width:auto; padding:4px 8px; margin-left:8px;" onclick="delSchedule(${idx})">Del</button>` : ''}
          </div>
        </div>
      `).join('');
    });
}

function addSchedule() {
  const name = document.getElementById('prescName').value;
  const time = document.getElementById('prescTime').value;
  if(!name || !time) { alert('Enter medicine name and time.'); return; }
  const [h, m] = time.split(':');
  fetch(`/add?name=${encodeURIComponent(name)}&h=${h}&m=${m}`)
    .then(() => {
      document.getElementById('prescName').value = '';
      fetchStatus();
    });
}

function delSchedule(idx) {
  fetch(`/del?idx=${idx}`).then(() => fetchStatus());
}

function manualDispense() {
  fetch('/dispense-manual').then(() => alert('Manual dispense sequence triggered'));
}

setInterval(fetchStatus, 3000);
</script>
</body>
</html>
)rawliteral";

// --- API ROUTES ---
void setupWebServer() {
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", INDEX_HTML);
  });

  server.on("/status", HTTP_GET, []() {
    int h, m, s, d;
    getSystemTime(h, m, s, d);
    char timeStr[16];
    snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d", h, m, s);

    StaticJsonDocument<1024> doc;
    doc["time"] = timeStr;
    doc["lastDispensed"] = lastDispenseLog;
    JsonArray items = doc.createNestedArray("items");

    for (int i = 0; i < scheduleCount; i++) {
      JsonObject obj = items.createNestedObject();
      obj["name"] = schedules[i].medName;
      obj["h"]    = schedules[i].hour;
      obj["m"]    = schedules[i].minute;
      obj["done"] = triggeredToday[i];
    }

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  server.on("/sync", HTTP_GET, []() {
    if (server.hasArg("hour") && server.hasArg("minute")) {
      int hr  = server.arg("hour").toInt();
      int min = server.arg("minute").toInt();
      int sec = server.hasArg("second") ? server.arg("second").toInt() : 0;
      int yr  = server.hasArg("year") ? server.arg("year").toInt() : 2026;
      int mth = server.hasArg("month") ? server.arg("month").toInt() : 1;
      int day = server.hasArg("day") ? server.arg("day").toInt() : 1;

      setHardwareTime(hr, min, sec, yr, mth, day);
      server.send(200, "text/plain", "OK");
      return;
    }
    server.send(400, "text/plain", "Bad Request");
  });

  server.on("/add", HTTP_GET, []() {
    if (scheduleCount < MAX_SCHEDULES && server.hasArg("name") && server.hasArg("h") && server.hasArg("m")) {
      String name = server.arg("name");
      strncpy(schedules[scheduleCount].medName, name.c_str(), sizeof(schedules[scheduleCount].medName) - 1);
      schedules[scheduleCount].hour = server.arg("h").toInt();
      schedules[scheduleCount].minute = server.arg("m").toInt();
      triggeredToday[scheduleCount] = false;
      scheduleCount++;
      saveSchedulesToEEPROM();
      server.send(200, "text/plain", "Added");
      return;
    }
    server.send(400, "text/plain", "Schedule full or invalid parameters");
  });

  server.on("/del", HTTP_GET, []() {
    if (server.hasArg("idx")) {
      int idx = server.arg("idx").toInt();
      if (idx >= 0 && idx < scheduleCount) {
        for (int i = idx; i < scheduleCount - 1; i++) {
          schedules[i] = schedules[i + 1];
          triggeredToday[i] = triggeredToday[i + 1];
        }
        scheduleCount--;
        saveSchedulesToEEPROM();
        server.send(200, "text/plain", "Deleted");
        return;
      }
    }
    server.send(400, "text/plain", "Bad index");
  });

  server.on("/dispense-manual", HTTP_GET, []() {
    dispense("Manual Test");
    server.send(200, "text/plain", "Dispensed");
  });

  server.begin();
}

// --- SETUP & MAIN LOOP ---
void setup() {
  Serial.begin(115200);

  // 1. Initialize PWM for Resistor-less LED
  #if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
    ledcAttach(LED_PIN, 5000, 8);
  #else
    ledcSetup(0, 5000, 8);
    ledcAttachPin(LED_PIN, 0);
  #endif
  setSafeLED(false);

  // 2. Initialize Buzzer & Servo
  pinMode(BUZZER_PIN, OUTPUT);
  myServo.setPeriodHertz(50);
  myServo.attach(SERVO_PIN);
  myServo.write(90); // Neutral / Stop position

  // 3. I2C & RTC Detection
  Wire.begin(SDA_PIN, SCL_PIN);
  rtcFound = checkRTCExists();
  if (rtcFound) {
    Serial.println("DS3231 RTC module connected.");
  } else {
    Serial.println("No RTC found: operating on internal timer.");
  }

  // 4. Load Prescriptions from EEPROM
  EEPROM.begin(EEPROM_SIZE);
  loadSchedulesFromEEPROM();

  // 5. Start Hotspot AP & Web Server
  WiFi.softAP(ssid, password);
  Serial.print("Access Point started. IP Address: ");
  Serial.println(WiFi.softAPIP());

  setupWebServer();
  Serial.println("System initialized.");
}

void loop() {
  server.handleClient();

  // Schedule Checker (Evaluated once per second)
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck >= 1000) {
    lastCheck = millis();

    int h, m, s, day;
    getSystemTime(h, m, s, day);

    // Midnight rollover: reset daily triggers
    if (day != lastTrackedDay) {
      for (int i = 0; i < scheduleCount; i++) {
        triggeredToday[i] = false;
      }
      lastTrackedDay = day;
    }

    // Trigger scheduled dispenses exactly on the minute
    if (s == 0) {
      for (int i = 0; i < scheduleCount; i++) {
        if (schedules[i].hour == h && schedules[i].minute == m && !triggeredToday[i]) {
          triggeredToday[i] = true;
          dispense(schedules[i].medName);
        }
      }
    }
  }
}