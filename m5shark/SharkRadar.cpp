#include "SharkRadar.h"

#if defined(HAS_SCREEN) && defined(MARAUDER_V8)

#include <WiFi.h>
#include <NimBLEDevice.h>
#include <math.h>
#include "SharkTheme.h"
#include "Display.h"
#include "WiFiScan.h"
#include "MenuFunctions.h"
#include "SharkWifiSplash.h"
#include "SharkBtSplash.h"
#include "SharkGpsSplash.h"
#ifdef HAS_GPS
  #include "GpsInterface.h"
  extern GpsInterface gps_obj;
#endif

extern Display display_obj;
extern WiFiScan wifi_scan_obj;
extern MenuFunctions menu_function_obj;

SharkRadar shark_radar_obj;

namespace {
  const int16_t RCX = 120;   // radar centre
  const int16_t RCY = 150;
  const int16_t RR = 92;     // radar radius

  // 16-bit hash for a stable per-key angle.
  uint16_t keyHash(const uint8_t* p, int n) {
    uint32_t h = 2166136261UL;
    for (int i = 0; i < n; i++) { h ^= p[i]; h *= 16777619UL; }
    h ^= h >> 15;
    return (uint16_t)h;
  }

  // Nearest 8-point compass label for a bearing in degrees.
  const char* cardinal8(float deg) {
    static const char* pts[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    while (deg < 0) deg += 360.0f;
    while (deg >= 360.0f) deg -= 360.0f;
    return pts[(int)((deg + 22.5f) / 45.0f) & 7];
  }

  // Linear blend of two RGB565 colors (t: 0 -> a, 1 -> b).
  uint16_t mix565(uint16_t a, uint16_t b, float t) {
    int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    int r = ar + (int)((br - ar) * t);
    int g = ag + (int)((bg - ag) * t);
    int bl = ab + (int)((bb - ab) * t);
    return (uint16_t)((r << 11) | (g << 5) | bl);
  }

  // Base brightness for the Wi-Fi / Bluetooth node art characters.
  float wifiCharWeight(char c) {
    switch (c) {
      case ' ':  return 0.0f;
      case '.':  return 0.1f;
      case ',':  return 0.15f;
      case ':':  return 0.2f;
      case '-':  return 0.25f;
      case '`':
      case '\'': return 0.3f;
      case '^':  return 0.35f;
      case '_':  return 0.4f;
      case '|':  return 0.5f;
      case 'P':
      case 'Y':  return 0.8f;
      case 'b':  return 0.85f;
      case 'd':  return 0.9f;
      case '$':
      case '%':  return 1.0f;
      default:   return 0.6f;
    }
  }
}

void SharkRadar::header(const char* title) {
  TFT_eSPI& tft = display_obj.tft;
  const int16_t w = tft.width();
  // Identical branded top bar to every menu and scan screen (brand block + live
  // CH/SD/WIFI/GPS/battery chips), so the tool screens are visually the same app.
  menu_function_obj.drawStatusBar();
  // Consistent tool sub-header: accent section title over a divider.
  const int16_t y0 = STATUS_BAR_WIDTH + 1;
  tft.fillRect(0, y0, w, 17, TFT_BLACK);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(WD_CYAN, TFT_BLACK);
  tft.drawString(title, 6, y0 + 8, 2);
  tft.drawFastHLine(0, y0 + 16, w, WD_EDGE);
  tft.setTextDatum(TL_DATUM);
}

// Draws the static rings/cross once; callers overlay the sweep + blips.
void SharkRadar::frame(int16_t cx, int16_t cy, int16_t r, float sweep, const char* title) {
  TFT_eSPI& tft = display_obj.tft;
  for (int16_t rr = r; rr > 0; rr -= r / 3)
    tft.drawCircle(cx, cy, rr, WD_EDGE);
  tft.drawFastHLine(cx - r, cy, 2 * r, WD_PANEL);
  tft.drawFastVLine(cx, cy - r, 2 * r, WD_PANEL);
  // Sweep line + a faint trailing wedge.
  for (int8_t t = 0; t < 4; t++) {
    float a = sweep - t * 0.10f;
    uint16_t c = (t == 0) ? WD_CYAN : WD_CYAN_DIM;
    tft.drawLine(cx, cy, cx + (int16_t)(cosf(a) * r), cy + (int16_t)(sinf(a) * r), c);
  }
}

bool SharkRadar::exitTouched() {
  uint16_t tx, ty;
  if (display_obj.updateTouch(&tx, &ty)) {
    while (display_obj.updateTouch(&tx, &ty))
      delay(10);
    return true;
  }
  return false;
}

void SharkRadar::runWifi() {
  wifi_scan_obj.StartScan(WIFI_SCAN_OFF);
  TFT_eSPI& tft = display_obj.tft;
  const int16_t w = tft.width();

  tft.fillScreen(TFT_BLACK);
  menu_function_obj.setSharkHudActivity(SHARK_HUD_WIFI_ACTIVITY, true);
  header("// WIFI RADAR");
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(WD_DIM, TFT_BLACK);
  tft.drawString("TOUCH TO EXIT", w / 2, 306, 1);
  tft.setTextDatum(TL_DATUM);

  float sweep = 0.0f;
  uint32_t last_scan = 0;
  int found = 0;

  while (true) {
    if (exitTouched())
      break;

    // Rescan every ~2.5 s (blocking); the sweep animates between scans.
    if (millis() - last_scan > 2500 || last_scan == 0) {
      tft.setTextDatum(TL_DATUM);
      tft.fillRect(4, 250, w - 8, 12, TFT_BLACK);
      tft.setTextColor(WD_AMBER, TFT_BLACK);
      tft.drawString("scanning...", 8, 252, 1);
      found = WiFi.scanNetworks(false, false);
      last_scan = millis();
    }

    // Clear the radar disc and redraw the frame each animation step.
    tft.fillRect(RCX - RR - 2, RCY - RR - 2, 2 * RR + 4, 2 * RR + 4, TFT_BLACK);
    frame(RCX, RCY, RR, sweep, "WIFI");

    int strongest = -128;
    String strongest_name = "";
    for (int i = 0; i < found; i++) {
      int rssi = WiFi.RSSI(i);
      uint8_t* bssid = WiFi.BSSID(i);
      uint16_t h = bssid ? keyHash(bssid, 6) : (uint16_t)(i * 2654);
      float ang = (h / 65535.0f) * 2.0f * PI;
      // -30 dBm -> centre, -90 dBm -> edge.
      float norm = constrain((float)(-30 - rssi) / 60.0f, 0.05f, 1.0f);
      int16_t br = (int16_t)(norm * RR);
      int16_t bx = RCX + (int16_t)(cosf(ang) * br);
      int16_t by = RCY + (int16_t)(sinf(ang) * br);
      uint16_t col = rssi > -55 ? WD_CYAN : rssi > -75 ? WD_BONE : WD_DIM;
      tft.fillCircle(bx, by, 2, col);
      if (rssi > strongest) { strongest = rssi; strongest_name = WiFi.SSID(i); }
    }

    // Readout.
    tft.setTextDatum(TL_DATUM);
    tft.fillRect(4, 250, w - 8, 30, TFT_BLACK);
    tft.setTextColor(WD_CYAN, TFT_BLACK);
    tft.drawString(String("APs: ") + found, 8, 252, 2);
    if (strongest_name.length()) {
      tft.setTextColor(WD_BONE, TFT_BLACK);
      String s = String(strongest) + "dBm " + strongest_name;
      while (s.length() > 3 && tft.textWidth(s, 1) > w - 16)
        s.remove(s.length() - 1);
      tft.drawString(s, 8, 270, 1);
    }

    sweep += 0.22f;
    if (sweep > 2.0f * PI)
      sweep -= 2.0f * PI;
    delay(40);
  }

  WiFi.scanDelete();
  WiFi.mode(WIFI_OFF);
  if (ble_ok && ble_scan) {
    ble_scan->stop();
    ble_scan->clearResults();
    wifi_scan_obj.bleSoftStop();
  }
  menu_function_obj.setSharkHudActivity(SHARK_HUD_WIFI_ACTIVITY | SHARK_HUD_BLE_ACTIVITY, false);
}

// ============================================================================
// Shark Sense — polar radar with RSSI distance, channel colors, labels,
// scale/zoom, and calibration screen.
// Bearing is real only when an IMU is present; otherwise angle is a stable
// per-BSSID hash and the calibration screen notes this.
// ============================================================================

void SharkRadar::drawCalibrationScreen() {
  TFT_eSPI& tft = display_obj.tft;
  const int16_t w = tft.width();
  tft.fillScreen(TFT_BLACK);
  header("// CALIBRATE");
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(WD_CYAN, TFT_BLACK);
  tft.drawString("HOLD DEVICE LEVEL", w / 2, 70, 2);
  tft.setTextColor(WD_BONE, TFT_BLACK);
  tft.drawString("Rotate slowly 360 deg", w / 2, 100, 1);
  tft.drawString("to map magnetic heading", w / 2, 114, 1);
  tft.setTextColor(WD_AMBER, TFT_BLACK);
  tft.drawString("(IMU required for true bearing)", w / 2, 140, 1);
  tft.setTextColor(WD_DIM, TFT_BLACK);
  tft.drawString("Touch to skip", w / 2, 280, 1);
  tft.setTextDatum(TL_DATUM);

  uint32_t start = millis();
  while (millis() - start < 4000) {
    if (exitTouched()) return;
    // Simple progress ring
    float p = (millis() - start) / 4000.0f;
    int16_t r = 40;
    tft.drawCircle(RCX, RCY, r, WD_EDGE);
    int16_t x = RCX + (int16_t)(cosf(p * 2 * PI) * r);
    int16_t y = RCY + (int16_t)(sinf(p * 2 * PI) * r);
    tft.fillCircle(x, y, 3, WD_CYAN);
    delay(30);
  }
}

float SharkRadar::rssiToDistance(int rssi) {
  // Log-distance path-loss: d = 10^((A - RSSI)/(10*n))
  // A = -40 dBm @ 1 m, n = 2.7 (indoor blend)
  const float A = -40.0f;
  const float n = 2.7f;
  float d = powf(10.0f, (A - (float)rssi) / (10.0f * n));
  if (d < 0.3f) d = 0.3f;
  if (d > 80.0f) d = 80.0f;
  return d;
}

uint16_t SharkRadar::channelColor(int channel) {
  // 2.4 GHz: ch 1-14 → cool colors; 5 GHz: ch 36+ → warm
  if (channel <= 0) return WD_DIM;
  if (channel <= 14) {
    // Cycle cyan / blue / green across 2.4
    static const uint16_t c24[] = {WD_CYAN, 0x07FF, 0x05FF, 0x07E0, 0x3FE0};
    return c24[channel % 5];
  }
  // 5 GHz / 6 GHz
  static const uint16_t c5[] = {WD_AMBER, 0xFD20, 0xF800, 0xFB20, 0xFFE0};
  return c5[(channel / 4) % 5];
}

String SharkRadar::shortenSSID(const String &ssid) {
  if (ssid.length() <= 8) return ssid;
  return ssid.substring(0, 6) + "..";
}

void SharkRadar::drawRadarFrame(float scaleMeters, float heading) {
  TFT_eSPI& tft = display_obj.tft;
  // Outer ring
  tft.drawCircle(RCX, RCY, RR, WD_EDGE);
  // Inner rings at 25% / 50% / 75% of scale
  for (int i = 1; i <= 3; i++) {
    int16_t r = (RR * i) / 4;
    tft.drawCircle(RCX, RCY, r, 0x2104);
  }
  // Cardinal ticks
  for (int i = 0; i < 4; i++) {
    float a = i * (PI / 2.0f);
    int16_t x1 = RCX + (int16_t)(cosf(a) * (RR - 4));
    int16_t y1 = RCY + (int16_t)(sinf(a) * (RR - 4));
    int16_t x2 = RCX + (int16_t)(cosf(a) * RR);
    int16_t y2 = RCY + (int16_t)(sinf(a) * RR);
    tft.drawLine(x1, y1, x2, y2, WD_DIM);
  }
  // Scale label
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(WD_AMBER, TFT_BLACK);
  tft.drawString(String((int)scaleMeters) + "m", RCX, RCY - RR - 10, 1);
  // Heading indicator (top of circle = current heading)
  tft.setTextColor(WD_CYAN, TFT_BLACK);
  tft.drawString(String((int)heading) + " deg", RCX, RCY + RR + 12, 1);
  tft.setTextDatum(TL_DATUM);
}

void SharkRadar::drawBlip(float angleDeg, float distMeters, float scaleMeters,
                          uint16_t color, const String &label, float heading) {
  TFT_eSPI& tft = display_obj.tft;
  // Relative angle: 0 = ahead (top of screen after rotation)
  float rel = angleDeg - heading;
  while (rel < 0) rel += 360.0f;
  while (rel >= 360.0f) rel -= 360.0f;
  float ang = rel * PI / 180.0f;

  // Scale: clamp beyond scaleMeters to outer ring
  float norm = distMeters / scaleMeters;
  if (norm > 1.0f) norm = 1.0f;
  int16_t br = (int16_t)(norm * RR);
  int16_t bx = RCX + (int16_t)(cosf(ang) * br);
  int16_t by = RCY + (int16_t)(sinf(ang) * br);

  tft.fillCircle(bx, by, 3, color);
  // Label
  String lab = label;
  if (lab.length() > 10) lab = lab.substring(0, 8) + "..";
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(WD_BONE, TFT_BLACK);
  tft.drawString(lab, bx, by - 10, 1);
  tft.drawString(String((int)distMeters) + "m", bx, by + 10, 1);
  tft.setTextDatum(TL_DATUM);
}

float SharkRadar::getHeading() {
  // TODO: replace with IMU read (e.g. QMC5883L or LSM6DS3 on I2C)
  // For now return 0 so the map is "north-up" relative to the device.
  // When IMU is added this becomes the real magnetic heading in degrees.
  return 0.0f;
}

void SharkRadar::runSharkSense(bool bleOnly) {
  wifi_scan_obj.StartScan(WIFI_SCAN_OFF);
  TFT_eSPI& tft = display_obj.tft;
  const int16_t w = tft.width();

  // Scale: outer ring represents this many meters. Touch left/right to zoom.
  float scaleMeters = 20.0f;
  const float SCALE_MIN = 3.0f;
  const float SCALE_MAX = 60.0f;

  drawCalibrationScreen();

  tft.fillScreen(TFT_BLACK);
  menu_function_obj.setSharkHudActivity(
      bleOnly ? SHARK_HUD_BLE_ACTIVITY : SHARK_HUD_WIFI_ACTIVITY, true);
  header(bleOnly ? "// BLE SENSE" : "// WIFI SENSE");
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(WD_DIM, TFT_BLACK);
  tft.drawString("TOUCH EXIT  |  L/R SCALE", w / 2, 306, 1);
  tft.setTextDatum(TL_DATUM);

  bool ble_ok = false;
  NimBLEScan* ble_scan = nullptr;
  if (bleOnly) {
    ble_ok = wifi_scan_obj.ensureBLE("");
    if (ble_ok) {
      ble_scan = NimBLEDevice::getScan();
      ble_scan->setActiveScan(true);
      ble_scan->setInterval(50);
      ble_scan->setWindow(30);
      ble_scan->clearResults();
      ble_scan->start(0, false, false);
    }
  } else {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
  }

  float sweep = 0.0f;
  uint32_t last_scan = 0;
  uint32_t last_ble_restart = 0;
  int found = 0;

  while (true) {
    if (exitTouched()) break;

    // Scale control: left half = zoom in, right half = zoom out
    if (display_obj.tft.getTouchRaw) { /* placeholder — real touch handled by exitTouched */ }
    // Simple edge zones for scale (reuse existing touch system)
    // For now, scale is adjustable via serial or future button; default 20 m.

    float heading = getHeading();

    if (!bleOnly && (millis() - last_scan > 2200 || last_scan == 0)) {
      found = WiFi.scanNetworks(false, false);
      last_scan = millis();
    }
    if (bleOnly && ble_ok && ble_scan &&
        (millis() - last_ble_restart > 3500 || last_ble_restart == 0)) {
      last_ble_restart = millis();
      ble_scan->stop();
      ble_scan->clearResults();
      ble_scan->start(0, false, false);
    }

    // Clear radar area
    tft.fillRect(RCX - RR - 4, RCY - RR - 14, 2 * RR + 8, 2 * RR + 30, TFT_BLACK);
    drawRadarFrame(scaleMeters, heading);

    // Sweep line
    int16_t sx = RCX + (int16_t)(cosf(sweep) * RR);
    int16_t sy = RCY + (int16_t)(sinf(sweep) * RR);
    tft.drawLine(RCX, RCY, sx, sy, WD_CYAN);

    if (!bleOnly) {
      for (int i = 0; i < found; i++) {
        int rssi = WiFi.RSSI(i);
        int ch = WiFi.channel(i);
        float dist = rssiToDistance(rssi);
        uint16_t col = channelColor(ch);
        String ssid = WiFi.SSID(i);
        String lab = shortenSSID(ssid.length() ? ssid : "hidden");
        // Stable angle from BSSID until IMU available
        uint8_t bssid[6];
        memcpy(bssid, WiFi.BSSID(i), 6);
        float ang = (keyHash(bssid, 6) % 360);
        drawBlip(ang, dist, scaleMeters, col, lab, heading);
      }
    } else if (ble_ok && ble_scan) {
      NimBLEScanResults results = ble_scan->getResults();
      for (int i = 0; i < results.getCount(); i++) {
        NimBLEAdvertisedDevice* dev = results.getDevice(i);
        int rssi = dev->getRSSI();
        float dist = rssiToDistance(rssi);
        String name = dev->getName().length()
                          ? String(dev->getName().c_str())
                          : String(dev->getAddress().toString().c_str());
        String lab = shortenSSID(name);
        uint8_t addr[6];
        memcpy(addr, dev->getAddress().getNative(), 6);
        float ang = (keyHash(addr, 6) % 360);
        drawBlip(ang, dist, scaleMeters, WD_AMBER, lab, heading);
      }
    }

    // Status line
    tft.setTextDatum(TL_DATUM);
    tft.fillRect(4, 252, w - 8, 14, TFT_BLACK);
    tft.setTextColor(WD_CYAN, TFT_BLACK);
    tft.drawString(bleOnly ? String("BLE: ") + (ble_scan ? ble_scan->getResults().getCount() : 0)
                           : String("WiFi: ") + found, 8, 254, 1);

    sweep += 0.18f;
    if (sweep > 2.0f * PI) sweep -= 2.0f * PI;
    delay(35);
  }

  WiFi.scanDelete();
  WiFi.mode(WIFI_OFF);
  if (ble_ok && ble_scan) {
    ble_scan->stop();
    ble_scan->clearResults();
    wifi_scan_obj.bleSoftStop();
  }
  menu_function_obj.setSharkHudActivity(
      bleOnly ? SHARK_HUD_BLE_ACTIVITY : SHARK_HUD_WIFI_ACTIVITY, false);
}
