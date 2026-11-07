#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <AccelStepper.h>
#include <EEPROM.h>
#include <ArduinoJson.h>
#include <time.h>

// --- PIN DEFINITIONS ---
// ULN2003A Stepper Driver Pins (Safe standard GPIOs)
const int STEP_IN1   = 18;  // Driver IN1
const int STEP_IN2   = 5;   // Driver IN2
const int STEP_IN3   = 17;  // Driver IN3
const int STEP_IN4   = 16;  // Driver IN4

// Peripherals
const int BUZZER_PIN = 19;  // Piezo Buzzer (+)
const int LED_PIN    = 23;  // LED Anode (+)
const int SDA_PIN    = 21;  // DS3231 I2C SDA
const int SCL_PIN    = 22;  // DS3231 I2C SCL

// --- STEPPER MOTOR SETUP ---
// 28BYJ-48 half-step sequence order: IN1, IN3, IN2, IN4
#define HALF_STEP 4
AccelStepper stepper(HALF_STEP, STEP_IN1, STEP_IN3, STEP_IN2, STEP_IN4);

// --- WI-FI SETTINGS (SoftAP Hotspot) ---
const char* ssid     = "goPharma";
const char* password = "Pharmacy@123";

WebServer server(80);

// --- HARDWARE & CLOCK STATUS ---
bool rtcFound = false;
unsigned long timeOffset = 0;
unsigned long lastEpochSyncedAt = 0;

// --- PRESCRIBED SCHEDULE DATA ---
struct ScheduleItem {
  char medName[20];
  int hour;
  int minute;
};

#define MAX_SCHEDULES 14
ScheduleItem schedules[MAX_SCHEDULES];
int scheduleCount = 0;
bool triggeredToday[MAX_SCHEDULES] = {false};
int lastTrackedDay = -1;
String lastDispenseLog = "None";

// --- EEPROM STORAGE ---
const int EEPROM_SIZE = 512;
const int CAL_ADDR    = 480;

struct Calib {
  uint8_t  magic;       // 0xB6 forces reload of calibration settings
  uint16_t speed;       // max speed in steps per second (default: 700)
  uint16_t slotSteps;   // steps per slot (default: 134)
};

// Default calibrated values: speed = 700, slotSteps = 134
Calib cal = {0xB6, 700, 134};
int slotsMoved = 0;

void saveCal() {
  EEPROM.put(CAL_ADDR, cal);
  EEPROM.commit();
}

void loadCal() {
  Calib c;
  EEPROM.get(CAL_ADDR, c);
  if (c.magic == 0xB6 && c.speed >= 100 && c.speed <= 1200 &&
      c.slotSteps >= 20 && c.slotSteps <= 2000) {
    cal = c;
  } else {
    saveCal();
  }
}

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
    schedules[i].medName[sizeof(schedules[i].medName) - 1] = '\0';
    triggeredToday[i] = false;
  }
}

// --- LED CONTROL ---
void setSafeLED(bool state) {
  digitalWrite(LED_PIN, state ? HIGH : LOW);
}

// --- RTC (DS3231) DETECTION & ACCESS ---
bool checkRTCExists() {
  Wire.beginTransmission(0x68);
  return (Wire.endTransmission() == 0);
}

byte bcdToDec(byte val) { return ((val / 16 * 10) + (val % 16)); }
byte decToBcd(byte val) { return ((val / 10 * 16) + (val % 10)); }

void setHardwareTime(int hr, int min, int sec, int yr, int mth, int day) {
  struct tm t = {0};
  t.tm_year = yr - 1900;
  t.tm_mon  = mth - 1;
  t.tm_mday = day;
  t.tm_hour = hr;
  t.tm_min  = min;
  t.tm_sec  = sec;
  timeOffset = mktime(&t);
  lastEpochSyncedAt = millis();

  if (rtcFound) {
    Wire.beginTransmission(0x68);
    Wire.write(0x00);
    Wire.write(decToBcd(sec));
    Wire.write(decToBcd(min));
    Wire.write(decToBcd(hr));
    Wire.write(decToBcd(1));
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
      Wire.read();
      day = bcdToDec(Wire.read());
      return;
    }
  }

  unsigned long currentEpoch = timeOffset + ((millis() - lastEpochSyncedAt) / 1000UL);
  sec = currentEpoch % 60;
  min = (currentEpoch / 60) % 60;
  hr  = (currentEpoch / 3600) % 24;
  day = (currentEpoch / 86400);
}

