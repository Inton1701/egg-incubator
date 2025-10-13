#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include <HardwareSerial.h>

// AP config
const char* AP_SSID = "EggIncubator";
const char* AP_PASS = "12345678";
const byte DNS_PORT = 53;
AsyncWebServer server(80);
DNSServer dns;
Preferences prefs;

// Pin Definitions
// LCD I²C
const int SDA_PIN = 21;
const int SCL_PIN = 22;

// 4-Channel Relay
const int HUMIDIFIER_PIN = 26;
const int FAN_PIN = 25;
const int BULB_PIN = 33;
const int EGG_TURNER_PIN = 32;

// Buzzer
const int BUZZER_PIN = 19;

// SIM900A
const int SIM_RX_PIN = 16;
const int SIM_TX_PIN = 17;

// Sensors
const int AM2302_1_PIN = 14;
const int AM2302_2_PIN = 27;
const int WATER_SENSOR_PIN = 35;

// Function declarations
void sendSMS(String message);
void sendCommand(const char *cmd, unsigned long wait = 2000);
void initializeGSMModule();
void processSMSCommand(String command);
void updateSignalStrengthQuick();

// Component initialization
LiquidCrystal_I2C lcd(0x27, 16, 2);
DHT dht1(AM2302_1_PIN, DHT22);
DHT dht2(AM2302_2_PIN, DHT22);
HardwareSerial sim900a(2);

// System state variables
struct SystemState {
  // Relay states
  bool fanState = false;
  bool eggTurnerState = false;
  bool humidifierState = false;
  bool bulbState = false;
  bool buzzerEnabled = true;
  
  // Auto modes
  bool humidifierAutoMode = false;
  bool bulbAutoMode = false;
  
  // Schedule modes
  bool fanScheduleEnabled = false;
  bool eggTurnerScheduleEnabled = false;
  
  // Sensor values
  float temperature = 0.0;
  float humidity = 0.0;
  bool waterPresent = false;
  
  // Temperature/Humidity thresholds
  float tempTrigger = 37.5;
  float tempStop = 38.0;
  float humidityTrigger = 60.0;
  float humidityStop = 65.0;
  
  // Schedule timers (in seconds)
  int fanRunDuration = 300;    // 5 minutes
  int fanIdleDuration = 300;   // 5 minutes
  int eggTurnerRunDuration = 10; // 10 seconds
  int eggTurnerIdleDuration = 21600; // 6 hours
  
  // Timer tracking
  unsigned long fanLastToggle = 0;
  unsigned long eggTurnerLastToggle = 0;
  bool fanScheduleRunning = false;
  bool eggTurnerScheduleRunning = false;
  
  // SMS
  String phoneNumber = "";
  String signalStrength = "No Signal";
  String simCardNumber = "Unknown";
  bool smsModuleActive = false;
  bool systemOnlineSent = false;
} state;

// LCD display variables
unsigned long lastLCDUpdate = 0;
bool lcdDisplayCycle = false; // false = temp/humidity, true = water/bulb then fan/egg turner

void controlRelay(int pin, bool state) {
  digitalWrite(pin, state ? LOW : HIGH); // LOW = ON, HIGH = OFF (inverted relay)
}

void updateRelayState(int pin, bool &stateVar, bool newState) {
  if (stateVar != newState) {
    stateVar = newState;
    controlRelay(pin, newState);
  }
}

void buzzerBeep(int duration, int count = 1) {
  if (!state.buzzerEnabled) return;
  
  for (int i = 0; i < count; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(duration);
    digitalWrite(BUZZER_PIN, LOW);
    if (i < count - 1) delay(100);
  }
}

void readSensors() {
  // Read DHT sensors
  float temp1 = dht1.readTemperature();
  float temp2 = dht2.readTemperature();
  float hum1 = dht1.readHumidity();
  float hum2 = dht2.readHumidity();
  
  // Calculate average or use available sensor
  int validTempSensors = 0;
  int validHumSensors = 0;
  float tempSum = 0, humSum = 0;
  
  if (!isnan(temp1)) { tempSum += temp1; validTempSensors++; }
  if (!isnan(temp2)) { tempSum += temp2; validTempSensors++; }
  if (!isnan(hum1)) { humSum += hum1; validHumSensors++; }
  if (!isnan(hum2)) { humSum += hum2; validHumSensors++; }
  
  if (validTempSensors > 0) {
    state.temperature = tempSum / validTempSensors;
  } else {
    state.temperature = -999; // Error indicator
  }
  
  if (validHumSensors > 0) {
    state.humidity = humSum / validHumSensors;
  } else {
    state.humidity = -999; // Error indicator
  }
  
  // Read water sensor
  state.waterPresent = digitalRead(WATER_SENSOR_PIN) == HIGH;
}

void handleAutoModes() {
  // Auto bulb control based on temperature
  if (state.bulbAutoMode && state.temperature > -999) {
    if (!state.bulbState && state.temperature < state.tempTrigger) {
      updateRelayState(BULB_PIN, state.bulbState, true);
    } else if (state.bulbState && state.temperature >= state.tempStop) {
      updateRelayState(BULB_PIN, state.bulbState, false);
    }
  }
  
  // Auto humidifier control based on humidity
  if (state.humidifierAutoMode && state.humidity > -999) {
    if (!state.humidifierState && state.humidity < state.humidityTrigger) {
      updateRelayState(HUMIDIFIER_PIN, state.humidifierState, true);
    } else if (state.humidifierState && state.humidity >= state.humidityStop) {
      updateRelayState(HUMIDIFIER_PIN, state.humidifierState, false);
    }
  }
}

void handleScheduleModes() {
  unsigned long currentTime = millis();
  
  // Fan schedule
  if (state.fanScheduleEnabled) {
    unsigned long elapsed = (currentTime - state.fanLastToggle) / 1000;
    
    if (state.fanScheduleRunning) {
      if (elapsed >= state.fanRunDuration) {
        updateRelayState(FAN_PIN, state.fanState, false);
        state.fanScheduleRunning = false;
        state.fanLastToggle = currentTime;
      }
    } else {
      if (elapsed >= state.fanIdleDuration) {
        updateRelayState(FAN_PIN, state.fanState, true);
        state.fanScheduleRunning = true;
        state.fanLastToggle = currentTime;
      }
    }
  }
  
  // Egg turner schedule
  if (state.eggTurnerScheduleEnabled) {
    unsigned long elapsed = (currentTime - state.eggTurnerLastToggle) / 1000;
    
    if (state.eggTurnerScheduleRunning) {
      if (elapsed >= state.eggTurnerRunDuration) {
        updateRelayState(EGG_TURNER_PIN, state.eggTurnerState, false);
        state.eggTurnerScheduleRunning = false;
        state.eggTurnerLastToggle = currentTime;
      }
    } else {
      if (elapsed >= state.eggTurnerIdleDuration) {
        updateRelayState(EGG_TURNER_PIN, state.eggTurnerState, true);
        state.eggTurnerScheduleRunning = true;
        state.eggTurnerLastToggle = currentTime;
      }
    }
  }
}

