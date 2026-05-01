#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>
#include <addons/RTDBHelper.h>

// AP config
const char* AP_SSID = "EggIncubator";
const char* AP_PASS = "12345678";
const byte DNS_PORT = 53;
AsyncWebServer server(80);
DNSServer dns;
Preferences prefs;

// Firebase Configuration – must match the frontend app's Firebase project
String FIREBASE_API_KEY = "AIzaSyC7eCkFuytEBekMxjzmRU0bQ9UDsddTc4o";
String FIREBASE_DB_URL = "https://incubator-1c8ed-default-rtdb.firebaseio.com/";
String DEVICE_ID = "incubator1";

// Firebase objects
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;
bool firebaseReady = false;
String firebaseStatus = "Not Connected";
String firebaseLastError = "";
bool firebaseConnected = false;

// WiFi STA Configuration (stored in Preferences)
String staSSID = "";
String staPassword = "";
bool onlineMode = false;
bool wifiConnected = false;
bool testMode = false;  // Test mode sends dummy sensor data
unsigned long lastFirebaseSync = 0;
unsigned long lastFirebaseCommand = 0;
String lastCmdId = "";
bool metaPublished = false;  // Tracks if device metadata has been written to RTDB
const unsigned long FIREBASE_SYNC_INTERVAL = 5000;  // 5 seconds

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

// Sensors
const int AM2302_1_PIN = 14;
const int AM2302_2_PIN = 27;
const int WATER_SENSOR_PIN = 4;  // Moved from GPIO 35 (input-only) to GPIO 4 (supports pull-up)

// Component initialization
LiquidCrystal_I2C lcd(0x27, 16, 2);
DHT dht1(AM2302_1_PIN, DHT22);
DHT dht2(AM2302_2_PIN, DHT22);

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
  
  // Manual override tracking (prevents auto-modes from interfering for 30 seconds)
  unsigned long bulbManualOverrideUntil = 0;
  unsigned long humidifierManualOverrideUntil = 0;
  unsigned long fanManualOverrideUntil = 0;
  unsigned long turnerManualOverrideUntil = 0;
  
  // Egg turner turn-now timer (non-blocking)
  unsigned long turnerTurnNowUntil = 0;
  bool turnerTurnNowActive = false;
  
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

// Connect to WiFi STA mode
bool connectToWiFi() {
  if (staSSID.length() == 0) return false;
  
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(staSSID.c_str(), staPassword.c_str());
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    attempts++;
  }
  
  wifiConnected = (WiFi.status() == WL_CONNECTED);
  return wifiConnected;
}

// Initialize Firebase
void initFirebase() {
  // Check if credentials are provided
  if (FIREBASE_API_KEY.length() == 0 || FIREBASE_DB_URL.length() == 0) {
    Serial.println("[Firebase] ERROR: Missing credentials");
    Serial.print("[Firebase] API Key length: ");
    Serial.println(FIREBASE_API_KEY.length());
    Serial.print("[Firebase] DB URL length: ");
    Serial.println(FIREBASE_DB_URL.length());
    firebaseStatus = "Missing Credentials";
    firebaseLastError = "Database URL or API Key not configured";
    firebaseReady = false;
    firebaseConnected = false;
    return;
  }
  
  // Validate API Key format
  if (FIREBASE_API_KEY.length() < 20) {
    Serial.println("[Firebase] ERROR: Invalid API Key");
    firebaseStatus = "Invalid API Key";
    firebaseLastError = "API Key too short";
    firebaseReady = false;
    firebaseConnected = false;
    return;
  }
  
  // Validate Database URL format
  if (!FIREBASE_DB_URL.startsWith("https://")) {
    Serial.println("[Firebase] ERROR: Invalid DB URL format");
    firebaseStatus = "Invalid DB URL";
    firebaseLastError = "Database URL must start with https://";
    firebaseReady = false;
    firebaseConnected = false;
    return;
  }
  
  config.api_key = FIREBASE_API_KEY.c_str();
  config.database_url = FIREBASE_DB_URL.c_str();
  
  // Set token status callback before Firebase.begin
  config.token_status_callback = tokenStatusCallback;
  
  // Set response size and timeout
  config.timeout.serverResponse = 10 * 1000;
  
  // For anonymous/unauthenticated access to work, Firebase rules must allow public access
  // Leave auth empty - no need to sign in
  
  Serial.println("[Firebase] Starting connection...");
  Serial.print("[Firebase] API Key: ");
  Serial.println(FIREBASE_API_KEY.substring(0, 10) + "...");
  Serial.print("[Firebase] DB URL: ");
  Serial.println(FIREBASE_DB_URL);
  
  // Initialize Firebase with config and empty auth
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  
  // Set RTDB response size
  fbdo.setResponseSize(4096);
  
  // Important: Sign up anonymously to get a token
  Serial.println("[Firebase] Attempting anonymous sign-up...");
  if (Firebase.signUp(&config, &auth, "", "")) {
    Serial.println("[Firebase] Anonymous sign-up success!");
  } else {
    Serial.print("[Firebase] Sign-up failed: ");
    Serial.println(config.signer.signupError.message.c_str());
    firebaseLastError = String(config.signer.signupError.message.c_str());
  }
  
  // Wait for token generation
  int waitCount = 0;
  while (!Firebase.ready() && waitCount < 10) {
    Serial.print(".");
    delay(500);
    waitCount++;
  }
  Serial.println();
  
  firebaseReady = true;
  firebaseStatus = Firebase.ready() ? "Initialized" : "Init Failed";
  Serial.println("[Firebase] Initialization complete");
  Serial.print("[Firebase] Firebase.ready() = ");
  Serial.println(Firebase.ready() ? "true" : "false");
}

// Publish state to Firebase
void publishToFirebase() {
  if (!wifiConnected) {
    Serial.println("[Firebase] Publish skipped: WiFi not connected");
    return;
  }
  if (!firebaseReady) {
    Serial.println("[Firebase] Publish skipped: Firebase not initialized");
    firebaseStatus = "Not Initialized";
    firebaseConnected = false;
    return;
  }
  if (!Firebase.ready()) {
    Serial.print("[Firebase] Publish skipped: Firebase not ready - ");
    Serial.println(fbdo.errorReason());
    firebaseStatus = "Not Ready";
    firebaseLastError = fbdo.errorReason();
    firebaseConnected = false;
    return;
  }
  
  String path = "/devices/" + DEVICE_ID + "/state";
  Serial.print("[Firebase] Publishing to: ");
  Serial.println(path);
  
  FirebaseJson json;
  
  // Use dummy data in test mode
  if (testMode) {
    json.set("tempC", 37.5);
    json.set("humidity", 65.0);
    json.set("waterHigh", true);
    json.set("waterLow", false);
  } else {
    json.set("tempC", state.temperature);
    json.set("humidity", state.humidity);
    json.set("waterHigh", state.waterPresent);
    json.set("waterLow", !state.waterPresent);
  }
  
  json.set("heaterBulb", state.bulbState);
  json.set("fan", state.fanState);
  json.set("humidifier", state.humidifierState);
  json.set("eggTurner", state.eggTurnerState);
  json.set("bulbAutoMode", state.bulbAutoMode);
  json.set("humidifierAutoMode", state.humidifierAutoMode);
  json.set("fanScheduleEnabled", state.fanScheduleEnabled);
  json.set("eggTurnerScheduleEnabled", state.eggTurnerScheduleEnabled);
  json.set("buzzerEnabled", state.buzzerEnabled);
  // Thresholds & schedule config (so web app can read and set them)
  json.set("tempTrigger", state.tempTrigger);
  json.set("tempStop", state.tempStop);
  json.set("humidityTrigger", state.humidityTrigger);
  json.set("humidityStop", state.humidityStop);
  json.set("fanRunDuration", state.fanRunDuration);
  json.set("fanIdleDuration", state.fanIdleDuration);
  json.set("eggTurnerRunDuration", state.eggTurnerRunDuration);
  json.set("eggTurnerIdleDuration", state.eggTurnerIdleDuration);
  json.set("mode", onlineMode ? "online" : "offline");
  json.set("testMode", testMode);
  json.set("lastSeen/.sv", "timestamp");
  json.set("ip", WiFi.localIP().toString());
  
  if (Firebase.RTDB.setJSON(&fbdo, path.c_str(), &json)) {
    Serial.println("[Firebase] Publish SUCCESS");
    firebaseStatus = "Publishing OK";
    firebaseLastError = "";
    firebaseConnected = true;

    // Write device metadata on first successful publish or reconnect
    if (!metaPublished) {
      FirebaseJson metaJson;
      metaJson.set("deviceId", DEVICE_ID);
      metaJson.set("firmwareVersion", "1.0.0");
      metaJson.set("claimStatus", "unclaimed");
      metaJson.set("ip", WiFi.localIP().toString());
      metaJson.set("onlineStatus", true);
      metaJson.set("lastSeen/.sv", "timestamp");
      String metaPath = "/devices/" + DEVICE_ID + "/meta";
      if (Firebase.RTDB.updateNode(&fbdo, metaPath.c_str(), &metaJson)) {
        Serial.println("[Firebase] Meta published OK");
        metaPublished = true;
      } else {
        Serial.print("[Firebase] Meta publish failed: ");
        Serial.println(fbdo.errorReason());
      }
    }
  } else {
    Serial.print("[Firebase] Publish FAILED: ");
    Serial.println(fbdo.errorReason());
    firebaseStatus = "Publish Failed";
    firebaseLastError = fbdo.errorReason();
    firebaseConnected = false;
    metaPublished = false;  // Retry meta on next successful publish
  }
}

