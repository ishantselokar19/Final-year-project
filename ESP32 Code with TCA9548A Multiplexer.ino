/*
===========================================================
 ESP32 COMPLETE TRAFFIC SIGNAL SYSTEM
 WITH TCA9548A I2C MULTIPLEXER FOR 4 TM1637 DISPLAYS
===========================================================
*/

#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <time.h>
#include <Wire.h>
#include <TM1637Display.h>
#include <DHT.h>
#include <TinyGPS++.h>
#include <SoftwareSerial.h>

// =========================================================
// WIFI CONFIGURATION
// =========================================================
const char* ssid = "Airtel_sahi_0849";
const char* password = "air99772";

const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 19800;  // IST (UTC+5:30)
const int daylightOffset_sec = 0;

// =========================================================
// SERVER
// =========================================================
WebServer server(80);
WebSocketsServer webSocket(81);

// =========================================================
// TCA9548A I2C MULTIPLEXER
// =========================================================
#define TCA_ADDR 0x70

// TM1637 Display Pins (Connected to TCA9548A channels)
// Each display uses its own I2C channel
const uint8_t displayCLK[4] = {2, 4, 6, 8};
const uint8_t displayDIO[4] = {3, 5, 7, 9};

TM1637Display displays[4] = {
  TM1637Display(displayCLK[0], displayDIO[0]),
  TM1637Display(displayCLK[1], displayDIO[1]),
  TM1637Display(displayCLK[2], displayDIO[2]),
  TM1637Display(displayCLK[3], displayDIO[3])
};

// =========================================================
// SENSORS
// =========================================================
#define DHTPIN 25
#define DHTTYPE DHT11
#define GPS_RX 26
#define GPS_TX 27
#define WATER_SENSOR_PIN 34

DHT dht(DHTPIN, DHTTYPE);
SoftwareSerial gpsSerial(GPS_RX, GPS_TX);
TinyGPSPlus gps;

// =========================================================
// LED PINS - Each road: [Red, Yellow, Green]
// =========================================================
const uint8_t ledPins[4][3] = {
  {2, 4, 5},    // Road 1
  {14, 13, 12}, // Road 2
  {15, 16, 23}, // Road 3
  {18, 19, 17}  // Road 4
};

// =========================================================
// TIMING CONSTANTS
// =========================================================
const uint32_t GREEN_TIME = 20;   // seconds
const uint32_t YELLOW_TIME = 3;   // seconds
const uint32_t CYCLE_DURATION = 4 * (GREEN_TIME + YELLOW_TIME);

// =========================================================
// USER-CONFIGURABLE MAPPINGS
// =========================================================
const uint8_t roadOrder[4] = {0, 1, 2, 3};
const uint8_t displayForRoad[4] = {0, 1, 2, 3};

// Pre-computed start time of green phase for each road
uint32_t greenStart[4];

// =========================================================
// STATE VARIABLES
// =========================================================
uint32_t cycleTime = 0;
uint32_t phaseStartMillis = 0;
uint8_t currentPhase = 0;
uint32_t phaseDurations[8];
uint8_t activeRoad[8];
bool isYellow[8];

int8_t lastDisplayValue[4] = {-1, -1, -1, -1};

// LED Status
bool ledStatus[12] = {
  true, true, true, true,
  true, true, true, true,
  true, true, true, true
};

// Sensor Data
float temperature = 0;
float humidity = 0;
int waterLevel = 0;
String gpsData = "0,0";
uint32_t systemTime = 0;

bool faultsDetected = false;
String faultList = "";

// =========================================================
// TCA9548A FUNCTIONS
// =========================================================
void tcaSelect(uint8_t channel) {
  if(channel > 7) return;
  Wire.beginTransmission(TCA_ADDR);
  Wire.write(1 << channel);
  Wire.endTransmission();
}