// --- STEPPER MOVEMENT ---
void moveSteps(long steps) {
  stepper.enableOutputs();
  stepper.setMaxSpeed(cal.speed);
  stepper.setAcceleration(400.0);
  stepper.move(steps);

  while (stepper.distanceToGo() != 0) {
    stepper.run();
    yield();
  }

  stepper.disableOutputs();
}

void moveOneSlot() {
  moveSteps(cal.slotSteps);
  slotsMoved = (slotsMoved + 1) % 14;
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

  moveOneSlot();
  playJingle();

  int h, m, s, d;
  getSystemTime(h, m, s, d);
  char buf[40];
  snprintf(buf, sizeof(buf), "%02d:%02d (%s)", h, m, medName);
  lastDispenseLog = String(buf);
}

// --- EMBEDDED DASHBOARD ---
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
    .tech-btn { background: #ff9800; color: #000; }
    .tech-btn:hover { background: #ffac33; }
    .hidden { display: none; }
    .med-row { display: flex; justify-content: space-between; align-items: center; padding: 10px 0; border-bottom: 1px solid #2a2a2a; }
    .badge { padding: 4px 8px; border-radius: 4px; font-size: 12px; font-weight: bold; }
    .badge-ok { background: #1e4620; color: #75b798; }
    .badge-pending { background: #4d3800; color: #ffda6a; }
    .status-text { font-size: 13px; color: #888; text-align: center; }
    .row2 { display: flex; gap: 8px; }
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
      <option value="technician">Technician Portal (Calibration)</option>
    </select>
    <div id="passGroup" class="hidden">
      <input type="password" id="portalPass" placeholder="Passcode (Doctor: 1234, Tech: 8888)">
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

    <!-- DOCTOR CONTROLS (Prescription management only) -->
    <div id="doctorSection" class="hidden">
      <hr style="border: 0; border-top: 1px solid #333; margin: 15px 0;">
      <h4>Prescribe Medication</h4>
      <input type="text" id="prescName" placeholder="Medicine Name (e.g. Aspirin)">
      <input type="time" id="prescTime">
      <button onclick="addSchedule()">Save Prescription</button>
      <button class="secondary" onclick="manualDispense()">Test Dispense (1 slot + alert)</button>
    </div>

    <!-- TECHNICIAN CONTROLS (Dedicated Calibration Section) -->
    <div id="technicianSection" class="hidden">
      <hr style="border: 0; border-top: 1px solid #333; margin: 15px 0;">
      <h4 style="color:#ff9800;">Technician Calibration</h4>
      <div class="status-text">Slot Steps: <span id="calSteps" style="color:#ff9800; font-weight:bold;">--</span> &nbsp;|&nbsp; Motor Speed: <span id="calSpeed" style="color:#ff9800; font-weight:bold;">--</span> step/s</div>
      
      <button class="tech-btn" onclick="slotTest()">Test 1 Slot (Silent Advance)</button>
      
      <div class="row2">
        <button class="secondary" onclick="nudge(-5)">-5 Steps</button>
        <button class="secondary" onclick="nudge(5)">+5 Steps</button>
      </div>
      <div class="row2">
        <button class="secondary" onclick="nudge(-1)">-1 Step</button>
        <button class="secondary" onclick="nudge(1)">+1 Step</button>
      </div>
      <div class="row2">
        <button class="secondary" onclick="setSpeed(-50)">Speed -50</button>
        <button class="secondary" onclick="setSpeed(50)">Speed +50</button>
      </div>
    </div>

    <!-- SCHEDULE LIST -->
    <hr style="border: 0; border-top: 1px solid #333; margin: 15px 0;">
    <h4>Today's Prescriptions</h4>
    <div id="scheduleContainer">Loading...</div>
  </div>

<script>
let role = 'patient';
let calSpeed = 700;

document.getElementById('userRole').addEventListener('change', function() {
  const needsPass = (this.value === 'doctor' || this.value === 'technician');
  document.getElementById('passGroup').className = needsPass ? '' : 'hidden';
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
  const pass = document.getElementById('portalPass').value;

  if (role === 'doctor') {
    if (pass !== '1234') {
      alert('Invalid doctor passcode');
      return;
    }
    document.getElementById('doctorSection').className = '';
    document.getElementById('technicianSection').className = 'hidden';
  } else if (role === 'technician') {
    if (pass !== '8888') {
      alert('Invalid technician passcode');
      return;
    }
    document.getElementById('doctorSection').className = 'hidden';
    document.getElementById('technicianSection').className = '';
  } else {
    document.getElementById('doctorSection').className = 'hidden';
    document.getElementById('technicianSection').className = 'hidden';
  }

  document.getElementById('loginCard').className = 'hidden';
  document.getElementById('dashboardCard').className = 'card';
  
  if (role === 'doctor') {
    document.getElementById('portalHeader').innerText = 'Doctor Portal';
  } else if (role === 'technician') {
    document.getElementById('portalHeader').innerText = 'Technician Portal';
  } else {
    document.getElementById('portalHeader').innerText = 'Patient Portal';
  }

  syncTime();
  fetchStatus();
}

function fetchStatus() {
  fetch('/status')
    .then(r => r.json())
    .then(data => {
      document.getElementById('clock').innerText = data.time;
      document.getElementById('lastDisp').innerText = data.lastDispensed;
      document.getElementById('calSteps').innerText = data.slotSteps;
      document.getElementById('calSpeed').innerText = data.speed;
      calSpeed = data.speed;

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
  fetch('/dispense-manual').then(() => fetchStatus());
}

function slotTest() {
  fetch('/slot-test').then(() => fetchStatus());
}

function nudge(d) {
  fetch(`/nudge?d=${d}`).then(r => r.text()).then(t => {
    document.getElementById('calSteps').innerText = t;
  });
}

function setSpeed(delta) {
  fetch(`/setspeed?v=${calSpeed + delta}`).then(r => r.text()).then(t => {
    calSpeed = parseInt(t);
    document.getElementById('calSpeed').innerText = t;
  });
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

    StaticJsonDocument<2048> doc;
    doc["time"] = timeStr;
    doc["lastDispensed"] = lastDispenseLog;
    doc["slotSteps"] = cal.slotSteps;
    doc["speed"]     = cal.speed;
    JsonArray items  = doc.createNestedArray("items");

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
      memset(schedules[scheduleCount].medName, 0, sizeof(schedules[scheduleCount].medName));
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

  server.on("/nudge", HTTP_GET, []() {
    int d = server.arg("d").toInt();
    cal.slotSteps = constrain((int)cal.slotSteps + d, 10, 2000);
    saveCal();
    server.send(200, "text/plain", String(cal.slotSteps));
  });

  server.on("/setspeed", HTTP_GET, []() {
    cal.speed = constrain(server.arg("v").toInt(), 100, 1200);
    saveCal();
    server.send(200, "text/plain", String(cal.speed));
  });

  server.on("/slot-test", HTTP_GET, []() {
    moveOneSlot();
    server.send(200, "text/plain", String(cal.slotSteps));
  });

  server.begin();
}

void setup() {
  Serial.begin(115200);

  // Initialize LED & Buzzer
  pinMode(LED_PIN, OUTPUT);
  setSafeLED(false);
  pinMode(BUZZER_PIN, OUTPUT);

  // Initialize Stepper
  stepper.disableOutputs();

  // Initialize RTC
  Wire.begin(SDA_PIN, SCL_PIN);
  rtcFound = checkRTCExists();
  if (rtcFound) {
    Serial.println("DS3231 RTC connected.");
  } else {
    Serial.println("No RTC found: operating on internal timer.");
  }

  // EEPROM loading (magic 0xB6 forces new 134-step default calibration)
  EEPROM.begin(EEPROM_SIZE);
  loadSchedulesFromEEPROM();
  loadCal();
  Serial.printf("Stepper Config: speed=%d slotSteps=%d\n", cal.speed, cal.slotSteps);

  // WiFi AP & Server initialization
  WiFi.softAP(ssid, password);
  Serial.print("Access Point started. IP Address: ");
  Serial.println(WiFi.softAPIP());

  setupWebServer();
  Serial.println("Dispenser ready.");
}

void loop() {
  server.handleClient();

  // Schedule evaluation every 1000ms
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