// Read commands from Firebase
void readFirebaseCommands() {
  if (!wifiConnected || !firebaseReady || !Firebase.ready()) return;
  
  String path = "/devices/" + DEVICE_ID + "/command";
  
  if (Firebase.RTDB.getJSON(&fbdo, path.c_str())) {
    FirebaseJson &json = fbdo.jsonObject();
    FirebaseJsonData jsonData;
    
    // Get command ID
    String cmdId = "";
    if (json.get(jsonData, "cmdId")) {
      cmdId = jsonData.stringValue;
      Serial.print("[Firebase] Command received, ID: ");
      Serial.println(cmdId);
    }
    
    // Only process new commands
    if (cmdId.length() > 0 && cmdId != lastCmdId) {
      Serial.println("[Firebase] Processing NEW command...");
      lastCmdId = cmdId;
      
      // Debug: Print the full JSON
      String jsonStr;
      json.toString(jsonStr, true);
      Serial.println("[Firebase] Command JSON:");
      Serial.println(jsonStr);
      
      // Process control commands
      if (json.get(jsonData, "heaterBulb")) {
        Serial.print("[Command] Heater: ");
        Serial.println(jsonData.boolValue ? "ON" : "OFF");
        updateRelayState(BULB_PIN, state.bulbState, jsonData.boolValue);
        state.bulbManualOverrideUntil = millis() + 30000;  // Override auto-mode for 30 seconds
      }
      if (json.get(jsonData, "fan")) {
        Serial.print("[Command] Fan: ");
        Serial.println(jsonData.boolValue ? "ON" : "OFF");
        updateRelayState(FAN_PIN, state.fanState, jsonData.boolValue);
        state.fanManualOverrideUntil = millis() + 30000;  // Override schedule for 30 seconds
      }
      if (json.get(jsonData, "humidifier")) {
        Serial.print("[Command] Humidifier: ");
        Serial.println(jsonData.boolValue ? "ON" : "OFF");
        updateRelayState(HUMIDIFIER_PIN, state.humidifierState, jsonData.boolValue);
        state.humidifierManualOverrideUntil = millis() + 30000;  // Override auto-mode for 30 seconds
      }
      if (json.get(jsonData, "eggTurner")) {
        Serial.print("[Command] Egg Turner: ");
        Serial.println(jsonData.boolValue ? "ON" : "OFF");
        updateRelayState(EGG_TURNER_PIN, state.eggTurnerState, jsonData.boolValue);
        state.turnerManualOverrideUntil = millis() + 30000;  // Override schedule for 30 seconds
      }
      if (json.get(jsonData, "bulbAutoMode")) {
        Serial.print("[Command] Bulb Auto Mode: ");
        Serial.println(jsonData.boolValue ? "ON" : "OFF");
        state.bulbAutoMode = jsonData.boolValue;
      }
      if (json.get(jsonData, "humidifierAutoMode")) {
        Serial.print("[Command] Humidifier Auto Mode: ");
        Serial.println(jsonData.boolValue ? "ON" : "OFF");
        state.humidifierAutoMode = jsonData.boolValue;
      }
      if (json.get(jsonData, "fanScheduleEnabled")) {
        Serial.print("[Command] Fan Schedule: ");
        Serial.println(jsonData.boolValue ? "ENABLED" : "DISABLED");
        state.fanScheduleEnabled = jsonData.boolValue;
        if (state.fanScheduleEnabled) {
          state.fanLastToggle = millis();
          state.fanScheduleRunning = false;
        }
      }
      if (json.get(jsonData, "eggTurnerScheduleEnabled")) {
        Serial.print("[Command] Turner Schedule: ");
        Serial.println(jsonData.boolValue ? "ENABLED" : "DISABLED");
        state.eggTurnerScheduleEnabled = jsonData.boolValue;
        if (state.eggTurnerScheduleEnabled) {
          state.eggTurnerLastToggle = millis();
          state.eggTurnerScheduleRunning = false;
        }
      }
      if (json.get(jsonData, "turnNow") && jsonData.boolValue) {
        Serial.println("[Command] Turn NOW triggered");
        // Start non-blocking egg turner turn (will be handled in handleScheduleModes)
        state.turnerTurnNowActive = true;
        state.turnerTurnNowUntil = millis() + (state.eggTurnerRunDuration * 1000);
        updateRelayState(EGG_TURNER_PIN, state.eggTurnerState, true);
        state.turnerManualOverrideUntil = millis() + (state.eggTurnerRunDuration * 1000 + 30000);  // Override schedule during turn
      }
      // Threshold & schedule config commands from web app
      if (json.get(jsonData, "tempTrigger")) { state.tempTrigger = jsonData.floatValue; prefs.putFloat("tempTrig", state.tempTrigger); }
      if (json.get(jsonData, "tempStop")) { state.tempStop = jsonData.floatValue; prefs.putFloat("tempStop", state.tempStop); }
      if (json.get(jsonData, "humidityTrigger")) { state.humidityTrigger = jsonData.floatValue; prefs.putFloat("humTrig", state.humidityTrigger); }
      if (json.get(jsonData, "humidityStop")) { state.humidityStop = jsonData.floatValue; prefs.putFloat("humStop", state.humidityStop); }
      if (json.get(jsonData, "fanRunDuration")) { state.fanRunDuration = jsonData.intValue; prefs.putInt("fanRun", state.fanRunDuration); }
      if (json.get(jsonData, "fanIdleDuration")) { state.fanIdleDuration = jsonData.intValue; prefs.putInt("fanIdle", state.fanIdleDuration); }
      if (json.get(jsonData, "eggTurnerRunDuration")) { state.eggTurnerRunDuration = jsonData.intValue; prefs.putInt("turnerRun", state.eggTurnerRunDuration); }
      if (json.get(jsonData, "eggTurnerIdleDuration")) { state.eggTurnerIdleDuration = jsonData.intValue; prefs.putInt("turnerIdle", state.eggTurnerIdleDuration); }
      
      // Update last processed command ID in Firebase
      String metaPath = "/devices/" + DEVICE_ID + "/meta/lastCmdId";
      Firebase.RTDB.setString(&fbdo, metaPath.c_str(), cmdId.c_str());
      Serial.println("[Firebase] Command processing complete");
    } else if (cmdId.length() > 0) {
      // Command already processed
      Serial.println("[Firebase] Command already processed (duplicate ID)");
    }
  } else {
    // Only log errors occasionally to avoid spam
    static unsigned long lastErrorLog = 0;
    if (millis() - lastErrorLog > 30000) {
      Serial.print("[Firebase] Command read failed: ");
      Serial.println(fbdo.errorReason());
      lastErrorLog = millis();
    }
  }
}