void sendCommand(const char *cmd, unsigned long wait) {
  sim900a.println(cmd);
  
  unsigned long startTime = millis();
  String response = "";
  
  // Wait for response with timeout
  while (millis() - startTime < wait) {
    if (sim900a.available()) {
      char c = sim900a.read();
      response += c;
    }
    yield(); // Prevent watchdog reset
  }
  
  // Clear any remaining data
  while (sim900a.available()) {
    sim900a.read();
  }
}
void initializeGSMModule() {
  // Initialize HardwareSerial
  sim900a.begin(115200, SERIAL_8N1, SIM_RX_PIN, SIM_TX_PIN);
  delay(3000); // Allow SIM900A to boot
  
  // Basic checks
  sendCommand("AT");
  sendCommand("AT+CSQ");   // Signal quality
  sendCommand("AT+CCID");  // SIM card ID
  sendCommand("AT+CREG?"); // Network registration
  
  // Get SIM phone number
  sim900a.println("AT+CNUM");
  delay(2000);
  
  String response = "";
  while (sim900a.available()) {
    char c = sim900a.read();
    response += c;
  }
  
  // Extract phone number from response
  if (response.indexOf("+CNUM:") >= 0) {
    int firstComma = response.indexOf(",") + 1;
    int secondComma = response.indexOf(",", firstComma);
    if (firstComma > 0 && secondComma > firstComma) {
      state.simCardNumber = response.substring(firstComma, secondComma);
      state.simCardNumber.trim(); // Remove any extra spaces
    }
  }
  
  // Set SMS to text mode
  sendCommand("AT+CMGF=1");
  
  state.smsModuleActive = true;
  
  // Send system online message if phone number configured
  if (!state.systemOnlineSent && state.phoneNumber.length() > 0) {
    delay(2000);
    sendSMS("EGG INCUBATOR ONLINE: System started! SIM: " + state.simCardNumber);
    state.systemOnlineSent = true;
  }
}

void sendSMS(String message) {
  if (state.phoneNumber.length() == 0) {
    return;
  }

  // Send SMS command
  sim900a.print("AT+CMGS=\"");
  sim900a.print(state.phoneNumber);
  sim900a.println("\"");

  unsigned long startTime = millis();
  bool promptReceived = false;

  // Wait for ">" prompt with timeout
  while (millis() - startTime < 2000) {
    if (sim900a.available()) {
      char c = sim900a.read();
      if (c == '>') {
        promptReceived = true;
        break;
      }
    }
    delay(10); // Short delay to avoid blocking
    yield();   // Reset watchdog
  }

  if (!promptReceived) {
    return;
  }

  // Send message content
  sim900a.println(message);
  sim900a.write(26); // CTRL+Z

  // Wait for response with shorter timeout
  startTime = millis();
  while (millis() - startTime < 3000) {
    if (sim900a.available()) {
      sim900a.read();
    }
    delay(10);
    yield();
  }

  // Clear any remaining data
  while (sim900a.available()) {
    sim900a.read();
  }
}
void checkSMS() {
  static unsigned long lastSMSCheck = 0;
  
  // Limit SMS checking frequency to reduce load
  if (millis() - lastSMSCheck < 2000) return;
  lastSMSCheck = millis();
  
  if (sim900a.available()) {
    String smsContent = "";
    unsigned long startTime = millis();
    
    // Read available data with timeout
    while (sim900a.available() && (millis() - startTime < 1000)) {
      smsContent += (char)sim900a.read();
      yield(); // Prevent watchdog reset
    }
    
    // Clear any remaining data
    while (sim900a.available()) {
      sim900a.read();
    }
    
    // Check for incoming SMS
    if (smsContent.indexOf("+CMTI:") >= 0) {
      // Read first SMS
      sendCommand("AT+CMGR=1", 2000); // Reduced timeout
      
      // Simple command processing - look for 'status'
      String lowerContent = smsContent;
      lowerContent.toLowerCase();
      if (lowerContent.indexOf("status") >= 0) {
        processSMSCommand("status");
      }
      
      // Delete SMS
      sendCommand("AT+CMGD=1", 1000);
    }
  }
}

void processSMSCommand(String command) {
  
  if (command == "status") {
    String status = "INCUBATOR STATUS:\n";
    status += "Temp: " + String(state.temperature, 1) + "C\n";
    status += "Humidity: " + String(state.humidity, 0) + "%\n";
    status += "Water: " + String(state.waterPresent ? "OK" : "LOW") + "\n";
    status += "Bulb: " + String(state.bulbState ? "ON" : "OFF") + "\n";
    status += "Fan: " + String(state.fanState ? "ON" : "OFF") + "\n";
    status += "SIM: " + state.simCardNumber;
    sendSMS(status);
  } else {
    sendSMS("Send 'status' for system info");
  }
}

void checkSMSModule() {
  static unsigned long lastModuleCheck = 0;
  if (millis() - lastModuleCheck < 60000) return; // Check every 60 seconds to reduce load
  
  lastModuleCheck = millis();
  
  // Clear any pending data
  while(sim900a.available()) {
    sim900a.read();
  }
  
  // Simple AT test with timeout
  sim900a.println("AT");
  
  unsigned long startTime = millis();
  String response = "";
  
  // Wait for response with timeout
  while (millis() - startTime < 2000) {
    if (sim900a.available()) {
      response += (char)sim900a.read();
    }
    yield(); // Prevent watchdog reset
  }
  
  // Clear any remaining data
  while (sim900a.available()) {
    sim900a.read();
  }
  
  if (response.indexOf("OK") != -1) {
    state.smsModuleActive = true;
    // Also update signal strength while we're checking
    updateSignalStrengthQuick();
  } else {
    state.smsModuleActive = false;
    state.signalStrength = "Module Error";
  }
}

void updateSignalStrengthQuick() {
  // Only check signal if module is active
  if (!state.smsModuleActive) {
    return;
  }
  
  sim900a.println("AT+CSQ");
  
  unsigned long startTime = millis();
  String response = "";
  
  // Wait for response with timeout
  while (millis() - startTime < 1500) {
    if (sim900a.available()) {
      response += (char)sim900a.read();
    }
    yield();
  }
  
  // Clear any remaining data
  while (sim900a.available()) {
    sim900a.read();
  }
  
  int rssiStart = response.indexOf("+CSQ: ") + 6;
  if (rssiStart > 5) {
    int rssiEnd = response.indexOf(",", rssiStart);
    if (rssiEnd > rssiStart) {
      int rssi = response.substring(rssiStart, rssiEnd).toInt();
      
      if (rssi == 0 || rssi == 99) {
        state.signalStrength = "No Signal";
      } else if (rssi < 10) {
        state.signalStrength = "Weak";
      } else if (rssi < 20) {
        state.signalStrength = "Medium";
      } else {
        state.signalStrength = "Strong";
      }
    }
  } else {
    state.signalStrength = "Read Error";
  }
}

// Keep the original function but make it call only when needed
void updateSignalStrength() {
  static unsigned long lastSignalCheck = 0;
  if (millis() - lastSignalCheck < 120000) return; // Check every 2 minutes only
  
  lastSignalCheck = millis();
  updateSignalStrengthQuick();
}

void updateLCD() {
  unsigned long currentTime = millis();
  if (currentTime - lastLCDUpdate < 1000) return; // Update every second
  
  lastLCDUpdate = currentTime;
  lcdDisplayCycle = !lcdDisplayCycle;
  
  lcd.clear();
  
  // First line always shows temperature and humidity
  lcd.setCursor(0, 0);
  if (state.temperature > -999) {
    lcd.print("T:");
    if (state.temperature < 10.0) lcd.print(" ");
    lcd.print(state.temperature, 2);
    lcd.print("C");
  } else {
    lcd.print("T:ERROR ");
  }
  
  lcd.setCursor(9, 0);
  if (state.humidity > -999) {
    lcd.print("H:");
    if (state.humidity < 10) lcd.print(" ");
    lcd.print((int)state.humidity);
    lcd.print("%");
  } else {
    lcd.print("H:ERR");
  }
  
  // Second line alternates between water/bulb and fan/egg turner
  lcd.setCursor(0, 1);
  if (!lcdDisplayCycle) {
    // Water and Bulb status
    lcd.print("W:");
    lcd.print(state.waterPresent ? "High" : "Low ");
    
    lcd.setCursor(9, 1);
    lcd.print("B:");
    lcd.print(state.bulbState ? "ON " : "OFF");
  } else {
    // Fan and Egg Turner status
    lcd.print("F:");
    lcd.print(state.fanState ? "ON " : "OFF");
    
    lcd.setCursor(9, 1);
    lcd.print("E:");
    lcd.print(state.eggTurnerState ? "ON " : "OFF");
  }
}

