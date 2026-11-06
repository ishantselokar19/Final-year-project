/*
===========================================================
 ESP32 MASTER CONTROLLER - FULLY CORRECTED
 Controls Arduino Nano (Sensors + Displays)
 4-Way Traffic Signal with 12 LEDs
===========================================================
*/

#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <time.h>

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
// NANO UART (GPIO17 TX, GPIO25 RX)
// =========================================================
#define NANO_RX 25
#define NANO_TX 17
HardwareSerial NanoSerial(2);

// =========================================================
// LED PINS - YOUR EXACT MAPPING
// =========================================================
// Road 1: R=GPIO2, Y=GPIO4, G=GPIO5
// Road 2: R=GPIO12, Y=GPIO13, G=GPIO14
// Road 3: R=GPIO15, Y=GPIO22, G=GPIO23
// Road 4: R=GPIO18, Y=GPIO19, G=GPIO21

const int ledPins[12] = {
  2, 12, 15, 18,   // RED 1-4
  4, 13, 22, 19,   // YELLOW 1-4
  5, 14, 23, 21    // GREEN 1-4
};

const char* ledName[12] = {
  "R1", "R2", "R3", "R4",
  "Y1", "Y2", "Y3", "Y4",
  "G1", "G2", "G3", "G4"
};

// =========================================================
// TRAFFIC LIGHT TIMING
// =========================================================
const uint32_t GREEN_TIME = 20;
const uint32_t YELLOW_TIME = 3;
const uint32_t CYCLE_DURATION = 4 * (GREEN_TIME + YELLOW_TIME);

uint32_t phaseDurations[8];
uint8_t activeRoad[8];
bool isYellow[8];

uint32_t cycleTime = 0;
uint32_t phaseStartMillis = 0;
uint8_t currentPhase = 0;

// LED Status - FIXED: All 12 LEDs initialized to true
bool ledStatus[12] = {
  true, true, true, true,
  true, true, true, true,
  true, true, true, true
};

int roadState[4] = {0};
int countdown[4] = {0};

// Sensor Data from Nano
float temperature = 0;
float humidity = 0;
int waterLevel = 0;
String gpsData = "0,0";
uint32_t systemTime = 0;

bool nanoConnected = false;
unsigned long lastDataReceived = 0;
unsigned long lastNanoSend = 0;

// =========================================================
// FAULT DETECTION
// =========================================================
bool faultsDetected = false;
String faultList = "";

