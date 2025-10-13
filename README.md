# ESP32 Egg Incubator System

A comprehensive egg incubator control system built with ESP32, featuring web-based control, automatic temperature and humidity regulation, scheduling, SMS alerts, and LCD display.

## Features

### 🌐 Web Interface
- **Access Point Mode**: Direct connection without router (SSID: `EggIncubator`, Password: `12345678`)
- **Responsive Design**: Yellow/orange gradient theme with color-coded buttons
- **Real-time Monitoring**: Live sensor readings and system status
- **Settings Storage**: All configurations saved to Flash memory
- **Auto-refresh**: Page updates every 5 seconds (pauses when editing inputs)

### 🔧 Actuator Control

#### 🌀 Fan Control
- **Manual ON/OFF**: Direct web control
- **Schedule Mode**: Configurable run/idle duration (format: seconds)
- **Real-time Timer**: Shows next action countdown
- **Default Schedule**: 5 minutes ON / 5 minutes OFF

#### 🔄 Egg Turner Control  
- **Manual ON/OFF**: Direct web control
- **Schedule Mode**: Configurable run/idle duration
- **Real-time Timer**: Shows next action countdown
- **Default Schedule**: 10 seconds ON / 6 hours OFF

#### 💨 Humidifier Control
- **Manual ON/OFF**: Direct web control
- **Auto Mode**: Triggers based on humidity thresholds
- **Configurable Thresholds**: Trigger and stop values (default: 60%-65%)
- **Real-time Status**: Current humidity and threshold display

#### 💡 Bulb (Heater) Control
- **Manual ON/OFF**: Direct web control  
- **Auto Mode**: Triggers based on temperature thresholds
- **Configurable Thresholds**: Trigger and stop values (default: 37.5°C-38.0°C)
- **Real-time Status**: Current temperature and threshold display

#### 🔊 Buzzer Control
- **Enable/Disable**: Toggle buzzer functionality
- **Startup Beeps**: 2 quick beeps after system initialization
- **Water Alert**: Continuous beeping when no water detected
- **Test Function**: Manual buzzer test

### 📊 Sensors & Monitoring

#### 🌡️ Dual AM2302 Temperature/Humidity Sensors
- **Redundancy**: Uses average of both sensors when available
- **Fallback**: Uses single sensor if one fails
- **Error Detection**: Displays error if both sensors fail
- **Real-time Display**: Live readings on web interface and LCD

#### 🚰 Water Level Sensor
- **Digital Switch Type**: HIGH = Water present, LOW = No water
- **Visual Indication**: Web interface and LCD status
- **Alert System**: SMS and buzzer warnings when water low

#### 📱 LCD Display (16x2 I²C)
- **Rotating Display**: Cycles every second between different views:
  1. Temperature and Humidity: `T: 37.50C  H: 62%`
  2. Water and Bulb Status: `W: High  B: ON`
  3. Fan and Egg Turner Status: `F: ON  E: OFF`
- **Error Indication**: Shows "ERROR" for failed sensors

### 📱 SIM900A SMS System

#### 📞 Contact Management
- **Single Contact**: Add/edit one custom phone number via web interface
- **System Online**: Automatic SMS sent when phone number is first configured
- **Signal Monitoring**: Displays signal strength (No Signal/Weak/Medium/Strong)

#### 🚨 Automated Alerts
- **Water Level Alert**: SMS sent 30 seconds after water goes low
- **One-time Alert**: Prevents SMS spam (one alert per water-low event)

#### 💬 SMS Commands
- **Status Request**: Send "status" or "STATUS" to get complete system report
- **Auto-reply**: Includes all sensor readings and device states
- Error detection and display

### 🔧 Actuator Control
- **Fan**: Manual control + schedule mode with run/idle duration
- **Egg Turner**: Manual control + schedule mode with run/idle duration  
- **Humidifier**: Automatic based on humidity thresholds
- **Heating Bulb**: Automatic based on temperature thresholds
- **Buzzer**: Startup beeps + water level alerts

### 💾 Persistent Storage
- **All settings saved** to ESP32 flash memory (Preferences)
- **Automatic restore** of thresholds, schedules, and phone number on restart
- **No data loss** during power cycles
- **Settings include**: Temperature/humidity thresholds, schedule modes, phone number, buzzer settings