// Read system_controls/enabled from Firebase and apply to actuators.
// This is how the web app (frontend) controls the incubator remotely.
void readSystemControls() {
  if (!wifiConnected || !firebaseReady || !Firebase.ready()) return;

  String path = "/system_controls/enabled";
  if (Firebase.RTDB.getJSON(&fbdo, path.c_str())) {
    FirebaseJson &json = fbdo.jsonObject();
    FirebaseJsonData jsonData;

    // Map frontend key names → ESP32 actuator pins
    // "heater"     → heaterBulb (BULB_PIN)
    // "humidifier" → humidifier (HUMIDIFIER_PIN)
    // "exhaust"    → fan (FAN_PIN)
    // "turner"     → eggTurner (EGG_TURNER_PIN)
    if (json.get(jsonData, "heater")) {
      if (!state.bulbAutoMode) {  // skip if auto mode is handling this
        updateRelayState(BULB_PIN, state.bulbState, jsonData.boolValue);
      }
    }
    if (json.get(jsonData, "humidifier")) {
      if (!state.humidifierAutoMode) {
        updateRelayState(HUMIDIFIER_PIN, state.humidifierState, jsonData.boolValue);
      }
    }
    if (json.get(jsonData, "exhaust")) {
      if (!state.fanScheduleEnabled) {
        updateRelayState(FAN_PIN, state.fanState, jsonData.boolValue);
      }
    }
    if (json.get(jsonData, "turner")) {
      if (!state.eggTurnerScheduleEnabled) {
        updateRelayState(EGG_TURNER_PIN, state.eggTurnerState, jsonData.boolValue);
      }
    }
    Serial.println("[Firebase] system_controls applied");
  } else {
    static unsigned long lastSysCtrlErr = 0;
    if (millis() - lastSysCtrlErr > 30000) {
      Serial.print("[Firebase] system_controls read failed: ");
      Serial.println(fbdo.errorReason());
      lastSysCtrlErr = millis();
    }
  }
}

// Handle Firebase sync in loop
void handleFirebaseSync() {
  // Allow sync when Online Mode OR Test Mode is active
  if (!onlineMode && !testMode) {
    return;
  }
  
  if (!wifiConnected) {
    static unsigned long lastWifiWarn = 0;
    if (millis() - lastWifiWarn > 10000) {
      Serial.println("[Firebase Sync] Skipped - WiFi not connected");
      lastWifiWarn = millis();
    }
    return;
  }
  
  static unsigned long lastStatusPrint = 0;
  unsigned long now = millis();
  
  // Print Firebase status every 30 seconds
  if (now - lastStatusPrint >= 30000) {
    lastStatusPrint = now;
    Serial.println("--- Firebase Status ---");
    Serial.print("WiFi Connected: "); Serial.println(wifiConnected ? "Yes" : "No");
    Serial.print("Firebase Initialized: "); Serial.println(firebaseReady ? "Yes" : "No");
    Serial.print("Firebase.ready(): "); Serial.println(Firebase.ready() ? "Yes" : "No");
    Serial.print("Token Status: "); Serial.println(Firebase.getToken() != "" ? "Has Token" : "No Token");
    Serial.print("Online Mode: "); Serial.println(onlineMode ? "Yes" : "No");
    Serial.print("Last Command ID: "); Serial.println(lastCmdId.length() > 0 ? lastCmdId : "None");
    if (!Firebase.ready()) {
      Serial.print("Last Error: "); Serial.println(fbdo.errorReason());
    }
    Serial.println("-----------------------");
  }
  
  if (now - lastFirebaseSync >= FIREBASE_SYNC_INTERVAL) {
    lastFirebaseSync = now;
    Serial.println("[Firebase] Sync cycle starting...");
    publishToFirebase();
    readFirebaseCommands();
    Serial.println("[Firebase] Sync cycle complete");
  }
}

void readSensors() {
  // In test mode, use dummy sensor values so auto-modes work correctly
  if (testMode) {
    state.temperature = 37.5;
    state.humidity    = 65.0;
    state.waterPresent = true;
    return;
  }

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
  
  // Read water sensor with debounce (GPIO 4 now has internal pull-up support)
  static uint8_t waterDebounceCount = 0;
  static bool waterRawLast = false;
  bool waterRaw = digitalRead(WATER_SENSOR_PIN) == LOW;  // LOW = water present (pull-up logic)
  if (waterRaw != waterRawLast) {
    waterDebounceCount = 0;
    waterRawLast = waterRaw;
  } else {
    if (waterDebounceCount < 10) waterDebounceCount++;
  }
  if (waterDebounceCount >= 10) {
    state.waterPresent = waterRaw;
  }
}

void handleAutoModes() {
  unsigned long now = millis();

  // Safety: if sensor data is invalid, turn OFF auto-controlled actuators immediately
  if (state.bulbAutoMode && state.temperature <= -999) {
    if (state.bulbState) {
      updateRelayState(BULB_PIN, state.bulbState, false);
      Serial.println("[AutoMode] Heater forced OFF — invalid temperature sensor data");
    }
  }
  if (state.humidifierAutoMode && state.humidity <= -999) {
    if (state.humidifierState) {
      updateRelayState(HUMIDIFIER_PIN, state.humidifierState, false);
      Serial.println("[AutoMode] Humidifier forced OFF — invalid humidity sensor data");
    }
  }

  // Auto bulb control based on temperature (only if manual override has expired)
  if (state.bulbAutoMode && state.temperature > -999 && now >= state.bulbManualOverrideUntil) {
    if (!state.bulbState && state.temperature < state.tempTrigger) {
      updateRelayState(BULB_PIN, state.bulbState, true);
    } else if (state.bulbState && state.temperature >= state.tempStop) {
      updateRelayState(BULB_PIN, state.bulbState, false);
    }
  }
  
  // Auto humidifier control based on humidity (only if manual override has expired)
  if (state.humidifierAutoMode && state.humidity > -999 && now >= state.humidifierManualOverrideUntil) {
    if (!state.humidifierState && state.humidity < state.humidityTrigger) {
      updateRelayState(HUMIDIFIER_PIN, state.humidifierState, true);
    } else if (state.humidifierState && state.humidity >= state.humidityStop) {
      updateRelayState(HUMIDIFIER_PIN, state.humidifierState, false);
    }
  }
}

