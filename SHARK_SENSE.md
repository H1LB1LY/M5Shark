# Shark Sense

Relative-distance polar radar for the M5Shark v8 (SHARK firmware).

## What it does

- Scans nearby APs (same data as the existing WiFi Radar).
- Estimates relative distance in meters using a log-distance path-loss model.
- Plots each AP on a sweeping polar map:
  - Radius = estimated meters (outer ring ≈ 30 m).
  - Angle = stable hash of the BSSID (not true bearing).
- Labeled range rings (2 m / 8 m / 20 m).
- Strongest-device readout (distance + SSID).
- Touch to exit.

## Status

The method is implemented in `m5shark/SharkRadar.cpp` as `SharkRadar::runSharkSense()`.

The menu entry is **not yet wired** because `MenuFunctions.cpp` is too large for a single API update. Add it manually (one line):

In `m5shark/MenuFunctions.cpp`, right after the existing `"WiFi Radar"` node inside `sharkDefenseMenu`, insert:

```cpp
this->addNodes(&sharkDefenseMenu, "Shark Sense", TFTGREEN, SCANNERS, [this]() {
  shark_radar_obj.runSharkSense();
  this->changeMenu(&sharkDefenseMenu, true);
});
```

Then rebuild with `build-shark-v8.ps1` and flash.

## Path-loss constants (tune later)

```cpp
const float PL_A = -40.0f;  // RSSI at 1 m
const float PL_N = 2.7f;    // path-loss exponent
const float MAX_M = 30.0f;  // outer ring
```

These are starting values. Real indoor/outdoor calibration will improve accuracy.

## Next steps

1. Wire the menu entry (snippet above).
2. Build and flash.
3. Walk-test against known APs and adjust PL_A / PL_N.
4. Optional later: BLE devices, true bearing if heading data is available, QR export of the strongest MAC.