void handleWaterAlert() {
  static bool lastWaterState = true;
  static unsigned long waterAlertTime = 0;
  static bool alertSent = false;
  
  if (!state.waterPresent && lastWaterState) {
    // Water just went low
    waterAlertTime = millis();
    alertSent = false;
  }
  
  if (!state.waterPresent) {
    // Continuous buzzer when no water
    if (state.buzzerEnabled && (millis() % 2000 < 1000)) {
      digitalWrite(BUZZER_PIN, HIGH);
    } else {
      digitalWrite(BUZZER_PIN, LOW);
    }
    
    // Send SMS alert (once per water low event)
    if (!alertSent && state.phoneNumber.length() > 0 && (millis() - waterAlertTime > 30000)) {
      sendSMS("ALERT: Egg Incubator - No water detected!");
      alertSent = true;
    }
  } else {
    digitalWrite(BUZZER_PIN, LOW);
  }
  
  lastWaterState = state.waterPresent;
}



String formatTime(int seconds) {
  int hours = seconds / 3600;
  int minutes = (seconds % 3600) / 60;
  int secs = seconds % 60;
  
  char buffer[10];
  sprintf(buffer, "%02d:%02d:%02d", hours, minutes, secs);
  return String(buffer);
}

String buildWebPage() {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><title>Egg Incubator Control</title>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>"
    "body{font-family:'Segoe UI',Arial,sans-serif;max-width:800px;margin:0 auto;padding:15px;background:linear-gradient(135deg,#FFA726 0%,#FF7043 100%);min-height:100vh;}"
    ".card{background:rgba(255,255,255,0.95);padding:20px;border-radius:15px;box-shadow:0 8px 32px rgba(0,0,0,0.1);margin-bottom:15px;backdrop-filter:blur(10px);}"
    ".title{margin:0 0 20px 0;color:#E65100;font-size:24px;text-align:center;}"
    ".sensor-grid{display:grid;grid-template-columns:1fr 1fr 1fr;gap:10px;margin-bottom:15px;}"
    ".sensor-item{text-align:center;padding:15px;border-radius:10px;background:linear-gradient(45deg,#FFA726,#FFB74D);color:white;}"
    ".control-section{margin-bottom:20px;}"
    ".section-title{color:#E65100;font-size:18px;margin:0 0 15px 0;border-bottom:2px solid #FF7043;padding-bottom:5px;}"
    ".control-grid{display:grid;grid-template-columns:1fr 1fr 1fr;gap:10px;margin-bottom:15px;}"
    ".control-item{padding:15px;border-radius:10px;text-align:center;}"
    ".status-on{background:linear-gradient(45deg,#4CAF50,#66BB6A);color:white;}"
    ".status-off{background:linear-gradient(45deg,#757575,#9E9E9E);color:white;}"
    ".btn-group{display:flex;gap:8px;margin:8px 0;flex-wrap:wrap;}"
    ".btn{padding:10px 15px;border:none;border-radius:8px;cursor:pointer;font-size:14px;font-weight:bold;transition:transform 0.2s,box-shadow 0.2s;flex:1;min-width:70px;}"
    ".btn:hover{transform:translateY(-2px);box-shadow:0 5px 15px rgba(0,0,0,0.2);}"
    ".btn-green{background:linear-gradient(45deg,#4CAF50,#66BB6A);color:white;}"
    ".btn-red{background:linear-gradient(45deg,#F44336,#E57373);color:white;}"
    ".btn-blue{background:linear-gradient(45deg,#2196F3,#64B5F6);color:white;}"
    ".btn-orange{background:linear-gradient(45deg,#FF9800,#FFB74D);color:white;}"
    ".schedule-controls{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin:10px 0;}"
    ".time-input{padding:8px;border:2px solid #FFB74D;border-radius:6px;width:80px;text-align:center;font-size:14px;}"
    ".threshold-row{display:flex;align-items:center;gap:10px;margin:8px 0;}"
    ".sms-section{background:linear-gradient(45deg,#9C27B0,#BA68C8);color:white;padding:15px;border-radius:10px;}"
    ".phone-input{padding:8px;border:2px solid #FFB74D;border-radius:6px;width:150px;}"
    "</style></head><body>";
  
  html += "<div class='card'>";
  html += "<h1 class='title'>🥚 Egg Incubator System</h1>";
  
  // Sensor status
  html += "<div class='sensor-grid'>";
  html += "<div class='sensor-item'>";
  html += "<div>🌡️ Temperature</div>";
  html += "<div>" + (state.temperature > -999 ? String(state.temperature, 1) + "°C" : "ERROR") + "</div>";
  html += "</div>";
  html += "<div class='sensor-item'>";
  html += "<div>💧 Humidity</div>";
  html += "<div>" + (state.humidity > -999 ? String((int)state.humidity) + "%" : "ERROR") + "</div>";
  html += "</div>";
  html += "<div class='sensor-item'>";
  html += "<div>🚰 Water Level</div>";
  html += "<div>" + String(state.waterPresent ? "HIGH" : "LOW") + "</div>";
  html += "</div>";
  html += "</div>";
  html += "</div>";
  
  // Fan Control
  html += "<div class='card'>";
  html += "<h3 class='section-title'>🌀 Fan Control</h3>";
  html += "<div class='control-grid'>";
  html += "<div class='control-item " + String(state.fanState ? "status-on" : "status-off") + "'>";
  html += "Status: " + String(state.fanState ? "ON" : "OFF");
  html += "</div>";
  html += "<div class='control-item " + String(state.fanScheduleEnabled ? "status-on" : "status-off") + "'>";
  html += "Schedule: " + String(state.fanScheduleEnabled ? "ON" : "OFF");
  html += "</div>";
  html += "<div class='control-item status-on'>";
  html += "Next: " + formatTime(state.fanScheduleEnabled ? 
    (state.fanScheduleRunning ? 
     (state.fanRunDuration - (millis() - state.fanLastToggle)/1000) : 
     (state.fanIdleDuration - (millis() - state.fanLastToggle)/1000)) : 0);
  html += "</div>";
  html += "</div>";
  
  html += "<div class='btn-group'>";
  html += "<button class='btn btn-green' onclick='controlRelay(\"fan\", \"on\")' id='fan-on-btn'>Manual ON</button>";
  html += "<button class='btn btn-red' onclick='controlRelay(\"fan\", \"off\")' id='fan-off-btn'>Manual OFF</button>";
  html += "<button class='btn btn-blue' onclick='toggleSchedule(\"fan\")' id='fan-schedule-btn'>" + String(state.fanScheduleEnabled ? "Stop" : "Start") + " Schedule</button>";
  html += "</div>";
  
  html += "<div class='schedule-controls'>";
  html += "<div style='margin-bottom:10px;'>";
  html += "<div style='display:flex;gap:10px;align-items:center;margin-bottom:5px;'>";
  html += "<label>Run Duration: </label>";
  html += "<input id='fan-run-duration' type='number' value='" + String(state.fanRunDuration) + "' min='1' max='86400' class='time-input'> sec";
  html += "<button class='btn btn-orange' onclick='setTiming(\"fan\", \"run\", document.getElementById(\"fan-run-duration\").value)' style='margin-left:10px;'>Set</button>";
  html += "</div>";
  html += "<div style='display:flex;gap:10px;align-items:center;'>";
  html += "<label>Idle Duration: </label>";
  html += "<input id='fan-idle-duration' type='number' value='" + String(state.fanIdleDuration) + "' min='1' max='86400' class='time-input'> sec";
  html += "<button class='btn btn-orange' onclick='setTiming(\"fan\", \"idle\", document.getElementById(\"fan-idle-duration\").value)' style='margin-left:10px;'>Set</button>";
  html += "</div>";
  html += "</div>";
  html += "</div>";
  html += "</div>";
  
  // Egg Turner Control
  html += "<div class='card'>";
  html += "<h3 class='section-title'>🔄 Egg Turner Control</h3>";
  html += "<div class='control-grid'>";
  html += "<div class='control-item " + String(state.eggTurnerState ? "status-on" : "status-off") + "'>";
  html += "Status: " + String(state.eggTurnerState ? "ON" : "OFF");
  html += "</div>";
  html += "<div class='control-item " + String(state.eggTurnerScheduleEnabled ? "status-on" : "status-off") + "'>";
  html += "Schedule: " + String(state.eggTurnerScheduleEnabled ? "ON" : "OFF");
  html += "</div>";
  html += "<div class='control-item status-on'>";
  html += "Next: " + formatTime(state.eggTurnerScheduleEnabled ? 
    (state.eggTurnerScheduleRunning ? 
     (state.eggTurnerRunDuration - (millis() - state.eggTurnerLastToggle)/1000) : 
     (state.eggTurnerIdleDuration - (millis() - state.eggTurnerLastToggle)/1000)) : 0);
  html += "</div>";
  html += "</div>";
  
  html += "<div class='btn-group'>";
  html += "<button class='btn btn-green' onclick='controlRelay(\"turner\", \"on\")' id='turner-on-btn'>Manual ON</button>";
  html += "<button class='btn btn-red' onclick='controlRelay(\"turner\", \"off\")' id='turner-off-btn'>Manual OFF</button>";
  html += "<button class='btn btn-blue' onclick='toggleSchedule(\"turner\")' id='turner-schedule-btn'>" + String(state.eggTurnerScheduleEnabled ? "Stop" : "Start") + " Schedule</button>";
  html += "</div>";
  
  html += "<div class='schedule-controls'>";
  html += "<div style='margin-bottom:10px;'>";
  html += "<div style='display:flex;gap:10px;align-items:center;margin-bottom:5px;'>";
  html += "<label>Run Duration: </label>";
  html += "<input id='turner-run-duration' type='number' value='" + String(state.eggTurnerRunDuration) + "' min='1' max='86400' class='time-input'> sec";
  html += "<button class='btn btn-orange' onclick='setTiming(\"turner\", \"run\", document.getElementById(\"turner-run-duration\").value)' style='margin-left:10px;'>Set</button>";
  html += "</div>";
  html += "<div style='display:flex;gap:10px;align-items:center;'>";
  html += "<label>Idle Duration: </label>";
  html += "<input id='turner-idle-duration' type='number' value='" + String(state.eggTurnerIdleDuration) + "' min='1' max='86400' class='time-input'> sec";
  html += "<button class='btn btn-orange' onclick='setTiming(\"turner\", \"idle\", document.getElementById(\"turner-idle-duration\").value)' style='margin-left:10px;'>Set</button>";
  html += "</div>";
  html += "</div>";
  html += "</div>";
  html += "</div>";
  
  // Humidifier Control
  html += "<div class='card'>";
  html += "<h3 class='section-title'>💨 Humidifier Control</h3>";
  html += "<div class='control-grid'>";
  html += "<div class='control-item " + String(state.humidifierState ? "status-on" : "status-off") + "'>";
  html += "Status: " + String(state.humidifierState ? "ON" : "OFF");
  html += "</div>";
  html += "<div class='control-item " + String(state.humidifierAutoMode ? "status-on" : "status-off") + "'>";
  html += "Auto Mode: " + String(state.humidifierAutoMode ? "ON" : "OFF");
  html += "</div>";
  html += "<div class='control-item status-on'>";
  html += "Trigger: " + String(state.humidityTrigger, 0) + "% / Stop: " + String(state.humidityStop, 0) + "%";
  html += "</div>";
  html += "</div>";
  
  html += "<div class='btn-group'>";
  html += "<button class='btn btn-green' onclick='controlRelay(\"humidifier\", \"on\")' id='humidifier-on-btn'>Manual ON</button>";
  html += "<button class='btn btn-red' onclick='controlRelay(\"humidifier\", \"off\")' id='humidifier-off-btn'>Manual OFF</button>";
  html += "<button class='btn btn-blue' onclick='toggleAutoMode(\"humidifier\")' id='humidifier-auto-btn'>" + String(state.humidifierAutoMode ? "Disable" : "Enable") + " Auto</button>";
  html += "</div>";
  
  html += "<div class='threshold-row'>";
  html += "<div style='margin-bottom:10px;'>";
  html += "<div style='display:flex;gap:10px;align-items:center;margin-bottom:5px;'>";
  html += "<label>Trigger:</label><input id='humidity-trigger' type='number' step='0.1' value='" + String(state.humidityTrigger, 1) + "' class='time-input'>%";
  html += "</div>";
  html += "<div style='display:flex;gap:10px;align-items:center;margin-bottom:5px;'>";
  html += "<label>Stop:</label><input id='humidity-stop' type='number' step='0.1' value='" + String(state.humidityStop, 1) + "' class='time-input'>%";
  html += "</div>";
  html += "<button class='btn btn-orange' onclick='setThresholds(\"humidifier\")' style='width:100%;'>Set Thresholds</button>";
  html += "</div>";
  html += "</div>";
  html += "</div>";
  
  // Bulb (Heater) Control
  html += "<div class='card'>";
  html += "<h3 class='section-title'>💡 Bulb (Heater) Control</h3>";
  html += "<div class='control-grid'>";
  html += "<div class='control-item " + String(state.bulbState ? "status-on" : "status-off") + "'>";
  html += "Status: " + String(state.bulbState ? "ON" : "OFF");
  html += "</div>";
  html += "<div class='control-item " + String(state.bulbAutoMode ? "status-on" : "status-off") + "'>";
  html += "Auto Mode: " + String(state.bulbAutoMode ? "ON" : "OFF");
  html += "</div>";
  html += "<div class='control-item status-on'>";
  html += "Trigger: " + String(state.tempTrigger, 1) + "°C / Stop: " + String(state.tempStop, 1) + "°C";
  html += "</div>";
  html += "</div>";
  
  html += "<div class='btn-group'>";
  html += "<button class='btn btn-green' onclick='controlRelay(\"bulb\", \"on\")' id='bulb-on-btn'>Manual ON</button>";
  html += "<button class='btn btn-red' onclick='controlRelay(\"bulb\", \"off\")' id='bulb-off-btn'>Manual OFF</button>";
  html += "<button class='btn btn-blue' onclick='toggleAutoMode(\"bulb\")' id='bulb-auto-btn'>" + String(state.bulbAutoMode ? "Disable" : "Enable") + " Auto</button>";
  html += "</div>";
  
  html += "<div class='threshold-row'>";
  html += "<div style='margin-bottom:10px;'>";
  html += "<div style='display:flex;gap:10px;align-items:center;margin-bottom:5px;'>";
  html += "<label>Trigger:</label><input id='temp-trigger' type='number' step='0.1' value='" + String(state.tempTrigger, 1) + "' class='time-input'>°C";
  html += "</div>";
  html += "<div style='display:flex;gap:10px;align-items:center;margin-bottom:5px;'>";
  html += "<label>Stop:</label><input id='temp-stop' type='number' step='0.1' value='" + String(state.tempStop, 1) + "' class='time-input'>°C";
  html += "</div>";
  html += "<button class='btn btn-orange' onclick='setThresholds(\"bulb\")' style='width:100%;'>Set Thresholds</button>";
  html += "</div>";
  html += "</div>";
  html += "</div>";
  
  // Buzzer Control
  html += "<div class='card'>";
  html += "<h3 class='section-title'>🔊 Buzzer Control</h3>";
  html += "<div class='btn-group'>";
  html += "<button class='btn " + String(state.buzzerEnabled ? "btn-red" : "btn-green") + "' onclick='toggleBuzzer()' id='buzzer-toggle-btn'>";
  html += String(state.buzzerEnabled ? "Disable" : "Enable") + " Buzzer";
  html += "</button>";
  html += "<button class='btn btn-blue' onclick='testBuzzer()' id='buzzer-test-btn'>Test Buzzer</button>";
  html += "</div>";
  html += "</div>";
  
  // SMS Control
  html += "<div class='card'>";
  html += "<div class='sms-section'>";
  html += "<h3 style='margin-top:0;color:white;'>📱 SMS Control & Monitoring</h3>";
  
  // SMS Module Status
  html += "<div style='display:grid;grid-template-columns:1fr 1fr 1fr;gap:10px;margin:10px 0;'>";
  html += "<div style='background:" + String(state.smsModuleActive ? "rgba(76,175,80,0.3)" : "rgba(244,67,54,0.3)") + ";padding:8px;border-radius:8px;text-align:center;'>";
  html += "<span style='color:white;font-size:12px;'>📡 Module: " + String(state.smsModuleActive ? "ACTIVE" : "OFFLINE") + "</span>";
  html += "</div>";
  html += "<div style='background:rgba(156,39,176,0.3);padding:8px;border-radius:8px;text-align:center;'>";
  html += "<span style='color:white;font-size:12px;'>📶 Signal: " + state.signalStrength + "</span>";
  html += "</div>";
  html += "<div style='background:rgba(63,81,181,0.3);padding:8px;border-radius:8px;text-align:center;'>";
  html += "<span style='color:white;font-size:12px;'>📞 SIM: " + state.simCardNumber + "</span>";
  html += "</div>";
  html += "</div>";
  
  // Test SMS Button
  html += "<div style='text-align:center;margin:10px 0;'>";
  html += "<button class='btn " + String(state.smsModuleActive ? "btn-blue" : "btn-orange") + "' onclick='testSMS()' id='sms-test-btn' style='padding:10px 20px;'>";
  html += String(state.smsModuleActive ? "📤 Test SMS" : "📤 Test SMS (Force)");
  html += "</button>";
  html += "</div>";
  
  // Phone Number Input
  html += "<div style='margin:10px 0;'>";
  html += "<label style='color:white;'>Phone Number: </label>";
  html += "<input id='phone-number' type='tel' value='" + state.phoneNumber + "' placeholder='+1234567890' class='phone-input'>";
  html += "<button class='btn btn-orange' onclick='savePhoneNumber()' style='margin-left:10px;'>Save</button>";
  html += "</div>";
  html += "<p style='margin:5px 0;color:white;font-size:14px;'>💡 Send 'status' SMS to get system status</p>";
  html += "</div>";
  html += "</div>";
  
  // System Control
  html += "<div class='card'>";
  html += "<h3 class='section-title'>🔄 System Control</h3>";
  html += "<form action='/restart' method='POST' style='margin:0;'>";
  html += "<button class='btn btn-red' type='submit' style='width:100%;' onclick='return confirm(\"Restart ESP32? This will disconnect you briefly.\")'>";
  html += "🔄 Restart System";
  html += "</button></form>";
  html += "</div>";
  
  html += "<script>";
  html += "let autoRefresh = true;";
  html += "let refreshTimer;";
  html += "let isUpdating = false;";
  
  html += "function showNotification(message, type = 'success') {";
  html += "  const notification = document.createElement('div');";
  html += "  notification.textContent = message;";
  html += "  notification.style.cssText = 'position:fixed;top:20px;right:20px;padding:10px 20px;border-radius:8px;color:white;z-index:1000;font-weight:bold;';";
  html += "  notification.style.background = type === 'success' ? '#4CAF50' : '#F44336';";
  html += "  document.body.appendChild(notification);";
  html += "  setTimeout(() => notification.remove(), 3000);";
  html += "}";
  
  html += "function updateUI(data) {";
  html += "  if (isUpdating) return;";
  html += "  isUpdating = true;";
  html += "  document.querySelectorAll('.sensor-item div:nth-child(2)').forEach((elem, index) => {";
  html += "    if (index === 0) elem.textContent = data.temperature > -999 ? data.temperature.toFixed(1) + '°C' : 'ERROR';";
  html += "    if (index === 1) elem.textContent = data.humidity > -999 ? Math.round(data.humidity) + '%' : 'ERROR';";
  html += "    if (index === 2) elem.textContent = data.waterPresent ? 'HIGH' : 'LOW';";
  html += "  });";
  html += "  setTimeout(() => { isUpdating = false; }, 100);";
  html += "}";
  
  html += "function fetchStatus() {";
  html += "  if (!autoRefresh) return;";
  html += "  fetch('/api/status').then(r => r.json()).then(updateUI).catch(() => {});";
  html += "}";
  
  html += "function controlRelay(device, action) {";
  html += "  fetch(`/api/${device}/${action}`, {method: 'POST'})";
  html += "    .then(r => r.json())";
  html += "    .then(data => {";
  html += "      if (data.success) {";
  html += "        showNotification(`${device} turned ${action.toUpperCase()}`);";
  html += "        setTimeout(fetchStatus, 200);";
  html += "      }";
  html += "    })";
  html += "    .catch(() => showNotification('Failed to control ' + device, 'error'));";
  html += "}";
  
  html += "function toggleSchedule(device) {";
  html += "  fetch(`/api/${device}/schedule_toggle`, {method: 'POST'})";
  html += "    .then(r => r.json())";
  html += "    .then(data => {";
  html += "      if (data.success) {";
  html += "        showNotification(`${device} schedule ${data.scheduleEnabled ? 'enabled' : 'disabled'}`);";
  html += "        setTimeout(() => location.reload(), 500);";
  html += "      }";
  html += "    })";
  html += "    .catch(() => showNotification('Failed to toggle schedule', 'error'));";
  html += "}";
  
  html += "function toggleAutoMode(device) {";
  html += "  fetch(`/api/${device}/auto_toggle`, {method: 'POST'})";
  html += "    .then(r => r.json())";
  html += "    .then(data => {";
  html += "      if (data.success) {";
  html += "        showNotification(`${device} auto mode ${data.autoMode ? 'enabled' : 'disabled'}`);";
  html += "        setTimeout(() => location.reload(), 500);";
  html += "      }";
  html += "    })";
  html += "    .catch(() => showNotification('Failed to toggle auto mode', 'error'));";
  html += "}";
  
  html += "function setTiming(device, type, value) {";
  html += "  const data = new FormData();";
  html += "  data.append(type, value);";
  html += "  fetch(`/api/${device}/timing`, {method: 'POST', body: data})";
  html += "    .then(r => r.json())";
  html += "    .then(data => {";
  html += "      if (data.success) {";
  html += "        showNotification(`${device} ${type} duration updated`);";
  html += "      }";
  html += "    })";
  html += "    .catch(() => showNotification('Failed to update timing', 'error'));";
  html += "}";
  
  html += "function setThresholds(device) {";
  html += "  const trigger = document.getElementById(device === 'humidifier' ? 'humidity-trigger' : 'temp-trigger').value;";
  html += "  const stop = document.getElementById(device === 'humidifier' ? 'humidity-stop' : 'temp-stop').value;";
  html += "  const data = new FormData();";
  html += "  data.append('trigger', trigger);";
  html += "  data.append('stop', stop);";
  html += "  fetch(`/api/${device}/thresholds`, {method: 'POST', body: data})";
  html += "    .then(r => r.json())";
  html += "    .then(data => {";
  html += "      if (data.success) {";
  html += "        showNotification(`${device} thresholds updated`);";
  html += "      }";
  html += "    })";
  html += "    .catch(() => showNotification('Failed to update thresholds', 'error'));";
  html += "}";
  
  html += "function toggleBuzzer() {";
  html += "  fetch('/api/buzzer/toggle', {method: 'POST'})";
  html += "    .then(r => r.json())";
  html += "    .then(data => {";
  html += "      if (data.success) {";
  html += "        showNotification(`Buzzer ${data.enabled ? 'enabled' : 'disabled'}`);";
  html += "        setTimeout(() => location.reload(), 500);";
  html += "      }";
  html += "    })";
  html += "    .catch(() => showNotification('Failed to toggle buzzer', 'error'));";
  html += "}";
  
  html += "function testBuzzer() {";
  html += "  fetch('/api/buzzer/test', {method: 'POST'})";
  html += "    .then(r => r.json())";
  html += "    .then(data => {";
  html += "      if (data.success) {";
  html += "        showNotification('Buzzer test sent');";
  html += "      }";
  html += "    })";
  html += "    .catch(() => showNotification('Failed to test buzzer', 'error'));";
  html += "}";
  
  html += "function savePhoneNumber() {";
  html += "  const phone = document.getElementById('phone-number').value;";
  html += "  const data = new FormData();";
  html += "  data.append('phone', phone);";
  html += "  fetch('/api/sms/phone', {method: 'POST', body: data})";
  html += "    .then(r => r.json())";
  html += "    .then(data => {";
  html += "      if (data.success) {";
  html += "        showNotification('Phone number saved');";
  html += "      }";
  html += "    })";
  html += "    .catch(() => showNotification('Failed to save phone number', 'error'));";
  html += "}";
  
  html += "function testSMS() {";
  html += "  fetch('/api/sms/test', {method: 'POST'})";
  html += "    .then(r => r.json())";
  html += "    .then(data => {";
  html += "      if (data.success) {";
  html += "        showNotification('Test SMS sent');";
  html += "      }";
  html += "    })";
  html += "    .catch(() => showNotification('Failed to send test SMS', 'error'));";
  html += "}";
  
  html += "function startRefresh() {";
  html += "  if(autoRefresh) {";
  html += "    refreshTimer = setTimeout(() => { fetchStatus(); startRefresh(); }, 2000);";
  html += "  }";
  html += "}";
  
  html += "document.querySelectorAll('input').forEach(input => {";
  html += "  input.addEventListener('focus', () => {";
  html += "    autoRefresh = false;";
  html += "    clearTimeout(refreshTimer);";
  html += "  });";
  html += "  input.addEventListener('blur', () => {";
  html += "    autoRefresh = true;";
  html += "    startRefresh();";
  html += "  });";
  html += "});";
  
  html += "document.addEventListener('DOMContentLoaded', () => {";
  html += "  fetchStatus();";
  html += "  startRefresh();";
  html += "});";
  
  html += "</script>";
  
  html += "</body></html>";
  return html;
}