void detectFaults() {
  faultList = "";
  faultsDetected = false;
  
  for(int i = 0; i < 12; i++) {
    if(ledStatus[i] == false) {
      faultsDetected = true;
      faultList += String(ledName[i]) + " ";
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

// =========================================================
// LED CONTROL
// =========================================================
void setLED(int index, bool state) {
  digitalWrite(ledPins[index], state ? HIGH : LOW);
}

void allLEDsOff() {
  for(int i = 0; i < 12; i++) setLED(i, false);
}

void updateLEDs(uint8_t phase) {
  uint8_t road = activeRoad[phase];
  bool yellow = isYellow[phase];

  allLEDsOff();

  if(yellow) {
    setLED(road + 4, true);
    roadState[road] = 1;
  } else {
    setLED(road + 8, true);
    roadState[road] = 2;
  }

  for(uint8_t r = 0; r < 4; r++) {
    if(r != road) {
      setLED(r, true);
      roadState[r] = 0;
    }
  }
  
  for(int r = 0; r < 4; r++) {
    countdown[r] = (r == road) ? (yellow ? YELLOW_TIME : GREEN_TIME) : 0;
  }
}

void advancePhase() {
  currentPhase = (currentPhase + 1) % 8;
  phaseStartMillis = millis();
  cycleTime += phaseDurations[(currentPhase + 7) % 8];
  cycleTime %= CYCLE_DURATION;
  updateLEDs(currentPhase);
}

// =========================================================
// SEND COMMANDS TO ARDUINO NANO (Every 1 Second)
// =========================================================
void sendToNano() {
  // Send STATE
  String state = "STATE:";
  for(int i = 0; i < 4; i++) {
    state += String(roadState[i]);
    if(i < 3) state += ",";
  }
  NanoSerial.println(state);
  
  // Send COUNT
  String count = "COUNT:";
  for(int i = 0; i < 4; i++) {
    count += String(countdown[i]);
    if(i < 3) count += ",";
  }
  NanoSerial.println(count);
  
  // Send TIME
  struct tm timeinfo;
  if(getLocalTime(&timeinfo)) {
    systemTime = timeinfo.tm_hour * 3600 + timeinfo.tm_min * 60 + timeinfo.tm_sec;
    NanoSerial.print("TIME:");
    NanoSerial.println(systemTime);
  }
  
  Serial.print("[ESP32] COUNT: ");
  Serial.println(count);
}

// =========================================================
// WEB SOCKET HANDLERS - SENDS ACTUAL JSON
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
    json += String(roadState[i]);
    if(i < 3) json += ",";
  }
  json += "],";
  json += "\"countdown\":[";
  for(int i = 0; i < 4; i++) {
    json += String(countdown[i]);
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
  NanoSerial.begin(9600, SERIAL_8N1, NANO_RX, NANO_TX);
  
  Serial.println("\n╔═══════════════════════════════════════════════════════════════════╗");
  Serial.println("║              🚦 ESP32 MASTER - TRAFFIC SIGNAL SYSTEM             ║");
  Serial.println("╠═══════════════════════════════════════════════════════════════════╣");
  Serial.println("║  UART2: GPIO17(TX) → Nano A1(RX)                                ║");
  Serial.println("║         GPIO25(RX) ← Nano A2(TX) via divider                    ║");
  Serial.println("╚═══════════════════════════════════════════════════════════════════╝");
  
  // Print pin mapping
  Serial.println("\n📋 LED Pin Mapping:");
  Serial.println("  Road 1: R=GPIO2, Y=GPIO4, G=GPIO5");
  Serial.println("  Road 2: R=GPIO12, Y=GPIO13, G=GPIO14");
  Serial.println("  Road 3: R=GPIO15, Y=GPIO22, G=GPIO23");
  Serial.println("  Road 4: R=GPIO18, Y=GPIO19, G=GPIO21");
  
  // Initialize LED pins
  for(int i = 0; i < 12; i++) {
    pinMode(ledPins[i], OUTPUT);
    digitalWrite(ledPins[i], LOW);
  }
  Serial.println("✅ LED Pins Initialized");
  
  // Build phase arrays
  for(uint8_t i = 0; i < 4; i++) {
    phaseDurations[2*i] = GREEN_TIME;
    phaseDurations[2*i + 1] = YELLOW_TIME;
    activeRoad[2*i] = i;
    activeRoad[2*i + 1] = i;
    isYellow[2*i] = false;
    isYellow[2*i + 1] = true;
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
  
  // Start traffic light
  phaseStartMillis = millis();
  currentPhase = 0;
  cycleTime = 0;
  updateLEDs(currentPhase);
  
  // Send initial data to Nano
  sendToNano();
  
  Serial.println("\n🚦 System Started! Web Dashboard available at: " + WiFi.localIP().toString());
  Serial.println("📊 Sending commands to Arduino Nano every 1 second...\n");
}

// =========================================================
// MAIN LOOP
// =========================================================
void loop() {
  server.handleClient();
  webSocket.loop();
  
  // RECEIVE DATA FROM ARDUINO NANO
  if(NanoSerial.available()) {
    String data = NanoSerial.readStringUntil('\n');
    data.trim();
    
    if(data.startsWith("DATA:")) {
      String values = data.substring(5);
      int idx1 = values.indexOf(',');
      int idx2 = values.indexOf(',', idx1 + 1);
      int idx3 = values.indexOf(',', idx2 + 1);
      int idx4 = values.indexOf('|', idx3 + 1);
      
      if(idx1 != -1 && idx2 != -1 && idx3 != -1) {
        temperature = values.substring(0, idx1).toFloat();
        humidity = values.substring(idx1 + 1, idx2).toFloat();
        waterLevel = values.substring(idx2 + 1, idx3).toInt();
        if(idx4 != -1) {
          gpsData = values.substring(idx3 + 1, idx4);
        } else {
          gpsData = values.substring(idx3 + 1);
        }
        nanoConnected = true;
        lastDataReceived = millis();
        Serial.print("[ESP32] Data from Nano: T=");
        Serial.print(temperature);
        Serial.print(" H=");
        Serial.print(humidity);
        Serial.print(" W=");
        Serial.println(waterLevel);
      }
    } else if(data.startsWith("ACK:")) {
      Serial.println("[ESP32] Nano ACK: " + data.substring(4));
    }
  }
  
  // CHECK NANO CONNECTION
  if(millis() - lastDataReceived > 10000 && nanoConnected) {
    nanoConnected = false;
    Serial.println("[ESP32] ⚠️ Arduino Nano disconnected!");
  }
  
  // TRAFFIC LIGHT TIMING
  uint32_t now = millis();
  uint32_t elapsedSeconds = (now - phaseStartMillis) / 1000;
  
  if(elapsedSeconds >= phaseDurations[currentPhase]) {
    advancePhase();
    elapsedSeconds = 0;
    sendToNano();
  }
  
  // Update countdowns
  for(int r = 0; r < 4; r++) {
    if(r == activeRoad[currentPhase]) {
      countdown[r] = phaseDurations[currentPhase] - elapsedSeconds;
    }
  }
  
  // SEND DATA TO NANO EVERY 1 SECOND (Synchronized)
  if(now - lastNanoSend >= 1000) {
    lastNanoSend = now;
    sendToNano();
  }
  
  // FAULT DETECTION
  detectFaults();
  
  // WEB SOCKET UPDATE - SEND ACTUAL JSON
  static unsigned long lastWebSend = 0;
  if(now - lastWebSend >= 1000) {
    lastWebSend = now;
    // Broadcast actual status JSON to all clients
    for(uint8_t i = 0; i < 4; i++) {
      sendStatusToClient(i);
    }
  }
  
  delay(50);
}
