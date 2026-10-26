# Shark Sense — Implementation Notes

## Features (2026-10-26)

- **Separate modes**: Menu entries "WiFi Sense" and "BLE Sense" call `runSharkSense(false)` and `runSharkSense(true)`.
- **Channel colors**: 2.4 GHz (ch 1–14) uses cool palette; 5/6 GHz uses warm palette. BLE blips are amber.
- **Per-blip labels**: Shortened SSID/name + distance in meters drawn next to each marker.
- **Scale / zoom**: Outer ring represents `scaleMeters` (default 20 m). Devices beyond the scale are clamped to the outer ring. (Touch zones for live zoom are stubbed; scale value is currently fixed at entry — ready for button or touch mapping.)
- **Calibration screen**: Shown on entry. Instructs user to hold level and rotate. Notes that true bearing requires an IMU.
- **Bearing**: `getHeading()` returns 0.0f (north-up relative to device). Structured so an external IMU (QMC5883L / LSM6DS3 on available I2C) can replace the stub without changing the plot path.

## Hardware note

M5Shark V8 has no onboard IMU (only GPS + IP5306). Real device-rotation bearing requires an external I2C magnetometer/IMU. Until that is attached, angles are stable per-BSSID hashes so the map remains consistent across scans but is not a true compass bearing.