void loadSettings() {
  prefs.begin("incubator", true);
  state.tempTrigger = prefs.getFloat("tempTrig", 37.5);
  state.tempStop = prefs.getFloat("tempStop", 38.0);
  state.humidityTrigger = prefs.getFloat("humTrig", 60.0);
  state.humidityStop = prefs.getFloat("humStop", 65.0);
  state.fanRunDuration = prefs.getUInt("fanRun", 300);
  state.fanIdleDuration = prefs.getUInt("fanIdle", 300);
  state.eggTurnerRunDuration = prefs.getUInt("turnRun", 10);
  state.eggTurnerIdleDuration = prefs.getUInt("turnIdle", 21600);
  state.buzzerEnabled = prefs.getBool("buzzer", true);
  state.phoneNumber = prefs.getString("phone", "");
  prefs.end();
}

void saveSettings() {
  prefs.begin("incubator", false);
  prefs.putFloat("tempTrig", state.tempTrigger);
  prefs.putFloat("tempStop", state.tempStop);
  prefs.putFloat("humTrig", state.humidityTrigger);
  prefs.putFloat("humStop", state.humidityStop);
  prefs.putUInt("fanRun", state.fanRunDuration);
  prefs.putUInt("fanIdle", state.fanIdleDuration);
  prefs.putUInt("turnRun", state.eggTurnerRunDuration);
  prefs.putUInt("turnIdle", state.eggTurnerIdleDuration);
  prefs.putBool("buzzer", state.buzzerEnabled);
  prefs.putString("phone", state.phoneNumber);
  prefs.end();
}

