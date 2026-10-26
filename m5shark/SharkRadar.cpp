#include "SharkRadar.h"

#if defined(HAS_SCREEN) && defined(MARAUDER_V8)

#include <WiFi.h>
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
  menu_function_obj.setSharkHudActivity(SHARK_HUD_WIFI_ACTIVITY, false);
}

// Shark Sense: polar map with estimated relative distance (meters).
// Uses a simple log-distance path-loss model (n=2.7, A=-40 dBm @ 1 m).
// Angle is still a stable per-BSSID hash (not true bearing). Rings are labeled
// with approximate range. Touch to exit.
void SharkRadar::runSharkSense() {
  wifi_scan_obj.StartScan(WIFI_SCAN_OFF);
  TFT_eSPI& tft = display_obj.tft;
  const int16_t w = tft.width();

  // Path-loss constants (indoor/outdoor blend). Tune later with real data.
  const float PL_A = -40.0f;   // RSSI at 1 m
  const float PL_N = 2.7f;     // path-loss exponent
  const float MAX_M = 30.0f;   // outer ring = 30 m

  tft.fillScreen(TFT_BLACK);
  menu_function_obj.setSharkHudActivity(SHARK_HUD_WIFI_ACTIVITY, true);
  header("// SHARK SENSE");
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

    if (millis() - last_scan > 2500 || last_scan == 0) {
      tft.setTextDatum(TL_DATUM);
      tft.fillRect(4, 250, w - 8, 12, TFT_BLACK);
      tft.setTextColor(WD_AMBER, TFT_BLACK);
      tft.drawString("sensing...", 8, 252, 1);
      found = WiFi.scanNetworks(false, false);
      last_scan = millis();
    }

    // Clear radar disc.
    tft.fillRect(RCX - RR - 2, RCY - RR - 2, 2 * RR + 4, 2 * RR + 4, TFT_BLACK);
    frame(RCX, RCY, RR, sweep, "SENSE");

    // Labeled range rings (approx).
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(WD_DIM, TFT_BLACK);
    tft.drawString("2m",  RCX + 18, RCY - RR / 3 - 6, 1);
    tft.drawString("8m",  RCX + 28, RCY - 2 * RR / 3 - 6, 1);
    tft.drawString("20m", RCX + 36, RCY - RR + 8, 1);
    tft.setTextDatum(TL_DATUM);

    int strongest = -128;
    String strongest_name = "";
    float strongest_m = 0;
    for (int i = 0; i < found; i++) {
      int rssi = WiFi.RSSI(i);
      uint8_t* bssid = WiFi.BSSID(i);
      uint16_t h = bssid ? keyHash(bssid, 6) : (uint16_t)(i * 2654);
      float ang = (h / 65535.0f) * 2.0f * PI;

      // meters = 10^((A - rssi) / (10 * n))
      float meters = powf(10.0f, (PL_A - (float)rssi) / (10.0f * PL_N));
      if (meters < 0.5f) meters = 0.5f;
      if (meters > MAX_M) meters = MAX_M;
      float norm = meters / MAX_M;
      int16_t br = (int16_t)(norm * RR);
      int16_t bx = RCX + (int16_t)(cosf(ang) * br);
      int16_t by = RCY + (int16_t)(sinf(ang) * br);
      uint16_t col = meters < 4.0f ? WD_CYAN : meters < 12.0f ? WD_BONE : WD_DIM;
      tft.fillCircle(bx, by, 3, col);
      if (rssi > strongest) {
        strongest = rssi;
        strongest_name = WiFi.SSID(i);
        strongest_m = meters;
      }
    }

    // Readout.
    tft.setTextDatum(TL_DATUM);
    tft.fillRect(4, 250, w - 8, 30, TFT_BLACK);
    tft.setTextColor(WD_CYAN, TFT_BLACK);
    tft.drawString(String("APs: ") + found, 8, 252, 2);
    if (strongest_name.length()) {
      tft.setTextColor(WD_BONE, TFT_BLACK);
      String s = String((int)strongest_m) + "m " + strongest_name;
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
  menu_function_obj.setSharkHudActivity(SHARK_HUD_WIFI_ACTIVITY, false);
}

namespace {
  // Shared intro chrome: progress bar + a cycling status caption.
  void introChrome(TFT_eSPI& tft, int16_t w, float prog, const char* const* status) {
    tft.drawRect(20, 272, w - 40, 10, WD_EDGE);
    tft.fillRect(22, 274, (int16_t)((w - 44) * prog), 6, WD_CYAN);
    tft.fillRect(0, 250, w, 16, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(WD_BONE, TFT_BLACK);
    tft.drawString(status[prog < 0.4f ? 0 : prog < 0.85f ? 1 : 2], w / 2, 256, 2);
    tft.setTextDatum(TL_DATUM);
  }
}
