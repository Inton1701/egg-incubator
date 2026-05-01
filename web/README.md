# Egg Incubator Remote Monitor

This is a standalone web application for remotely monitoring and controlling your ESP32 Egg Incubator when it's in **Online Mode**.

## Requirements

1. **Firebase Realtime Database** - Create a free Firebase project at https://console.firebase.google.com
2. **ESP32 in Online Mode** - Configure WiFi and Firebase settings on the device

## Firebase Setup

### 1. Create a Firebase Project
1. Go to [Firebase Console](https://console.firebase.google.com)
2. Click "Add project" and follow the setup wizard
3. Once created, go to **Build → Realtime Database**
4. Click "Create Database"
5. Choose a location close to you
6. Start in **Test mode** for development

### 2. Get Your Credentials
- **Database URL**: Found in the Realtime Database section (e.g., `https://your-project-default-rtdb.firebaseio.com`)
- **API Key**: Go to **Project Settings → General → Web API Key**

### 3. Database Rules for Testing
For development, set your rules to allow public access:
```json
{
  "rules": {
    ".read": true,
    ".write": true
  }
}
```

### 4. Secure Your Database (Production)
For production use, update your database rules:
```json
{
  "rules": {
    "devices": {
      "$deviceId": {
        ".read": true,
        ".write": true
      }
    }
  }
}
```

## Using the Remote Monitor

### Option 1: Open Directly
Simply open `index.html` in any modern web browser. It works completely offline and communicates directly with Firebase.

### Option 2: Host Online
Host this file on any web server, GitHub Pages, Netlify, or Vercel for access from anywhere.

### Configuration
1. Enter your Firebase Database URL (e.g., `https://your-project-default-rtdb.firebaseio.com`)
2. Enter your Device ID (default: `incubator1`)
3. Click "Connect to Firebase"

**Note:** Your Firebase database rules must allow public read/write access for the web app to work without authentication.

## Features

- **Real-time monitoring** - Temperature, humidity, and water level updates every 3 seconds
- **Remote control** - Toggle heater, fan, humidifier, and egg turner
- **Auto mode control** - Enable/disable automatic temperature and humidity control
- **Schedule control** - Enable/disable fan and egg turner schedules
- **Turn Now** - Manually trigger egg turning
- **Connection status** - See when the device was last online

## ESP32 Configuration

On the ESP32 device (via local WiFi at 192.168.4.1):
1. Go to the **WiFi** tab
2. Enable **Online Mode**
3. Enter your **WiFi SSID** and **Password**
4. Enter your **Firebase API Key** (from Project Settings → General)
5. Enter your **Firebase Database URL** (from Realtime Database)
6. Set your **Device ID**
7. Click **Connect to WiFi**

## Data Structure

The ESP32 publishes to Firebase with this structure:

```
/devices/{deviceId}/
  ├── state/
  │   ├── tempC: 37.5
  │   ├── humidity: 65
  │   ├── waterHigh: true
  │   ├── waterLow: false
  │   ├── heaterBulb: true
  │   ├── fan: false
  │   ├── humidifier: true
  │   ├── eggTurner: false
  │   ├── bulbAutoMode: true
  │   ├── humidifierAutoMode: true
  │   ├── fanScheduleEnabled: true
  │   ├── eggTurnerScheduleEnabled: true
  │   ├── buzzerEnabled: true
  │   ├── mode: "online"
  │   ├── lastSeen: 1234567890
  │   └── ip: "192.168.1.100"
  │
  ├── command/
  │   ├── cmdId: "1234567890"
  │   ├── heaterBulb: true/false
  │   ├── fan: true/false
  │   ├── humidifier: true/false
  │   ├── eggTurner: true/false
  │   ├── bulbAutoMode: true/false
  │   ├── humidifierAutoMode: true/false
  │   ├── fanScheduleEnabled: true/false
  │   ├── eggTurnerScheduleEnabled: true/false
  │   └── turnNow: true
  │
  └── meta/
      └── lastCmdId: "1234567890"
```

## Troubleshooting

### "Device Offline" Status
- Check ESP32 has internet connectivity
- Verify Firebase credentials are correct
- Make sure Online Mode is enabled

### Commands Not Working
- Verify the Device ID matches on both sides
- Check the database secret has write permissions
- Look at Firebase Database logs for errors

### Connection Failed
- Double-check the Firebase URL (no `https://` prefix)
- Verify your database secret is correct
- Check browser console for detailed errors

## Security Notes

⚠️ **Important**: The database secret provides full access to your database. For production:
- Use Firebase Authentication instead of database secrets
- Implement proper security rules
- Never expose your database secret in public code