### 📱 Connectivity
- **WiFi Access Point**: "EggIncubator" (password: incubator123)
- **Web Interface**: Modern, responsive design with real-time updates
- **SIM900A SMS**: Status requests, water alerts, system online notifications

### 🖥️ Display
- **I2C LCD (16x2)**: Rotating display showing temp/humidity, water/bulb, fan/egg turner
- Icons and status indicators

## Hardware Connections

```
ESP32 Pin Connections:
├── LCD I2C
│   ├── SDA → GPIO 21
│   └── SCL → GPIO 22
├── 4-Channel Relay
│   ├── Humidifier → GPIO 26
│   ├── Fan → GPIO 25
│   ├── Bulb → GPIO 33
│   └── Egg Turner → GPIO 32
├── Sensors
│   ├── AM2302 #1 → GPIO 14
│   ├── AM2302 #2 → GPIO 27
│   └── Water Level → GPIO 35
├── Communication
│   ├── SIM900A RX → GPIO 17
│   ├── SIM900A TX → GPIO 16
│   └── Buzzer → GPIO 19
```

## Installation

1. **Install PlatformIO** in VS Code
2. **Upload filesystem** (SPIFFS):
   ```bash
   pio run --target uploadfs
   ```
3. **Compile and upload** code:
   ```bash
   pio run --target upload
   ```

## Usage

### Web Interface
1. Connect to WiFi: "EggIncubator" (password: incubator123)
2. Open browser: http://192.168.4.1
3. Monitor status and control devices
4. Set temperature/humidity thresholds
5. Configure schedules for fan and egg turner

### SMS Control
1. Set phone number in web interface
2. Send "status" to get system status
3. Receive automatic water level alerts
4. Get system online notification on startup

### LCD Display
The display rotates every second showing:
- **Screen 1**: Temperature and Humidity
- **Screen 2**: Water Level and Bulb Status  
- **Screen 3**: Fan and Egg Turner Status

## Default Settings

- **Temperature**: Trigger 37.5°C, Stop 37.0°C
- **Humidity**: Trigger 60%, Stop 65%
- **Fan Schedule**: 1 min ON, 5 min OFF
- **Egg Turner**: 10 sec ON, 2 hours OFF
- **Buzzer**: Enabled

## Troubleshooting

### Sensor Issues
- Check DHT22 connections
- Verify 3.3V power supply
- System shows "ERROR" if both sensors fail

### WiFi Connection
- Access Point: "EggIncubator" 
- Default IP: 192.168.4.1
- Check antenna connection

### SMS Not Working
- Verify SIM900A power (2A supply recommended)
- Check SIM card insertion
- Ensure network coverage
- Confirm SMS settings in web interface

### Relay Issues
- Relays are **active LOW** (inverted logic)
- Check power supply capacity
- Verify relay module type

## System States

### Manual Mode
- Direct on/off control via web interface
- Overrides automatic control
- Schedules disabled when manual active

### Schedule Mode  
- Automatic timing control
- Independent for fan and egg turner
- Configurable run/idle durations

### Automatic Mode
- Temperature-based bulb control
- Humidity-based humidifier control
- Always active unless hardware failure

## Safety Features

- **Water level monitoring** with alerts
- **Sensor redundancy** with error detection
- **Temperature limits** to prevent overheating
- **SMS alerts** for critical conditions
- **Manual override** for all controls

## File Structure

```
├── src/main.cpp          # Main application code
├── data/                 # Web interface files (SPIFFS)
│   ├── index.html       # Web interface
│   ├── style.css        # Styling
│   └── script.js        # JavaScript functionality
├── platformio.ini       # Project configuration
└── README.md           # Documentation
```

## API Endpoints

- `GET /api/status` - Get system status
- `POST /api/control` - Send control commands
- `GET /` - Web interface
- `GET /style.css` - CSS stylesheet  
- `GET /script.js` - JavaScript code

## License

This project is open source. Feel free to modify and distribute.

## Support

For issues or questions, check connections and power supply first. The system is designed to be simple and reliable for egg incubation applications.