// =========================================================
// LED CONTROL FUNCTIONS
// =========================================================
void setRoadLEDs(uint8_t road, uint8_t redState, uint8_t yellowState, uint8_t greenState) {
  digitalWrite(ledPins[road][0], redState);
  digitalWrite(ledPins[road][1], yellowState);
  digitalWrite(ledPins[road][2], greenState);
}

// Update LEDs for all roads based on current phase
void updateLEDs(uint8_t phase) {
  uint8_t road = activeRoad[phase];
  bool yellow = isYellow[phase];

  // All roads start with red off
  for (uint8_t r = 0; r < 4; r++) {
    setRoadLEDs(r, LOW, LOW, LOW);
  }

  // Set the active road
  if (yellow) {
    setRoadLEDs(road, LOW, HIGH, LOW);   // Yellow
  } else {
    setRoadLEDs(road, LOW, LOW, HIGH);   // Green
  }

  // All other roads remain red
  for (uint8_t r = 0; r < 4; r++) {
    if (r != road) {
      setRoadLEDs(r, HIGH, LOW, LOW);    // Red
    }
  }
}

// Advance to the next phase
void advancePhase() {
  currentPhase = (currentPhase + 1) % 8;
  phaseStartMillis = millis();
  cycleTime += phaseDurations[(currentPhase + 7) % 8];
  cycleTime %= CYCLE_DURATION;
  updateLEDs(currentPhase);
}

// =========================================================
// DISPLAY FUNCTIONS (Through TCA9548A)
// =========================================================

// Show countdown on a specific display via TCA9548A
void showCountdownOnDisplay(int displayIndex, int value) {
  // Select the TCA9548A channel
  tcaSelect(displayIndex);
  
  // Clear and set brightness
  displays[displayIndex].clear();
  displays[displayIndex].setBrightness(0x0A);
  
  // Clamp value
  if (value < 0) value = 0;
  if (value > 99) value = 99;
  
  // Show the value with 2 digits
  displays[displayIndex].showNumberDec(value, false, 2, 0);
}

// Compute and display the countdown for a given road
void updateDisplay(uint8_t road, uint32_t currentTime) {
  uint32_t gStart = greenStart[road];
  uint32_t gEnd = gStart + GREEN_TIME;
  uint32_t yEnd = gEnd + YELLOW_TIME;

  int32_t value = 0;
  if (currentTime >= gStart && currentTime < gEnd) {
    // Green phase
    value = gEnd - currentTime;
  } else if (currentTime >= gEnd && currentTime < yEnd) {
    // Yellow phase
    value = yEnd - currentTime;
  } else {
    // Red phase – time until next green
    uint32_t nextGreen = (currentTime < gStart) ? gStart : (gStart + CYCLE_DURATION);
    value = nextGreen - currentTime;
  }

  // Clamp to 0–99
  if (value < 0) value = 0;
  if (value > 99) value = 99;

  // Get the physical display assigned to this road
  uint8_t dispIdx = displayForRoad[road];

  // Update only if value changed
  if (lastDisplayValue[dispIdx] != value) {
    lastDisplayValue[dispIdx] = value;
    showCountdownOnDisplay(dispIdx, value);
  }
}

// Test all displays through TCA9548A
void testDisplays() {
  Serial.println("Testing displays through TCA9548A...");
  
  for(int ch = 0; ch < 4; ch++) {
    tcaSelect(ch);
    displays[ch].clear();
    displays[ch].setBrightness(0x0F);
    displays[ch].showNumberDec(ch + 1, false, 1, 1);
    delay(300);
  }
  
  for(int ch = 0; ch < 4; ch++) {
    tcaSelect(ch);
    displays[ch].clear();
    displays[ch].showNumberDec(88, false, 2, 0);
    delay(300);
  }
  
  for(int ch = 0; ch < 4; ch++) {
    tcaSelect(ch);
    displays[ch].clear();
  }
  
  Serial.println("Display test complete");
}