void setupWebServer() {
  // Main page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/html", buildWebPage());
  });
  
  // Fan controls
  server.on("/fan/on", HTTP_POST, [](AsyncWebServerRequest *request){
    updateRelayState(FAN_PIN, state.fanState, true);
    request->redirect("/");
  });
  
  server.on("/fan/off", HTTP_POST, [](AsyncWebServerRequest *request){
    updateRelayState(FAN_PIN, state.fanState, false);
    request->redirect("/");
  });
  
  server.on("/fan/schedule_toggle", HTTP_POST, [](AsyncWebServerRequest *request){
    state.fanScheduleEnabled = !state.fanScheduleEnabled;
    if (state.fanScheduleEnabled) {
      state.fanLastToggle = millis();
      state.fanScheduleRunning = false;
    }
    request->redirect("/");
  });
  
  server.on("/fan/timing", HTTP_POST, [](AsyncWebServerRequest *request){
    if (request->hasParam("run", true)) {
      state.fanRunDuration = request->getParam("run", true)->value().toInt();
    }
    if (request->hasParam("idle", true)) {
      state.fanIdleDuration = request->getParam("idle", true)->value().toInt();
    }
    saveSettings();
    request->redirect("/");
  });
  
  // Egg Turner controls
  server.on("/turner/on", HTTP_POST, [](AsyncWebServerRequest *request){
    updateRelayState(EGG_TURNER_PIN, state.eggTurnerState, true);
    request->redirect("/");
  });
  
  server.on("/turner/off", HTTP_POST, [](AsyncWebServerRequest *request){
    updateRelayState(EGG_TURNER_PIN, state.eggTurnerState, false);
    request->redirect("/");
  });
  
  server.on("/turner/schedule_toggle", HTTP_POST, [](AsyncWebServerRequest *request){
    state.eggTurnerScheduleEnabled = !state.eggTurnerScheduleEnabled;
    if (state.eggTurnerScheduleEnabled) {
      state.eggTurnerLastToggle = millis();
      state.eggTurnerScheduleRunning = false;
    }
    request->redirect("/");
  });
  
  server.on("/turner/timing", HTTP_POST, [](AsyncWebServerRequest *request){
    if (request->hasParam("run", true)) {
      state.eggTurnerRunDuration = request->getParam("run", true)->value().toInt();
    }
    if (request->hasParam("idle", true)) {
      state.eggTurnerIdleDuration = request->getParam("idle", true)->value().toInt();
    }
    saveSettings();
    request->redirect("/");
  });
  
  // Humidifier controls
  server.on("/humidifier/on", HTTP_POST, [](AsyncWebServerRequest *request){
    updateRelayState(HUMIDIFIER_PIN, state.humidifierState, true);
    request->redirect("/");
  });
  
  server.on("/humidifier/off", HTTP_POST, [](AsyncWebServerRequest *request){
    updateRelayState(HUMIDIFIER_PIN, state.humidifierState, false);
    request->redirect("/");
  });
  
  server.on("/humidifier/auto_toggle", HTTP_POST, [](AsyncWebServerRequest *request){
    state.humidifierAutoMode = !state.humidifierAutoMode;
    request->redirect("/");
  });
  
  server.on("/humidifier/thresholds", HTTP_POST, [](AsyncWebServerRequest *request){
    if (request->hasParam("trigger", true)) {
      state.humidityTrigger = request->getParam("trigger", true)->value().toFloat();
    }
    if (request->hasParam("stop", true)) {
      state.humidityStop = request->getParam("stop", true)->value().toFloat();
    }
    saveSettings();
    request->redirect("/");
  });
  
  // Bulb controls
  server.on("/bulb/on", HTTP_POST, [](AsyncWebServerRequest *request){
    updateRelayState(BULB_PIN, state.bulbState, true);
    request->redirect("/");
  });
  
  server.on("/bulb/off", HTTP_POST, [](AsyncWebServerRequest *request){
    updateRelayState(BULB_PIN, state.bulbState, false);
    request->redirect("/");
  });
  
  server.on("/bulb/auto_toggle", HTTP_POST, [](AsyncWebServerRequest *request){
    state.bulbAutoMode = !state.bulbAutoMode;
    request->redirect("/");
  });
  
  server.on("/bulb/thresholds", HTTP_POST, [](AsyncWebServerRequest *request){
    if (request->hasParam("trigger", true)) {
      state.tempTrigger = request->getParam("trigger", true)->value().toFloat();
    }
    if (request->hasParam("stop", true)) {
      state.tempStop = request->getParam("stop", true)->value().toFloat();
    }
    saveSettings();
    request->redirect("/");
  });
  
  // Buzzer controls
  server.on("/buzzer/toggle", HTTP_POST, [](AsyncWebServerRequest *request){
    state.buzzerEnabled = !state.buzzerEnabled;
    saveSettings();
    request->redirect("/");
  });
  
  server.on("/buzzer/test", HTTP_POST, [](AsyncWebServerRequest *request){
    buzzerBeep(200, 2);
    request->redirect("/");
  });
  
  // SMS controls
  server.on("/sms/phone", HTTP_POST, [](AsyncWebServerRequest *request){
    if (request->hasParam("phone", true)) {
      state.phoneNumber = request->getParam("phone", true)->value();
      saveSettings();
      
      // Send system online SMS
      if (state.phoneNumber.length() > 0 && !state.systemOnlineSent) {
        sendSMS("Egg Incubator System is now online and monitoring.");
        state.systemOnlineSent = true;
      }
    }
    request->redirect("/");
  });
  
  server.on("/sms/test", HTTP_POST, [](AsyncWebServerRequest *request){
    if (state.phoneNumber.length() > 0) {
      String testMsg = "TEST SMS - Egg Incubator System\n";
      testMsg += "SMS Module: " + String(state.smsModuleActive ? "ACTIVE" : "TESTING") + "\n";
      testMsg += "Signal: " + state.signalStrength + "\n";
      testMsg += "Temperature: " + String(state.temperature, 1) + "C\n";
      testMsg += "Humidity: " + String((int)state.humidity) + "%\n";
      testMsg += "Water: " + String(state.waterPresent ? "OK" : "LOW") + "\n";
      testMsg += "Uptime: " + String(millis()/60000) + " min\n";
      testMsg += "Test message sent!";
      sendSMS(testMsg);
    }
    request->redirect("/");
  });
  
  // System restart
  // JSON API endpoints for AJAX
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request){
    String json = "{";
    json += "\"temperature\":" + String(state.temperature, 1) + ",";
    json += "\"humidity\":" + String(state.humidity, 0) + ",";
    json += "\"waterPresent\":" + String(state.waterPresent ? "true" : "false") + ",";
    json += "\"fanState\":" + String(state.fanState ? "true" : "false") + ",";
    json += "\"eggTurnerState\":" + String(state.eggTurnerState ? "true" : "false") + ",";
    json += "\"humidifierState\":" + String(state.humidifierState ? "true" : "false") + ",";
    json += "\"bulbState\":" + String(state.bulbState ? "true" : "false") + ",";
    json += "\"fanScheduleEnabled\":" + String(state.fanScheduleEnabled ? "true" : "false") + ",";
    json += "\"eggTurnerScheduleEnabled\":" + String(state.eggTurnerScheduleEnabled ? "true" : "false") + ",";
    json += "\"humidifierAutoMode\":" + String(state.humidifierAutoMode ? "true" : "false") + ",";
    json += "\"bulbAutoMode\":" + String(state.bulbAutoMode ? "true" : "false") + ",";
    json += "\"buzzerEnabled\":" + String(state.buzzerEnabled ? "true" : "false") + ",";
    json += "\"smsModuleActive\":" + String(state.smsModuleActive ? "true" : "false") + ",";
    json += "\"signalStrength\":\"" + state.signalStrength + "\",";
    json += "\"phoneNumber\":\"" + state.phoneNumber + "\",";
    json += "\"tempTrigger\":" + String(state.tempTrigger, 1) + ",";
    json += "\"tempStop\":" + String(state.tempStop, 1) + ",";
    json += "\"humidityTrigger\":" + String(state.humidityTrigger, 1) + ",";
    json += "\"humidityStop\":" + String(state.humidityStop, 1) + ",";
    json += "\"fanRunDuration\":" + String(state.fanRunDuration) + ",";
    json += "\"fanIdleDuration\":" + String(state.fanIdleDuration) + ",";
    json += "\"eggTurnerRunDuration\":" + String(state.eggTurnerRunDuration) + ",";
    json += "\"eggTurnerIdleDuration\":" + String(state.eggTurnerIdleDuration);
    json += "}";
    request->send(200, "application/json", json);
  });

  server.on("/api/fan/on", HTTP_POST, [](AsyncWebServerRequest *request){
    updateRelayState(FAN_PIN, state.fanState, true);
    request->send(200, "application/json", "{\"success\":true,\"state\":true}");
  });

  server.on("/api/fan/off", HTTP_POST, [](AsyncWebServerRequest *request){
    updateRelayState(FAN_PIN, state.fanState, false);
    request->send(200, "application/json", "{\"success\":true,\"state\":false}");
  });

  server.on("/api/fan/schedule_toggle", HTTP_POST, [](AsyncWebServerRequest *request){
    state.fanScheduleEnabled = !state.fanScheduleEnabled;
    if (state.fanScheduleEnabled) {
      state.fanLastToggle = millis();
      state.fanScheduleRunning = false;
    }
    request->send(200, "application/json", "{\"success\":true,\"scheduleEnabled\":" + String(state.fanScheduleEnabled ? "true" : "false") + "}");
  });

  server.on("/api/fan/timing", HTTP_POST, [](AsyncWebServerRequest *request){
    if (request->hasParam("run", true)) {
      state.fanRunDuration = request->getParam("run", true)->value().toInt();
    }
    if (request->hasParam("idle", true)) {
      state.fanIdleDuration = request->getParam("idle", true)->value().toInt();
    }
    saveSettings();
    request->send(200, "application/json", "{\"success\":true}");
  });

  server.on("/api/turner/on", HTTP_POST, [](AsyncWebServerRequest *request){
    updateRelayState(EGG_TURNER_PIN, state.eggTurnerState, true);
    request->send(200, "application/json", "{\"success\":true,\"state\":true}");
  });

  server.on("/api/turner/off", HTTP_POST, [](AsyncWebServerRequest *request){
    updateRelayState(EGG_TURNER_PIN, state.eggTurnerState, false);
    request->send(200, "application/json", "{\"success\":true,\"state\":false}");
  });

  server.on("/api/turner/schedule_toggle", HTTP_POST, [](AsyncWebServerRequest *request){
    state.eggTurnerScheduleEnabled = !state.eggTurnerScheduleEnabled;
    if (state.eggTurnerScheduleEnabled) {
      state.eggTurnerLastToggle = millis();
      state.eggTurnerScheduleRunning = false;
    }
    request->send(200, "application/json", "{\"success\":true,\"scheduleEnabled\":" + String(state.eggTurnerScheduleEnabled ? "true" : "false") + "}");
  });

  server.on("/api/turner/timing", HTTP_POST, [](AsyncWebServerRequest *request){
    if (request->hasParam("run", true)) {
      state.eggTurnerRunDuration = request->getParam("run", true)->value().toInt();
    }
    if (request->hasParam("idle", true)) {
      state.eggTurnerIdleDuration = request->getParam("idle", true)->value().toInt();
    }
    saveSettings();
    request->send(200, "application/json", "{\"success\":true}");
  });

  server.on("/api/humidifier/on", HTTP_POST, [](AsyncWebServerRequest *request){
    updateRelayState(HUMIDIFIER_PIN, state.humidifierState, true);
    request->send(200, "application/json", "{\"success\":true,\"state\":true}");
  });

  server.on("/api/humidifier/off", HTTP_POST, [](AsyncWebServerRequest *request){
    updateRelayState(HUMIDIFIER_PIN, state.humidifierState, false);
    request->send(200, "application/json", "{\"success\":true,\"state\":false}");
  });

  server.on("/api/humidifier/auto_toggle", HTTP_POST, [](AsyncWebServerRequest *request){
    state.humidifierAutoMode = !state.humidifierAutoMode;
    request->send(200, "application/json", "{\"success\":true,\"autoMode\":" + String(state.humidifierAutoMode ? "true" : "false") + "}");
  });

  server.on("/api/humidifier/thresholds", HTTP_POST, [](AsyncWebServerRequest *request){
    if (request->hasParam("trigger", true)) {
      state.humidityTrigger = request->getParam("trigger", true)->value().toFloat();
    }
    if (request->hasParam("stop", true)) {
      state.humidityStop = request->getParam("stop", true)->value().toFloat();
    }
    saveSettings();
    request->send(200, "application/json", "{\"success\":true}");
  });

  server.on("/api/bulb/on", HTTP_POST, [](AsyncWebServerRequest *request){
    updateRelayState(BULB_PIN, state.bulbState, true);
    request->send(200, "application/json", "{\"success\":true,\"state\":true}");
  });

  server.on("/api/bulb/off", HTTP_POST, [](AsyncWebServerRequest *request){
    updateRelayState(BULB_PIN, state.bulbState, false);
    request->send(200, "application/json", "{\"success\":true,\"state\":false}");
  });

  server.on("/api/bulb/auto_toggle", HTTP_POST, [](AsyncWebServerRequest *request){
    state.bulbAutoMode = !state.bulbAutoMode;
    request->send(200, "application/json", "{\"success\":true,\"autoMode\":" + String(state.bulbAutoMode ? "true" : "false") + "}");
  });

  server.on("/api/bulb/thresholds", HTTP_POST, [](AsyncWebServerRequest *request){
    if (request->hasParam("trigger", true)) {
      state.tempTrigger = request->getParam("trigger", true)->value().toFloat();
    }
    if (request->hasParam("stop", true)) {
      state.tempStop = request->getParam("stop", true)->value().toFloat();
    }
    saveSettings();
    request->send(200, "application/json", "{\"success\":true}");
  });

  server.on("/api/buzzer/toggle", HTTP_POST, [](AsyncWebServerRequest *request){
    state.buzzerEnabled = !state.buzzerEnabled;
    saveSettings();
    request->send(200, "application/json", "{\"success\":true,\"enabled\":" + String(state.buzzerEnabled ? "true" : "false") + "}");
  });

  server.on("/api/buzzer/test", HTTP_POST, [](AsyncWebServerRequest *request){
    buzzerBeep(200, 2);
    request->send(200, "application/json", "{\"success\":true}");
  });

  server.on("/api/sms/phone", HTTP_POST, [](AsyncWebServerRequest *request){
    if (request->hasParam("phone", true)) {
      state.phoneNumber = request->getParam("phone", true)->value();
      saveSettings();
      
      // Send system online SMS
      if (state.phoneNumber.length() > 0 && !state.systemOnlineSent) {
        sendSMS("Egg Incubator System is now online and monitoring.");
        state.systemOnlineSent = true;
      }
    }
    request->send(200, "application/json", "{\"success\":true}");
  });

  server.on("/api/sms/test", HTTP_POST, [](AsyncWebServerRequest *request){
    if (state.phoneNumber.length() > 0) {
      String testMsg = "TEST SMS - Egg Incubator System\n";
      testMsg += "SMS Module: " + String(state.smsModuleActive ? "ACTIVE" : "TESTING") + "\n";
      testMsg += "Signal: " + state.signalStrength + "\n";
      testMsg += "Temperature: " + String(state.temperature, 1) + "C\n";
      testMsg += "Humidity: " + String((int)state.humidity) + "%\n";
      testMsg += "Water: " + String(state.waterPresent ? "OK" : "LOW") + "\n";
      testMsg += "Uptime: " + String(millis()/60000) + " min\n";
      testMsg += "Test message sent!";
      sendSMS(testMsg);
    }
    request->send(200, "application/json", "{\"success\":true}");
  });

  server.on("/restart", HTTP_POST, [](AsyncWebServerRequest *request){
    request->send(200, "text/html", 
      "<!DOCTYPE html><html><head><title>Restarting...</title>"
      "<meta http-equiv='refresh' content='15;url=/'>"
      "<style>body{font-family:Arial,sans-serif;text-align:center;padding:50px;background:linear-gradient(135deg,#FFA726 0%,#FF7043 100%);color:white;}</style>"
      "</head><body><h2>🔄 Egg Incubator Restarting...</h2>"
      "<p>Please wait 15 seconds, then you'll be redirected automatically.</p>"
      "<p>Or <a href='/' style='color:white;'>click here</a> to return manually.</p></body></html>");
    delay(1000);
    ESP.restart();
  });
  
  server.onNotFound([](AsyncWebServerRequest *request){
    request->redirect("/");
  });
}