void handleScheduleModes() {
  unsigned long currentTime = millis();
  
  // Handle egg turner turn-now timer (non-blocking)
  if (state.turnerTurnNowActive) {
    if (currentTime >= state.turnerTurnNowUntil) {
      updateRelayState(EGG_TURNER_PIN, state.eggTurnerState, false);
      state.turnerTurnNowActive = false;
      Serial.println("[Schedule] Turn NOW complete");
    }
    return;  // Skip schedule while turn-now is active
  }
  
  // Fan schedule (only if manual override has expired)
  if (state.fanScheduleEnabled && currentTime >= state.fanManualOverrideUntil) {
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
  
  // Egg turner schedule (only if manual override has expired)
  if (state.eggTurnerScheduleEnabled && currentTime >= state.turnerManualOverrideUntil) {
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
  if (!state.waterPresent) {
    // Continuous buzzer when no water
    if (state.buzzerEnabled && (millis() % 2000 < 1000)) {
      digitalWrite(BUZZER_PIN, HIGH);
    } else {
      digitalWrite(BUZZER_PIN, LOW);
    }
  } else {
    digitalWrite(BUZZER_PIN, LOW);
  }
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
  // Calculate next egg turn countdown
  int turnerCountdown = 0;
  if (state.eggTurnerScheduleEnabled) {
    if (state.eggTurnerScheduleRunning) {
      turnerCountdown = state.eggTurnerRunDuration - (millis() - state.eggTurnerLastToggle)/1000;
    } else {
      turnerCountdown = state.eggTurnerIdleDuration - (millis() - state.eggTurnerLastToggle)/1000;
    }
    if (turnerCountdown < 0) turnerCountdown = 0;
  }
  
  // Determine status colors
  String tempColor = "normal";
  if (state.temperature > -999) {
    if (state.temperature < state.tempTrigger - 1 || state.temperature > state.tempStop + 1) {
      tempColor = "danger";
    } else if (state.temperature < state.tempTrigger || state.temperature > state.tempStop) {
      tempColor = "warn";
    }
  } else {
    tempColor = "danger";
  }
  
  String humColor = "normal";
  if (state.humidity > -999) {
    if (state.humidity < state.humidityTrigger - 5 || state.humidity > state.humidityStop + 5) {
      humColor = "danger";
    } else if (state.humidity < state.humidityTrigger || state.humidity > state.humidityStop) {
      humColor = "warn";
    }
  } else {
    humColor = "danger";
  }

  String html = R"(<!DOCTYPE html><html><head><meta charset='utf-8'><title>Egg Incubator</title>
<meta name='viewport' content='width=device-width,initial-scale=1,user-scalable=no'>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,system-ui,sans-serif;background:#f5f5f5;color:#333;font-size:16px}
.header{background:#2c3e50;color:#fff;padding:12px;text-align:center;position:sticky;top:0;z-index:100}
.header h1{font-size:18px;font-weight:600}
.tabs{display:flex;background:#34495e}
.tab{flex:1;padding:12px;text-align:center;color:#bdc3c7;cursor:pointer;border:none;background:none;font-size:15px}
.tab.active{background:#2c3e50;color:#fff;font-weight:600}
.panel{display:none;padding:12px}
.panel.active{display:block}
.card{background:#fff;border-radius:8px;padding:12px;margin-bottom:10px;box-shadow:0 1px 3px rgba(0,0,0,0.1)}
.big-num{font-size:42px;font-weight:700;text-align:center;padding:8px 0}
.big-num.normal{color:#27ae60}
.big-num.warn{color:#f39c12}
.big-num.danger{color:#e74c3c}
.label{font-size:12px;color:#7f8c8d;text-align:center;text-transform:uppercase;letter-spacing:1px}
.grid2{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.grid3{display:grid;grid-template-columns:1fr 1fr 1fr;gap:8px}
.status-row{display:flex;justify-content:space-between;align-items:center;padding:10px 0;border-bottom:1px solid #ecf0f1}
.status-row:last-child{border:none}
.status-label{font-size:14px;color:#555}
.status-val{font-weight:600;font-size:14px}
.status-val.on{color:#27ae60}
.status-val.off{color:#95a5a6}
.status-val.low{color:#e74c3c}
.status-val.high{color:#27ae60}
.btn{padding:12px 16px;border:none;border-radius:6px;font-size:14px;font-weight:600;cursor:pointer;width:100%}
.btn-on{background:#27ae60;color:#fff}
.btn-off{background:#e74c3c;color:#fff}
.btn-action{background:#3498db;color:#fff}
.btn-sec{background:#ecf0f1;color:#333}
.btn:active{opacity:0.8}
.form-row{margin-bottom:12px}
.form-row label{display:block;font-size:13px;color:#555;margin-bottom:4px}
.form-row input{width:100%;padding:10px;border:1px solid #ddd;border-radius:6px;font-size:16px}
.section-title{font-size:14px;font-weight:600;color:#2c3e50;margin:16px 0 8px;text-transform:uppercase;letter-spacing:0.5px}
.toggle-row{display:flex;justify-content:space-between;align-items:center;padding:12px 0}
.toggle{position:relative;width:50px;height:28px}
.toggle input{opacity:0;width:0;height:0}
.toggle-slider{position:absolute;cursor:pointer;top:0;left:0;right:0;bottom:0;background:#ccc;border-radius:28px;transition:.3s}
.toggle-slider:before{position:absolute;content:'';height:22px;width:22px;left:3px;bottom:3px;background:#fff;border-radius:50%;transition:.3s}
.toggle input:checked+.toggle-slider{background:#27ae60}
.toggle input:checked+.toggle-slider:before{transform:translateX(22px)}
.info-bar{background:#ecf0f1;padding:8px 12px;font-size:12px;color:#7f8c8d;display:flex;justify-content:space-between}
.toast{position:fixed;bottom:20px;left:50%;transform:translateX(-50%);background:#333;color:#fff;padding:12px 24px;border-radius:25px;font-size:14px;z-index:200;opacity:0;transition:opacity .3s}
.toast.show{opacity:1}
.countdown{font-size:24px;font-weight:600;color:#3498db;text-align:center}
</style></head><body>
<div class='header'><h1>Egg Incubator</h1></div>
<div class='tabs'>
<button class='tab active' onclick='showTab(0)'>Dashboard</button>
<button class='tab' onclick='showTab(1)'>Settings</button>
<button class='tab' onclick='showTab(2)'>WiFi</button>
</div>)";

  // Dashboard Panel
  html += "<div class='panel active' id='p0'>";
  
  // Temperature and Humidity - Large Display
  html += "<div class='grid2'>";
  html += "<div class='card'><div class='label'>Temperature</div><div class='big-num " + tempColor + "' id='temp-val'>";
  html += (state.temperature > -999 ? String(state.temperature, 1) : "--") + "</div><div class='label'>°C</div></div>";
  html += "<div class='card'><div class='label'>Humidity</div><div class='big-num " + humColor + "' id='hum-val'>";
  html += (state.humidity > -999 ? String((int)state.humidity) : "--") + "</div><div class='label'>%</div></div>";
  html += "</div>";
  
  // Status Grid
  html += "<div class='card'>";
  html += "<div class='status-row'><span class='status-label'>Water Level</span><span class='status-val " + String(state.waterPresent ? "high" : "low") + "' id='water-val'>" + String(state.waterPresent ? "HIGH" : "LOW") + "</span></div>";
  html += "<div class='status-row'><span class='status-label'>Heater</span><span class='status-val " + String(state.bulbState ? "on" : "off") + "' id='heater-val'>" + String(state.bulbState ? "ON" : "OFF") + "</span></div>";
  html += "<div class='status-row'><span class='status-label'>Humidifier</span><span class='status-val " + String(state.humidifierState ? "on" : "off") + "' id='humidifier-val'>" + String(state.humidifierState ? "ON" : "OFF") + "</span></div>";
  html += "<div class='status-row'><span class='status-label'>Fan</span><span class='status-val " + String(state.fanState ? "on" : "off") + "' id='fan-val'>" + String(state.fanState ? "ON" : "OFF") + "</span></div>";
  html += "<div class='status-row'><span class='status-label'>Egg Turner</span><span class='status-val " + String(state.eggTurnerState ? "on" : "off") + "' id='turner-val'>" + String(state.eggTurnerState ? "ON" : "OFF") + "</span></div>";
  html += "</div>";
  
  // Egg Turner Countdown
  html += "<div class='card'>";
  html += "<div class='label'>Next Egg Turn</div>";
  html += "<div class='countdown' id='turner-countdown'>" + formatTime(turnerCountdown) + "</div>";
  html += "</div>";
  
  // Quick Controls
  html += "<div class='section-title'>Quick Controls</div>";
  html += "<div class='grid2'>";
  html += "<button class='btn " + String(state.bulbState ? "btn-off" : "btn-on") + "' id='btn-heater' onclick='toggle(\"bulb\")'>" + String(state.bulbState ? "Heater Off" : "Heater On") + "</button>";
  html += "<button class='btn " + String(state.humidifierState ? "btn-off" : "btn-on") + "' id='btn-humidifier' onclick='toggle(\"humidifier\")'>" + String(state.humidifierState ? "Humidifier Off" : "Humidifier On") + "</button>";
  html += "<button class='btn " + String(state.fanState ? "btn-off" : "btn-on") + "' id='btn-fan' onclick='toggle(\"fan\")'>" + String(state.fanState ? "Fan Off" : "Fan On") + "</button>";
  html += "<button class='btn " + String(state.eggTurnerState ? "btn-off" : "btn-on") + "' id='btn-turner' onclick='toggle(\"turner\")'>" + String(state.eggTurnerState ? "Turner Off" : "Turner On") + "</button>";
  html += "</div>";
  html += "<button class='btn btn-action' style='margin-top:8px;width:100%' onclick='turnNow()'>Turn Now (Timed)</button>";
  
  // Status Info
  html += "<div class='info-bar' style='margin-top:12px;border-radius:6px'>";
  html += "<span>Mode: <strong id='mode-val'>" + String(onlineMode ? "ONLINE" : "OFFLINE") + "</strong></span>";
  html += "<span id='update-time'>Updated: now</span>";
  html += "</div>";
  
  html += "</div>"; // End Dashboard Panel
  
  // Settings Panel
  html += "<div class='panel' id='p1'>";
  
  // Online mode lockout notice
  if (onlineMode) {
    html += "<div class='card' style='background:#fff7ed;border:1px solid #fed7aa;margin-bottom:10px'>";
    html += "<p style='font-size:13px;color:#c2410c;font-weight:600;margin-bottom:4px'>&#x1F512; Online Mode Active</p>";
    html += "<p style='font-size:12px;color:#9a3412'>Auto modes, schedules, and thresholds are controlled by the web app. Use the web app to change settings.</p>";
    html += "</div>";
  }
  html += "<div class='card'>";
  html += "<div class='section-title' style='margin-top:0'>Temperature Control</div>";
  html += "<div class='toggle-row'><span>Auto Mode</span><label class='toggle'><input type='checkbox' id='bulb-auto' " + String(state.bulbAutoMode ? "checked" : "") + " onchange='setAuto(\"bulb\",this.checked)'><span class='toggle-slider'></span></label></div>";
  html += "<div class='grid2'>";
  html += "<div class='form-row'><label>Heat ON below (°C)</label><input type='number' step='0.1' id='temp-trigger' value='" + String(state.tempTrigger, 1) + "'></div>";
  html += "<div class='form-row'><label>Heat OFF above (°C)</label><input type='number' step='0.1' id='temp-stop' value='" + String(state.tempStop, 1) + "'></div>";
  html += "</div>";
  html += "<button class='btn btn-action' onclick='saveTemp()'>Save Temperature Settings</button>";
  html += "</div>";
  
  // Humidity Settings
  html += "<div class='card'>";
  html += "<div class='section-title' style='margin-top:0'>Humidity Control</div>";
  html += "<div class='toggle-row'><span>Auto Mode</span><label class='toggle'><input type='checkbox' id='hum-auto' " + String(state.humidifierAutoMode ? "checked" : "") + " onchange='setAuto(\"humidifier\",this.checked)'><span class='toggle-slider'></span></label></div>";
  html += "<div class='grid2'>";
  html += "<div class='form-row'><label>Humidify ON below (%)</label><input type='number' step='1' id='hum-trigger' value='" + String((int)state.humidityTrigger) + "'></div>";
  html += "<div class='form-row'><label>Humidify OFF above (%)</label><input type='number' step='1' id='hum-stop' value='" + String((int)state.humidityStop) + "'></div>";
  html += "</div>";
  html += "<button class='btn btn-action' onclick='saveHum()'>Save Humidity Settings</button>";
  html += "</div>";
  
  // Fan Schedule
  html += "<div class='card'>";
  html += "<div class='section-title' style='margin-top:0'>Fan Schedule</div>";
  html += "<div class='toggle-row'><span>Schedule Enabled</span><label class='toggle'><input type='checkbox' id='fan-sched' " + String(state.fanScheduleEnabled ? "checked" : "") + " onchange='setSched(\"fan\",this.checked)'><span class='toggle-slider'></span></label></div>";
  html += "<div class='grid2'>";
  html += "<div class='form-row'><label>Run (seconds)</label><input type='number' id='fan-run' value='" + String(state.fanRunDuration) + "'></div>";
  html += "<div class='form-row'><label>Idle (seconds)</label><input type='number' id='fan-idle' value='" + String(state.fanIdleDuration) + "'></div>";
  html += "</div>";
  html += "<button class='btn btn-action' onclick='saveFanTiming()'>Save Fan Timing</button>";
  html += "</div>";
  
  // Egg Turner Schedule
  html += "<div class='card'>";
  html += "<div class='section-title' style='margin-top:0'>Egg Turner Schedule</div>";
  html += "<div class='toggle-row'><span>Schedule Enabled</span><label class='toggle'><input type='checkbox' id='turner-sched' " + String(state.eggTurnerScheduleEnabled ? "checked" : "") + " onchange='setSched(\"turner\",this.checked)'><span class='toggle-slider'></span></label></div>";
  html += "<div class='grid2'>";
  html += "<div class='form-row'><label>Turn Duration (sec)</label><input type='number' id='turner-run' value='" + String(state.eggTurnerRunDuration) + "'></div>";
  html += "<div class='form-row'><label>Interval (seconds)</label><input type='number' id='turner-idle' value='" + String(state.eggTurnerIdleDuration) + "'></div>";
  html += "</div>";
  html += "<button class='btn btn-action' onclick='saveTurnerTiming()'>Save Turner Timing</button>";
  html += "</div>";
  
  // Alerts & SMS
  // Alerts
  html += "<div class='card'>";
  html += "<div class='section-title' style='margin-top:0'>Alerts</div>";
  html += "<div class='toggle-row'><span>Buzzer Alarm</span><label class='toggle'><input type='checkbox' id='buzzer-en' " + String(state.buzzerEnabled ? "checked" : "") + " onchange='setBuzzer(this.checked)'><span class='toggle-slider'></span></label></div>";
  html += "</div>";
  
  // System
  html += "<div class='card'>";
  html += "<div class='section-title' style='margin-top:0'>System</div>";
  html += "<button class='btn btn-off' onclick='restart()'>Restart System</button>";
  html += "</div>";
  
  html += "</div>"; // End Settings Panel
  
  // WiFi Panel
  html += "<div class='panel' id='p2'>";
  
  // Online Mode Toggle
  html += "<div class='card'>";
  html += "<div class='section-title' style='margin-top:0'>Operating Mode</div>";
  html += "<div class='toggle-row'><span>Online Mode</span><label class='toggle'><input type='checkbox' id='online-mode' " + String(onlineMode ? "checked" : "") + " onchange='setOnlineMode(this.checked)'><span class='toggle-slider'></span></label></div>";
  html += "<div class='toggle-row'><span>Test Mode (Dummy Data)</span><label class='toggle'><input type='checkbox' id='test-mode' " + String(testMode ? "checked" : "") + " onchange='setTestMode(this.checked)'><span class='toggle-slider'></span></label></div>";
  html += "<p style='font-size:12px;color:#7f8c8d;margin-top:8px'><strong>Offline:</strong> Local control only via this web UI</p>";
  html += "<p style='font-size:12px;color:#7f8c8d;margin-top:4px'><strong>Online:</strong> Syncs with Firebase for remote control</p>";
  html += "<p style='font-size:12px;color:#3498db;margin-top:4px'><strong>Test Mode:</strong> Sends dummy sensor data (37.5°C, 65% humidity)</p>";
  html += "</div>";
  
  // WiFi Status
  html += "<div class='card'>";
  html += "<div class='section-title' style='margin-top:0'>WiFi Status</div>";
  html += "<div class='status-row'><span class='status-label'>Connection</span><span class='status-val " + String(wifiConnected ? "on" : "off") + "' id='wifi-status'>" + String(wifiConnected ? "CONNECTED" : "DISCONNECTED") + "</span></div>";
  if (wifiConnected) {
    html += "<div class='status-row'><span class='status-label'>IP Address</span><span class='status-val'>" + WiFi.localIP().toString() + "</span></div>";
    html += "<div class='status-row'><span class='status-label'>Signal</span><span class='status-val'>" + String(WiFi.RSSI()) + " dBm</span></div>";
  }
  html += "<div class='status-row'><span class='status-label'>AP IP</span><span class='status-val'>192.168.4.1</span></div>";
  html += "</div>";
  
  // Firebase Status
  html += "<div class='card'>";
  html += "<div class='section-title' style='margin-top:0'>Firebase Status</div>";
  html += "<div class='status-row'><span class='status-label'>Connection</span><span class='status-val " + String(firebaseConnected ? "on" : "off") + "' id='fb-status'>" + String(firebaseConnected ? "CONNECTED" : "DISCONNECTED") + "</span></div>";
  html += "<div class='status-row'><span class='status-label'>Status</span><span class='status-val' id='fb-status-msg'>" + firebaseStatus + "</span></div>";
  html += "<div class='status-row'><span class='status-label'>Error</span><span class='status-val " + String(firebaseLastError.length() > 0 ? "off" : "") + "' id='fb-error'>" + (firebaseLastError.length() > 0 ? firebaseLastError : "None") + "</span></div>";
  html += "<div class='status-row'><span class='status-label'>Firebase Ready</span><span class='status-val " + String(Firebase.ready() ? "on" : "off") + "'>" + String(Firebase.ready() ? "YES" : "NO") + "</span></div>";
  html += "</div>";
  
  // WiFi Configuration
  html += "<div class='card'>";
  html += "<div class='section-title' style='margin-top:0'>WiFi Network</div>";
  html += "<div class='form-row'><label>WiFi SSID</label><input type='text' id='sta-ssid' value='" + staSSID + "' placeholder='Your WiFi network name'></div>";
  html += "<div class='form-row'><label>WiFi Password</label><input type='password' id='sta-pass' value='" + staPassword + "' placeholder='Your WiFi password'></div>";
  html += "<button class='btn btn-action' onclick='saveWiFi()'>Save WiFi Settings</button>";
  html += "</div>";
  
  // Firebase Configuration
  html += "<div class='card'>";
  html += "<div class='section-title' style='margin-top:0'>Firebase Configuration</div>";
  html += "<div class='status-row'><span class='status-label'>Device ID</span><span class='status-val' style='font-family:monospace;font-size:1.1em;font-weight:bold;color:#1d4ed8'>" + DEVICE_ID + "</span></div>";
  html += "<p style='font-size:0.75em;color:#64748b;margin-bottom:8px'>Use this Device ID when adding this incubator in the web app.</p>";
  html += "<div class='form-row'><label>API Key</label><input type='text' id='fb-api-key' value='" + FIREBASE_API_KEY + "' placeholder='Web API Key from Firebase Console'></div>";
  html += "<div class='form-row'><label>Database URL</label><input type='text' id='fb-db-url' value='" + FIREBASE_DB_URL + "' placeholder='https://your-project.firebaseio.com'></div>";
  html += "<div class='form-row'><label>Device ID (override)</label><input type='text' id='device-id' value='" + DEVICE_ID + "' placeholder='Auto-generated from MAC'></div>";
  html += "<button class='btn btn-action' onclick='saveFirebase()'>Save Firebase Settings</button>";
  html += "</div>";
  
  // Connect/Reconnect Button
  html += "<div class='card'>";
  html += "<button class='btn btn-on' onclick='reconnectWifi()'>Connect to WiFi</button>";
  html += "</div>";
  
  html += "</div>"; // End WiFi Panel
  
  // Toast notification
  html += "<div class='toast' id='toast'></div>";
  
  // JavaScript
  html += R"(<script>
let autoRefresh=true,lastUpdate=Date.now(),turnerCountdownVal=0;
function formatCountdown(s){const h=Math.floor(s/3600),m=Math.floor((s%3600)/60),sec=s%60;return (h<10?'0'+h:h)+':'+(m<10?'0'+m:m)+':'+(sec<10?'0'+sec:sec);}
function showTab(n){document.querySelectorAll('.tab').forEach((t,i)=>t.classList.toggle('active',i===n));document.querySelectorAll('.panel').forEach((p,i)=>p.classList.toggle('active',i===n));}
function toast(m){const t=document.getElementById('toast');t.textContent=m;t.classList.add('show');setTimeout(()=>t.classList.remove('show'),2000);}
function api(u,cb){fetch('/api/'+u,{method:'POST'}).then(r=>r.json()).then(d=>{if(d.success){cb&&cb(d);toast('Saved!');}}).catch(()=>toast('Error'));}
function toggle(d){api(d+'/toggle_state',()=>setTimeout(refresh,200));}
function turnNow(){api('turner/on',()=>{toast('Turning eggs...');setTimeout(()=>api('turner/off'),5000);});}
function setAuto(d,v){api(d+'/auto_'+(v?'on':'off'),()=>refresh());}
function setSched(d,v){api(d+'/schedule_'+(v?'on':'off'),()=>refresh());}
function setBuzzer(v){api('buzzer/'+(v?'enable':'disable'));}
function saveTemp(){const fd=new FormData();fd.append('trigger',document.getElementById('temp-trigger').value);fd.append('stop',document.getElementById('temp-stop').value);fetch('/api/bulb/thresholds',{method:'POST',body:fd}).then(r=>r.json()).then(()=>toast('Saved!')).catch(()=>toast('Error'));}
function saveHum(){const fd=new FormData();fd.append('trigger',document.getElementById('hum-trigger').value);fd.append('stop',document.getElementById('hum-stop').value);fetch('/api/humidifier/thresholds',{method:'POST',body:fd}).then(r=>r.json()).then(()=>toast('Saved!')).catch(()=>toast('Error'));}
function saveFanTiming(){const fd=new FormData();fd.append('run',document.getElementById('fan-run').value);fd.append('idle',document.getElementById('fan-idle').value);fetch('/api/fan/timing',{method:'POST',body:fd}).then(r=>r.json()).then(()=>toast('Saved!')).catch(()=>toast('Error'));}
function saveTurnerTiming(){const fd=new FormData();fd.append('run',document.getElementById('turner-run').value);fd.append('idle',document.getElementById('turner-idle').value);fetch('/api/turner/timing',{method:'POST',body:fd}).then(r=>r.json()).then(()=>toast('Saved!')).catch(()=>toast('Error'));}
function restart(){if(confirm('Restart the system?')){fetch('/restart',{method:'POST'});toast('Restarting...');}}
function setOnlineMode(v){const fd=new FormData();fd.append('mode',v?'online':'offline');fetch('/api/mode',{method:'POST',body:fd}).then(r=>r.json()).then(d=>{if(d.success){toast(v?'Online mode enabled':'Offline mode enabled');document.getElementById('mode-val').textContent=v?'ONLINE':'OFFLINE';}}).catch(()=>toast('Error'));}
function setTestMode(v){fetch('/api/testmode/'+(v?'on':'off'),{method:'POST'}).then(r=>r.json()).then(d=>{if(d.success){toast(v?'Test mode enabled':'Test mode disabled');}}).catch(()=>toast('Error'));}
function saveWiFi(){const fd=new FormData();fd.append('ssid',document.getElementById('sta-ssid').value);fd.append('password',document.getElementById('sta-pass').value);fetch('/api/wifi',{method:'POST',body:fd}).then(r=>r.json()).then(()=>toast('WiFi settings saved!')).catch(()=>toast('Error'));}
function saveFirebase(){const fd=new FormData();fd.append('apiKey',document.getElementById('fb-api-key').value);fd.append('dbUrl',document.getElementById('fb-db-url').value);fd.append('deviceId',document.getElementById('device-id').value);fetch('/api/firebase',{method:'POST',body:fd}).then(r=>r.json()).then(()=>toast('Firebase settings saved!')).catch(()=>toast('Error'));}
function reconnectWifi(){fetch('/api/wifi/connect',{method:'POST'}).then(r=>r.json()).then(d=>{toast(d.connected?'Connected to WiFi!':'Connection failed');setTimeout(()=>location.reload(),1000);}).catch(()=>toast('Error'));}
function refreshFirebaseStatus(){fetch('/api/firebase/status').then(r=>r.json()).then(d=>{const el=document.getElementById('fb-status');if(el){el.textContent=d.connected?'CONNECTED':'DISCONNECTED';el.className='status-val '+(d.connected?'on':'off');}const msgEl=document.getElementById('fb-status-msg');if(msgEl)msgEl.textContent=d.status;const errEl=document.getElementById('fb-error');if(errEl)errEl.textContent=d.error||'None';}).catch(()=>{});}
setInterval(refreshFirebaseStatus,5000);
function refresh(){
fetch('/api/status').then(r=>r.json()).then(d=>{
lastUpdate=Date.now();
const tempEl=document.getElementById('temp-val');
const humEl=document.getElementById('hum-val');
if(d.temperature>-999){tempEl.textContent=d.temperature.toFixed(1);tempEl.className='big-num '+(d.temperature<d.tempTrigger-1||d.temperature>d.tempStop+1?'danger':d.temperature<d.tempTrigger||d.temperature>d.tempStop?'warn':'normal');}else{tempEl.textContent='--';tempEl.className='big-num danger';}
if(d.humidity>-999){humEl.textContent=Math.round(d.humidity);humEl.className='big-num '+(d.humidity<d.humidityTrigger-5||d.humidity>d.humidityStop+5?'danger':d.humidity<d.humidityTrigger||d.humidity>d.humidityStop?'warn':'normal');}else{humEl.textContent='--';humEl.className='big-num danger';}
document.getElementById('water-val').textContent=d.waterPresent?'HIGH':'LOW';document.getElementById('water-val').className='status-val '+(d.waterPresent?'high':'low');
document.getElementById('heater-val').textContent=d.bulbState?'ON':'OFF';document.getElementById('heater-val').className='status-val '+(d.bulbState?'on':'off');
document.getElementById('humidifier-val').textContent=d.humidifierState?'ON':'OFF';document.getElementById('humidifier-val').className='status-val '+(d.humidifierState?'on':'off');
document.getElementById('fan-val').textContent=d.fanState?'ON':'OFF';document.getElementById('fan-val').className='status-val '+(d.fanState?'on':'off');
document.getElementById('turner-val').textContent=d.eggTurnerState?'ON':'OFF';document.getElementById('turner-val').className='status-val '+(d.eggTurnerState?'on':'off');
const btnHeater=document.getElementById('btn-heater');btnHeater.textContent=d.bulbState?'Heater Off':'Heater On';btnHeater.className='btn '+(d.bulbState?'btn-off':'btn-on');
const btnHumidifier=document.getElementById('btn-humidifier');btnHumidifier.textContent=d.humidifierState?'Humidifier Off':'Humidifier On';btnHumidifier.className='btn '+(d.humidifierState?'btn-off':'btn-on');
const btnFan=document.getElementById('btn-fan');btnFan.textContent=d.fanState?'Fan Off':'Fan On';btnFan.className='btn '+(d.fanState?'btn-off':'btn-on');
const btnTurner=document.getElementById('btn-turner');if(btnTurner){btnTurner.textContent=d.eggTurnerState?'Turner Off':'Turner On';btnTurner.className='btn '+(d.eggTurnerState?'btn-off':'btn-on');}
if(typeof d.turnerCountdown!=='undefined'){turnerCountdownVal=d.turnerCountdown;}
}).catch(()=>{});}
function updateTime(){const s=Math.floor((Date.now()-lastUpdate)/1000);document.getElementById('update-time').textContent='Updated: '+(s<5?'now':s+'s ago');if(turnerCountdownVal>0){turnerCountdownVal--;document.getElementById('turner-countdown').textContent=formatCountdown(turnerCountdownVal);}}
setInterval(()=>{if(autoRefresh)refresh();},2000);
setInterval(updateTime,1000);
document.querySelectorAll('input').forEach(i=>{i.addEventListener('focus',()=>autoRefresh=false);i.addEventListener('blur',()=>autoRefresh=true);});
refresh();
</script></body></html>)";
  
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
  // WiFi and Online mode settings
  staSSID = prefs.getString("staSSID", "");
  staPassword = prefs.getString("staPASS", "");
  onlineMode = prefs.getBool("onlineMode", false);
  FIREBASE_API_KEY = prefs.getString("fbApiKey", "");
  FIREBASE_DB_URL = prefs.getString("fbDbUrl", "");
  DEVICE_ID = prefs.getString("deviceId", "");
  prefs.end();

  // Auto-generate device ID from MAC address if not set or still default
  if (DEVICE_ID.length() == 0 || DEVICE_ID == "incubator1") {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    DEVICE_ID = "INC-";
    for (int i = 2; i < 6; i++) {
      if (mac[i] < 0x10) DEVICE_ID += "0";
      DEVICE_ID += String(mac[i], HEX);
    }
    DEVICE_ID.toUpperCase();
    // Persist the newly generated ID immediately
    prefs.begin("incubator", false);
    prefs.putString("deviceId", DEVICE_ID);
    prefs.end();
    Serial.print("[Device] Generated unique device ID: ");
    Serial.println(DEVICE_ID);
  }
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
  // WiFi and Online mode settings
  prefs.putString("staSSID", staSSID);
  prefs.putString("staPASS", staPassword);
  prefs.putBool("onlineMode", onlineMode);
  prefs.putString("fbApiKey", FIREBASE_API_KEY);
  prefs.putString("fbDbUrl", FIREBASE_DB_URL);
  prefs.putString("deviceId", DEVICE_ID);
  prefs.end();
}

// Returns locked response when online mode blocks a local config change
String onlineLocked() {
  return "{\"success\":false,\"locked\":true,\"reason\":\"Device is in online mode. Use the web app to configure.\"}";
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
    json += "\"tempTrigger\":" + String(state.tempTrigger, 1) + ",";
    json += "\"tempStop\":" + String(state.tempStop, 1) + ",";
    json += "\"humidityTrigger\":" + String(state.humidityTrigger, 1) + ",";
    json += "\"humidityStop\":" + String(state.humidityStop, 1) + ",";
    json += "\"fanRunDuration\":" + String(state.fanRunDuration) + ",";
    json += "\"fanIdleDuration\":" + String(state.fanIdleDuration) + ",";
    json += "\"eggTurnerRunDuration\":" + String(state.eggTurnerRunDuration) + ",";
    json += "\"eggTurnerIdleDuration\":" + String(state.eggTurnerIdleDuration) + ",";
    json += "\"onlineMode\":" + String(onlineMode ? "true" : "false") + ",";
    int turnerCountdown = 0;
    if (state.eggTurnerScheduleEnabled) {
      if (state.eggTurnerScheduleRunning) {
        turnerCountdown = state.eggTurnerRunDuration - (int)((millis() - state.eggTurnerLastToggle) / 1000);
      } else {
        turnerCountdown = state.eggTurnerIdleDuration - (int)((millis() - state.eggTurnerLastToggle) / 1000);
      }
      if (turnerCountdown < 0) turnerCountdown = 0;
    }
    json += "\"turnerCountdown\":" + String(turnerCountdown) + ",";
    json += "\"wifiConnected\":" + String(wifiConnected ? "true" : "false");
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
    if (onlineMode) { request->send(423, "application/json", onlineLocked()); return; }
    state.fanScheduleEnabled = !state.fanScheduleEnabled;
    if (state.fanScheduleEnabled) {
      state.fanLastToggle = millis();
      state.fanScheduleRunning = false;
    }
    request->send(200, "application/json", "{\"success\":true,\"scheduleEnabled\":" + String(state.fanScheduleEnabled ? "true" : "false") + "}");
  });

  server.on("/api/fan/timing", HTTP_POST, [](AsyncWebServerRequest *request){
    if (onlineMode) { request->send(423, "application/json", onlineLocked()); return; }
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
    if (onlineMode) { request->send(423, "application/json", onlineLocked()); return; }
    state.eggTurnerScheduleEnabled = !state.eggTurnerScheduleEnabled;
    if (state.eggTurnerScheduleEnabled) {
      state.eggTurnerLastToggle = millis();
      state.eggTurnerScheduleRunning = false;
    }
    request->send(200, "application/json", "{\"success\":true,\"scheduleEnabled\":" + String(state.eggTurnerScheduleEnabled ? "true" : "false") + "}");
  });

  server.on("/api/turner/timing", HTTP_POST, [](AsyncWebServerRequest *request){
    if (onlineMode) { request->send(423, "application/json", onlineLocked()); return; }
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
    if (onlineMode) { request->send(423, "application/json", onlineLocked()); return; }
    state.humidifierAutoMode = !state.humidifierAutoMode;
    request->send(200, "application/json", "{\"success\":true,\"autoMode\":" + String(state.humidifierAutoMode ? "true" : "false") + "}");
  });

  server.on("/api/humidifier/thresholds", HTTP_POST, [](AsyncWebServerRequest *request){
    if (onlineMode) { request->send(423, "application/json", onlineLocked()); return; }
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
    if (onlineMode) { request->send(423, "application/json", onlineLocked()); return; }
    state.bulbAutoMode = !state.bulbAutoMode;
    request->send(200, "application/json", "{\"success\":true,\"autoMode\":" + String(state.bulbAutoMode ? "true" : "false") + "}");
  });

  server.on("/api/bulb/thresholds", HTTP_POST, [](AsyncWebServerRequest *request){
    if (onlineMode) { request->send(423, "application/json", onlineLocked()); return; }
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

  // Toggle state endpoints for new UI
  server.on("/api/bulb/toggle_state", HTTP_POST, [](AsyncWebServerRequest *request){
    bool newState = !state.bulbState;
    updateRelayState(BULB_PIN, state.bulbState, newState);
    state.bulbManualOverrideUntil = millis() + 30000;
    request->send(200, "application/json", "{\"success\":true,\"state\":" + String(newState ? "true" : "false") + "}");
  });

  server.on("/api/fan/toggle_state", HTTP_POST, [](AsyncWebServerRequest *request){
    bool newState = !state.fanState;
    updateRelayState(FAN_PIN, state.fanState, newState);
    state.fanManualOverrideUntil = millis() + 30000;
    request->send(200, "application/json", "{\"success\":true,\"state\":" + String(newState ? "true" : "false") + "}");
  });

  server.on("/api/humidifier/toggle_state", HTTP_POST, [](AsyncWebServerRequest *request){
    bool newState = !state.humidifierState;
    updateRelayState(HUMIDIFIER_PIN, state.humidifierState, newState);
    state.humidifierManualOverrideUntil = millis() + 30000;
    request->send(200, "application/json", "{\"success\":true,\"state\":" + String(newState ? "true" : "false") + "}");
  });

  server.on("/api/turner/toggle_state", HTTP_POST, [](AsyncWebServerRequest *request){
    bool newState = !state.eggTurnerState;
    updateRelayState(EGG_TURNER_PIN, state.eggTurnerState, newState);
    state.turnerManualOverrideUntil = millis() + 30000;
    request->send(200, "application/json", "{\"success\":true,\"state\":" + String(newState ? "true" : "false") + "}");
  });

  // Auto mode on/off endpoints
  server.on("/api/bulb/auto_on", HTTP_POST, [](AsyncWebServerRequest *request){
    if (onlineMode) { request->send(423, "application/json", onlineLocked()); return; }
    state.bulbAutoMode = true;
    request->send(200, "application/json", "{\"success\":true,\"autoMode\":true}");
  });

  server.on("/api/bulb/auto_off", HTTP_POST, [](AsyncWebServerRequest *request){
    if (onlineMode) { request->send(423, "application/json", onlineLocked()); return; }
    state.bulbAutoMode = false;
    request->send(200, "application/json", "{\"success\":true,\"autoMode\":false}");
  });

  server.on("/api/humidifier/auto_on", HTTP_POST, [](AsyncWebServerRequest *request){
    if (onlineMode) { request->send(423, "application/json", onlineLocked()); return; }
    state.humidifierAutoMode = true;
    request->send(200, "application/json", "{\"success\":true,\"autoMode\":true}");
  });

  server.on("/api/humidifier/auto_off", HTTP_POST, [](AsyncWebServerRequest *request){
    if (onlineMode) { request->send(423, "application/json", onlineLocked()); return; }
    state.humidifierAutoMode = false;
    request->send(200, "application/json", "{\"success\":true,\"autoMode\":false}");
  });

  // Schedule on/off endpoints
  server.on("/api/fan/schedule_on", HTTP_POST, [](AsyncWebServerRequest *request){
    if (onlineMode) { request->send(423, "application/json", onlineLocked()); return; }
    state.fanScheduleEnabled = true;
    state.fanLastToggle = millis();
    state.fanScheduleRunning = false;
    request->send(200, "application/json", "{\"success\":true,\"scheduleEnabled\":true}");
  });

  server.on("/api/fan/schedule_off", HTTP_POST, [](AsyncWebServerRequest *request){
    if (onlineMode) { request->send(423, "application/json", onlineLocked()); return; }
    state.fanScheduleEnabled = false;
    request->send(200, "application/json", "{\"success\":true,\"scheduleEnabled\":false}");
  });

  server.on("/api/turner/schedule_on", HTTP_POST, [](AsyncWebServerRequest *request){
    if (onlineMode) { request->send(423, "application/json", onlineLocked()); return; }
    state.eggTurnerScheduleEnabled = true;
    state.eggTurnerLastToggle = millis();
    state.eggTurnerScheduleRunning = false;
    request->send(200, "application/json", "{\"success\":true,\"scheduleEnabled\":true}");
  });

  server.on("/api/turner/schedule_off", HTTP_POST, [](AsyncWebServerRequest *request){
    if (onlineMode) { request->send(423, "application/json", onlineLocked()); return; }
    state.eggTurnerScheduleEnabled = false;
    request->send(200, "application/json", "{\"success\":true,\"scheduleEnabled\":false}");
  });

  // Buzzer enable/disable endpoints
  server.on("/api/buzzer/enable", HTTP_POST, [](AsyncWebServerRequest *request){
    state.buzzerEnabled = true;
    saveSettings();
    request->send(200, "application/json", "{\"success\":true,\"enabled\":true}");
  });

  server.on("/api/buzzer/disable", HTTP_POST, [](AsyncWebServerRequest *request){
    state.buzzerEnabled = false;
    saveSettings();
    request->send(200, "application/json", "{\"success\":true,\"enabled\":false}");
  });

  // Test mode endpoints
  server.on("/api/testmode/on", HTTP_POST, [](AsyncWebServerRequest *request){
    testMode = true;
    request->send(200, "application/json", "{\"success\":true,\"testMode\":true}");
  });

  server.on("/api/testmode/off", HTTP_POST, [](AsyncWebServerRequest *request){
    testMode = false;
    request->send(200, "application/json", "{\"success\":true,\"testMode\":false}");
  });

  // Firebase status endpoint
  server.on("/api/firebase/status", HTTP_GET, [](AsyncWebServerRequest *request){
    String json = "{";
    json += "\"connected\":" + String(firebaseConnected ? "true" : "false") + ",";
    json += "\"ready\":" + String(Firebase.ready() ? "true" : "false") + ",";
    json += "\"initialized\":" + String(firebaseReady ? "true" : "false") + ",";
    json += "\"status\":\"" + firebaseStatus + "\",";
    json += "\"error\":\"" + firebaseLastError + "\"";
    json += "}";
    request->send(200, "application/json", json);
  });

  // Mode control endpoint
  server.on("/api/mode", HTTP_POST, [](AsyncWebServerRequest *request){
    if (request->hasParam("mode", true)) {
      String mode = request->getParam("mode", true)->value();
      onlineMode = (mode == "online");
      saveSettings();
      if (onlineMode && staSSID.length() > 0) {
        connectToWiFi();
      }
    }
    request->send(200, "application/json", "{\"success\":true,\"mode\":\"" + String(onlineMode ? "online" : "offline") + "\"}");
  });

  // WiFi configuration endpoint
  server.on("/api/wifi", HTTP_POST, [](AsyncWebServerRequest *request){
    if (request->hasParam("ssid", true)) {
      staSSID = request->getParam("ssid", true)->value();
    }
    if (request->hasParam("password", true)) {
      staPassword = request->getParam("password", true)->value();
    }
    saveSettings();
    request->send(200, "application/json", "{\"success\":true}");
  });

  // WiFi connect endpoint
  server.on("/api/wifi/connect", HTTP_POST, [](AsyncWebServerRequest *request){
    bool connected = connectToWiFi();
    request->send(200, "application/json", "{\"success\":true,\"connected\":" + String(connected ? "true" : "false") + "}");
  });

  // Firebase configuration endpoint
  server.on("/api/firebase", HTTP_POST, [](AsyncWebServerRequest *request){
    if (request->hasParam("apiKey", true)) {
      FIREBASE_API_KEY = request->getParam("apiKey", true)->value();
    }
    if (request->hasParam("dbUrl", true)) {
      FIREBASE_DB_URL = request->getParam("dbUrl", true)->value();
    }
    if (request->hasParam("deviceId", true)) {
      DEVICE_ID = request->getParam("deviceId", true)->value();
    }
    saveSettings();
    // Reinitialize Firebase with new settings
    if (wifiConnected) {
      initFirebase();
    }
    request->send(200, "application/json", "{\"success\":true}");
  });

  // WiFi status endpoint
  server.on("/api/wifi/status", HTTP_GET, [](AsyncWebServerRequest *request){
    String json = "{";
    json += "\"connected\":" + String(wifiConnected ? "true" : "false") + ",";
    json += "\"onlineMode\":" + String(onlineMode ? "true" : "false") + ",";
    json += "\"ssid\":\"" + staSSID + "\",";
    if (wifiConnected) {
      json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
      json += "\"rssi\":" + String(WiFi.RSSI());
    } else {
      json += "\"ip\":\"\",";
      json += "\"rssi\":0";
    }
    json += "}";
    request->send(200, "application/json", json);
  });

  server.on("/restart", HTTP_POST, [](AsyncWebServerRequest *request){
    request->send(200, "text/html", 
      "<!DOCTYPE html><html><head><title>Restarting...</title>"
      "<meta http-equiv='refresh' content='15;url=/'>"
      "<style>body{font-family:-apple-system,sans-serif;text-align:center;padding:50px;background:#f5f5f5;color:#333;}</style>"
      "</head><body><h2>Restarting...</h2>"
      "<p>Please wait 15 seconds, then you'll be redirected automatically.</p>"
      "<p>Or <a href='/'>click here</a> to return manually.</p></body></html>");
    delay(1000);
    ESP.restart();
  });
  
  server.onNotFound([](AsyncWebServerRequest *request){
    request->redirect("/");
  });
}

void setup() {
  // Initialize Serial Monitor
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("=================================");
  Serial.println("   ESP32 Egg Incubator System   ");
  Serial.println("=================================");
  Serial.println();
  
  // Initialize pins
  pinMode(HUMIDIFIER_PIN, OUTPUT);
  pinMode(FAN_PIN, OUTPUT);
  pinMode(BULB_PIN, OUTPUT);
  pinMode(EGG_TURNER_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(WATER_SENSOR_PIN, INPUT_PULLUP);  // GPIO 4 supports pull-up to prevent floating
  
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
  
  // Load settings from flash
  loadSettings();
  
  // Start AP mode
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  dns.start(DNS_PORT, "*", WiFi.softAPIP());
  
  // Try to connect to WiFi if online mode enabled
  if (onlineMode && staSSID.length() > 0) {
    lcd.setCursor(0, 1);
    lcd.print("Connecting WiFi..");
    connectToWiFi();
    
    // Initialize Firebase if WiFi connected
    if (wifiConnected) {
      lcd.setCursor(0, 1);
      lcd.print("Init Firebase...");
      initFirebase();
    }
  }
  
  // Setup web server
  setupWebServer();
  server.begin();
  
  // System startup complete - 2 quick beeps
  delay(500);
  buzzerBeep(200, 2);
  
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(onlineMode ? "Mode: ONLINE" : "Mode: OFFLINE");
  lcd.setCursor(0, 1);
  if (wifiConnected) {
    lcd.print(WiFi.localIP().toString());
  } else {
    lcd.print("AP:192.168.4.1");
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
  
  // Handle Firebase sync (Online mode)
  handleFirebaseSync();
  
  // Check WiFi connection status
  if (onlineMode) {
    wifiConnected = (WiFi.status() == WL_CONNECTED);
  }
  
  yield(); 
  delay(100);
}