// =========================================================
// FAULT DETECTION
// =========================================================
void detectFaults() {
  faultList = "";
  faultsDetected = false;
  
  for(int i = 0; i < 12; i++) {
    if(ledStatus[i] == false) {
      faultsDetected = true;
      faultList += String(ledName(i)) + " ";
    }
  }
  
  if(temperature > 60) {
    faultsDetected = true;
    faultList += "TEMP ";
  }
  
  if(waterLevel > 500) {
    faultsDetected = true;
    faultList += "RAIN ";
  }
}

String ledName(int index) {
  String names[12] = {"R1","R2","R3","R4","Y1","Y2","Y3","Y4","G1","G2","G3","G4"};
  return names[index];
}

// =========================================================
// SENSOR READING FUNCTIONS
// =========================================================
void readSensors() {
  // DHT11
  float temp = dht.readTemperature();
  float hum = dht.readHumidity();
  if (!isnan(temp) && !isnan(hum)) {
    temperature = temp;
    humidity = hum;
  }
  
  // Rain/Water Sensor
  waterLevel = analogRead(WATER_SENSOR_PIN);
  
  // GPS
  while(gpsSerial.available() > 0) {
    gps.encode(gpsSerial.read());
  }
  if (gps.location.isValid()) {
    gpsData = String(gps.location.lat(), 6) + "|" + String(gps.location.lng(), 6);
  }
}

// =========================================================
// WEB SOCKET HANDLERS
// =========================================================
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  if(type == WStype_TEXT) {
    String message = String((char*)payload);
    if(message == "get_status") {
      sendStatusToClient(num);
    }
  }
}

void sendStatusToClient(uint8_t num) {
  String json = "{";
  json += "\"led_status\":[";
  for(int i = 0; i < 12; i++) {
    json += ledStatus[i] ? "1" : "0";
    if(i < 11) json += ",";
  }
  json += "],";
  json += "\"road_state\":[";
  for(int i = 0; i < 4; i++) {
    json += String(roadState(i));
    if(i < 3) json += ",";
  }
  json += "],";
  json += "\"countdown\":[";
  for(int i = 0; i < 4; i++) {
    json += String(getCountdown(i));
    if(i < 3) json += ",";
  }
  json += "],";
  json += "\"temperature\":" + String(temperature, 1) + ",";
  json += "\"humidity\":" + String(humidity, 1) + ",";
  json += "\"water_level\":" + String(waterLevel) + ",";
  json += "\"gps\":\"" + gpsData + "\",";
  json += "\"system_time\":" + String(systemTime) + ",";
  json += "\"phase\":" + String(currentPhase + 1) + ",";
  json += "\"active_road\":" + String(activeRoad[currentPhase] + 1) + ",";
  json += "\"is_yellow\":" + String(isYellow[currentPhase] ? "true" : "false") + ",";
  json += "\"faults_detected\":" + String(faultsDetected ? "true" : "false") + ",";
  json += "\"fault_list\":\"" + faultList + "\"";
  json += "}";
  
  webSocket.sendTXT(num, json);
}

int roadState(int road) {
  if(activeRoad[currentPhase] == road) {
    return isYellow[currentPhase] ? 1 : 2;
  }
  return 0;
}

int getCountdown(int road) {
  uint32_t currentTime = cycleTime + ((millis() - phaseStartMillis) / 1000);
  uint32_t gStart = greenStart[road];
  uint32_t gEnd = gStart + GREEN_TIME;
  uint32_t yEnd = gEnd + YELLOW_TIME;

  if (currentTime >= gStart && currentTime < gEnd) {
    return gEnd - currentTime;
  } else if (currentTime >= gEnd && currentTime < yEnd) {
    return yEnd - currentTime;
  } else {
    uint32_t nextGreen = (currentTime < gStart) ? gStart : (gStart + CYCLE_DURATION);
    return nextGreen - currentTime;
  }
}