void setup() {
  // Initialize pins
  pinMode(HUMIDIFIER_PIN, OUTPUT);
  pinMode(FAN_PIN, OUTPUT);
  pinMode(BULB_PIN, OUTPUT);
  pinMode(EGG_TURNER_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(WATER_SENSOR_PIN, INPUT);
  
  // Set all relays to OFF initially
  digitalWrite(HUMIDIFIER_PIN, HIGH);
  digitalWrite(FAN_PIN, HIGH);
  digitalWrite(BULB_PIN, HIGH);
  digitalWrite(EGG_TURNER_PIN, HIGH);
  digitalWrite(BUZZER_PIN, LOW);
  
  // Initialize LCD
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Egg Incubator");
  lcd.setCursor(0, 1);
  lcd.print("Starting...");
  
  // Initialize DHT sensors
  dht1.begin();
  dht2.begin();
  
  // Initialize GSM module
  initializeGSMModule();
  
  // Load settings from flash
  loadSettings();
  
  // Start AP and DNS
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  dns.start(DNS_PORT, "*", WiFi.softAPIP());
  
  // Setup web server
  setupWebServer();
  server.begin();
  
  // System startup complete - 2 quick beeps
  delay(2000); // Wait for SIM900A initialization
  buzzerBeep(200, 2);
  
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("System Ready");
  lcd.setCursor(0, 1);
  lcd.print("IP:192.168.4.1");
  
  // Send system online SMS if phone number is configured
  if (state.phoneNumber.length() > 0) {
    sendSMS("Egg Incubator System is now online and monitoring.");
    state.systemOnlineSent = true;
  }
}

void loop() {
  dns.processNextRequest();
  
  // Read sensors
  readSensors();
  
  // Handle automatic modes
  handleAutoModes();
  
  // Handle schedule modes
  handleScheduleModes();
  
  // Update LCD display
  updateLCD();
  
  // Handle water level alerts
  handleWaterAlert();
  
  // Check SMS module connectivity 
  checkSMSModule();
  
  // Check for SMS messages 
  checkSMS();
  
  yield(); 
  delay(100);
}
