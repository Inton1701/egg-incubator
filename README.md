# ESP32 Egg Incubator

This project is an ESP32-based egg incubator controller with web interface, automatic temperature/humidity regulation, scheduling, SMS alerts, and LCD display.

## Features

- WiFi Access Point with web dashboard
- Real-time sensor readings (temperature, humidity, water level)
- Relay control for fan, bulb, humidifier, egg turner
- Manual and scheduled operation modes
- Automatic heater/humidifier control by thresholds
- SMS alerts and status via SIM900A
- LCD (I2C) status display

## Hardware

- ESP32
- 4-channel relay module
- AM2302 (DHT22) sensors x2
- Water level sensor (digital)
- SIM900A GSM module
- I2C LCD 16x2

## Wiring

- LCD SDA: GPIO 21, SCL: GPIO 22
- Relays: Humidifier 26, Fan 25, Bulb 33, Egg Turner 32
- Sensors: DHT22 #1 14, DHT22 #2 27, Water 35
- SIM900A: RX 17, TX 16
- Buzzer: 19

## Usage

1. Flash firmware and SPIFFS using PlatformIO.
2. Connect to WiFi SSID `EggIncubator` (password: `12345678`).
3. Open browser at `http://192.168.4.1` for control and monitoring.
4. Set thresholds, schedules, and phone number via web interface.
5. SMS "status" to receive system report.

## Default Settings

- Temperature: Trigger 37.5°C, Stop 38.0°C
- Humidity: Trigger 60%, Stop 65%
- Fan: 5 min ON / 5 min OFF
- Egg Turner: 10 sec ON / 6 hr OFF

## API Endpoints

- `/api/status` - Get system status (JSON)
- `/api/[device]/[action]` - Control relays, schedules, thresholds

## License

Open source. Modify and use freely.