// =========================================================
// WEB SERVER
// =========================================================
void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>Traffic Signal Monitor</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body { font-family: Arial, sans-serif; background: #1a1a2e; color: #fff; padding: 20px; }
        .container { max-width: 1400px; margin: 0 auto; }
        h1 { text-align: center; margin-bottom: 20px; color: #ffd700; }
        .time-display { text-align: center; font-size: 24px; color: #00ff00; margin-bottom: 20px; }
        .grid { display: grid; grid-template-columns: repeat(4, 1fr); gap: 20px; }
        .road-card { background: rgba(255,255,255,0.1); border-radius: 10px; padding: 20px; text-align: center; }
        .road-title { font-size: 20px; font-weight: bold; margin-bottom: 10px; }
        .state { padding: 5px 15px; border-radius: 20px; display: inline-block; margin-bottom: 10px; }
        .state-green { background: #00ff00; color: #000; }
        .state-yellow { background: #ffd700; color: #000; }
        .state-red { background: #ff0000; color: #fff; }
        .led-grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 10px; margin-top: 10px; }
        .led-item { padding: 10px; background: rgba(255,255,255,0.05); border-radius: 5px; }
        .led-indicator { width: 40px; height: 40px; border-radius: 50%; margin: 0 auto 5px; }
        .led-on-red { background: #ff0000; box-shadow: 0 0 30px #ff0000; }
        .led-on-yellow { background: #ffd700; box-shadow: 0 0 30px #ffd700; }
        .led-on-green { background: #00ff00; box-shadow: 0 0 30px #00ff00; }
        .led-off { background: #333; }
        .led-fault { background: #ff0000; animation: blink 0.5s infinite; }
        @keyframes blink { 0%,100% { opacity: 1; } 50% { opacity: 0.2; } }
        .sensors { display: grid; grid-template-columns: repeat(4, 1fr); gap: 20px; margin-top: 30px; }
        .sensor-card { background: rgba(255,255,255,0.1); border-radius: 10px; padding: 15px; text-align: center; }
        .sensor-value { font-size: 24px; font-weight: bold; }
        .fault-panel { margin-top: 30px; background: rgba(255,0,0,0.1); border-radius: 10px; padding: 20px; border: 1px solid #ff0000; }
        .fault-panel h3 { color: #ff0000; margin-bottom: 10px; }
        .no-faults { color: #00ff00; text-align: center; padding: 10px; }
        .phase-info { text-align: center; font-size: 18px; color: #ffd700; margin-top: 10px; }
        @media (max-width: 768px) { .grid { grid-template-columns: repeat(2, 1fr); } .sensors { grid-template-columns: repeat(2, 1fr); } }
        @media (max-width: 480px) { .grid { grid-template-columns: 1fr; } .sensors { grid-template-columns: 1fr; } }
    </style>
</head>
<body>
    <div class="container">
        <h1>🚦 Traffic Signal Monitor</h1>
        <div class="time-display" id="timeDisplay">⏱️ System Time: --:--:--</div>
        <div class="phase-info" id="phaseInfo">Phase: -- | Road --: --</div>
        <div class="grid" id="roadGrid"></div>
        <div class="sensors" id="sensorGrid"></div>
        <div class="fault-panel" id="faultPanel">
            <h3>⚠️ Fault Alerts</h3>
            <div id="faultContent" class="no-faults">✅ No faults detected</div>
        </div>
    </div>
    <script>
        const ws = new WebSocket('ws://' + window.location.hostname + ':81');
        ws.onopen = function() { ws.send('get_status'); };
        ws.onmessage = function(event) {
            const data = JSON.parse(event.data);
            updateDashboard(data);
        };
        function updateDashboard(data) {
            const hours = Math.floor(data.system_time / 3600);
            const minutes = Math.floor((data.system_time % 3600) / 60);
            const seconds = data.system_time % 60;
            document.getElementById('timeDisplay').textContent = 
                '⏱️ System Time: ' + 
                String(hours).padStart(2, '0') + ':' + 
                String(minutes).padStart(2, '0') + ':' + 
                String(seconds).padStart(2, '0');
            document.getElementById('phaseInfo').textContent = 
                'Phase: ' + data.phase + '/8 | Road ' + data.active_road + ': ' + 
                (data.is_yellow ? 'YELLOW' : 'GREEN') + ' | Countdown: ' + 
                data.countdown.join('s | ') + 's';
            const grid = document.getElementById('roadGrid');
            let html = '';
            const states = ['RED', 'YELLOW', 'GREEN'];
            const colors = ['red', 'yellow', 'green'];
            const ledColors = ['R', 'Y', 'G'];
            for(let road = 0; road < 4; road++) {
                const state = states[data.road_state[road]] || 'OFF';
                const stateClass = 'state-' + state.toLowerCase();
                html += '<div class="road-card">';
                html += '<div class="road-title">Road ' + (road + 1) + ' (' + data.countdown[road] + 's)</div>';
                html += '<div class="state ' + stateClass + '">' + state + '</div>';
                html += '<div class="led-grid">';
                for(let color = 0; color < 3; color++) {
                    const idx = road + (color * 4);
                    const isOn = data.road_state[road] === color;
                    const isOk = data.led_status[idx] === 1;
                    const ledClass = isOn ? 'led-on-' + colors[color] : 'led-off';
                    const faultClass = isOk ? '' : 'led-fault';
                    html += '<div class="led-item">';
                    html += '<div class="led-indicator ' + ledClass + ' ' + faultClass + '"></div>';
                    html += '<div>' + ledColors[color] + ': ' + (isOk ? '✓' : '✗') + '</div>';
                    html += '</div>';
                }
                html += '</div></div>';
            }
            grid.innerHTML = html;
            const sensors = document.getElementById('sensorGrid');
            sensors.innerHTML = `
                <div class="sensor-card"><div>🌡️ Temperature</div><div class="sensor-value">${data.temperature.toFixed(1)}°C</div></div>
                <div class="sensor-card"><div>💧 Humidity</div><div class="sensor-value">${data.humidity.toFixed(1)}%</div></div>
                <div class="sensor-card"><div>💦 Rain/Water Level</div><div class="sensor-value">${data.water_level}</div></div>
                <div class="sensor-card"><div>📍 GPS</div><div class="sensor-value" style="font-size:14px;">${data.gps}</div></div>
            `;
            const faultPanel = document.getElementById('faultContent');
            if(data.faults_detected) {
                faultPanel.className = '';
                faultPanel.innerHTML = '⚠️ ' + data.fault_list;
                faultPanel.style.color = '#ff0000';
            } else {
                faultPanel.className = 'no-faults';
                faultPanel.textContent = '✅ No faults detected';
            }
        }
        setInterval(function() { ws.send('get_status'); }, 1000);
    </script>
</body>
</html>
)rawliteral";
  server.send(200, "text/html", html);
}

// =========================================================
// SETUP
// =========================================================
void setup() {
  Serial.begin(115200);
  Wire.begin();
  
  Serial.println("\n╔═══════════════════════════════════════════════════════════════════╗");
  Serial.println("║              🚦 ESP32 TRAFFIC SIGNAL SYSTEM                      ║");
  Serial.println("╠═══════════════════════════════════════════════════════════════════╣");
  Serial.println("║  TCA9548A: 4x TM1637 Displays                                   ║");
  Serial.println("║  Sensors: DHT11, GPS, Rain/Water                                ║");
  Serial.println("║  12 Traffic LEDs                                                ║");
  Serial.println("╚═══════════════════════════════════════════════════════════════════╝");
  
  // Print pin mapping
  Serial.println("\n📋 LED Pin Mapping:");
  Serial.println("  Road 1: R=GPIO2, Y=GPIO4, G=GPIO5");
  Serial.println("  Road 2: R=GPIO14, Y=GPIO13, G=GPIO12");
  Serial.println("  Road 3: R=GPIO15, Y=GPIO16, G=GPIO23");
  Serial.println("  Road 4: R=GPIO18, Y=GPIO19, G=GPIO17");
  Serial.println("  TCA9548A: SDA=GPIO21, SCL=GPIO22");
  
  // Initialize LED pins
  for (uint8_t r = 0; r < 4; r++) {
    for (uint8_t c = 0; c < 3; c++) {
      pinMode(ledPins[r][c], OUTPUT);
      digitalWrite(ledPins[r][c], LOW);
    }
  }
  Serial.println("✅ LED Pins Initialized");
  
  // Initialize DHT11
  dht.begin();
  Serial.println("✅ DHT11 Initialized");
  
  // Initialize GPS
  gpsSerial.begin(9600);
  Serial.println("✅ GPS Initialized");
  
  // Initialize Displays (through TCA9548A)
  for(int i = 0; i < 4; i++) {
    tcaSelect(i);
    displays[i].setBrightness(0x0A);
    displays[i].clear();
    lastDisplayValue[i] = -1;
  }
  Serial.println("✅ TM1637 Displays Initialized");
  
  // Test displays
  testDisplays();
  
  // Fill phase arrays using roadOrder
  for (uint8_t i = 0; i < 4; i++) {
    uint8_t road = roadOrder[i];
    phaseDurations[2 * i] = GREEN_TIME;
    phaseDurations[2 * i + 1] = YELLOW_TIME;
    activeRoad[2 * i] = road;
    activeRoad[2 * i + 1] = road;
    isYellow[2 * i] = false;
    isYellow[2 * i + 1] = true;
  }

  // Pre-compute greenStart for each road
  for (uint8_t i = 0; i < 4; i++) {
    uint8_t road = roadOrder[i];
    greenStart[road] = i * (GREEN_TIME + YELLOW_TIME);
  }
  
  // WiFi
  Serial.print("\n📡 Connecting to WiFi");
  WiFi.begin(ssid, password);
  int attempts = 0;
  while(WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if(WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ Connected to WiFi");
    Serial.print("📡 IP Address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\n❌ WiFi Connection Failed!");
  }
  
  // NTP
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  Serial.print("⏱️ Waiting for NTP time sync...");
  struct tm timeinfo;
  int retry = 0;
  while(!getLocalTime(&timeinfo) && retry < 10) {
    Serial.print(".");
    delay(500);
    retry++;
  }
  Serial.println(retry < 10 ? " ✅ Time synced!" : " ❌ Time sync failed!");
  
  // Web Server
  server.on("/", handleRoot);
  server.begin();
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
  
  // Start with phase 0
  phaseStartMillis = millis();
  currentPhase = 0;
  cycleTime = 0;
  updateLEDs(currentPhase);
  
  Serial.println("\n🚦 System Started! Web Dashboard available at: " + WiFi.localIP().toString());
}

// =========================================================
// MAIN LOOP
// =========================================================
void loop() {
  uint32_t now = millis();
  uint32_t elapsedSeconds = (now - phaseStartMillis) / 1000;

  // Check if phase duration has elapsed
  if (elapsedSeconds >= phaseDurations[currentPhase]) {
    advancePhase();
    updateLEDs(currentPhase);
    elapsedSeconds = 0;
  }

  // Current time within the cycle (seconds)
  uint32_t currentCycleTime = cycleTime + elapsedSeconds;

  // Update all four displays
  for (uint8_t road = 0; road < 4; road++) {
    updateDisplay(road, currentCycleTime);
  }
  
  // Read sensors every 2 seconds
  static uint32_t lastSensorRead = 0;
  if(now - lastSensorRead >= 2000) {
    lastSensorRead = now;
    readSensors();
  }
  
  // Detect faults
  detectFaults();
  
  // Update time
  struct tm timeinfo;
  if(getLocalTime(&timeinfo)) {
    systemTime = timeinfo.tm_hour * 3600 + timeinfo.tm_min * 60 + timeinfo.tm_sec;
  }
  
  // Web Socket Update
  static uint32_t lastWebSend = 0;
  if(now - lastWebSend >= 1000) {
    lastWebSend = now;
    for(uint8_t i = 0; i < 4; i++) {
      sendStatusToClient(i);
    }
  }

  // Small delay to avoid excessive CPU usage
  delay(50);
}
