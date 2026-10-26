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

// WiFi section intro: a live ASCII "liquid morph" signal node (DedSec style)
// that grows expanding arcs over four frames, with Watch Dogs hacking-glyph
// glitches. Same morph engine as the boot marks; auto-advances, touch skips.
void SharkRadar::playWifiIntro() {
  TFT_eSPI& tft = display_obj.tft;
  const int16_t w = tft.width();
  tft.fillScreen(TFT_BLACK);
  header("// WIFI");
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(WD_DIM, TFT_BLACK);
  tft.drawString("TOUCH TO SKIP", w / 2, 306, 1);
  tft.setTextDatum(TL_DATUM);

  static const char* const status[3] = {"NODE LOCATED", "INTERCEPTING PACKETS", "SIGNAL OPTIMAL"};

  // Density maps for the four morph poses.
  static uint8_t wdens[4][WIFI_ROWS][WIFI_COLS];
  for (int f = 0; f < 4; f++)
    for (int y = 0; y < WIFI_ROWS; y++) {
      const char* row = wifi_splash_frames[f][y];
      int len = (int)strlen(row);
      for (int x = 0; x < WIFI_COLS; x++)
        wdens[f][y][x] = (uint8_t)(wifiCharWeight(x < len ? row[x] : ' ') * 255.0f);
    }

  const char ramp[] = " .,:;i1tfLCG08@#$";
  const int rampMax = 16;
  const char glitchc[] = "0101!@#$%^&*()_+{}[]|:;<>?/~A1B2C3D4E5F67890X";
  const int glitchN = (int)sizeof(glitchc) - 1;
  const int seq[6] = {0, 1, 0, 2, 0, 3};

  const int CELL_W = 5, CELL_H = 8;
  const int SW = WIFI_COLS * CELL_W + 1;   // 216
  const int SH = WIFI_ROWS * CELL_H;       // 176
  const int16_t sx = (w - SW) / 2;
  const int16_t sy = 46;

  TFT_eSprite spr = TFT_eSprite(&tft);
  spr.setColorDepth(16);
  bool use_sprite = (spr.createSprite(SW, SH) != nullptr);
  if (use_sprite) { spr.setTextFont(1); spr.setTextWrap(false); }

  uint16_t rowcol[WIFI_ROWS];
  uint32_t rng = 0x1ff1c1a7u ^ millis();
  const float cycleDuration = 0.42f;
  const uint32_t t0 = millis();
  const uint32_t dur = (uint32_t)(6 * cycleDuration * 1000.0f);

  while (millis() - t0 < dur) {
    if (exitTouched()) break;
    float time = (millis() - t0) * 0.001f;
    float prog = (millis() - t0) / (float)dur;
    float totalProgress = time / cycleDuration;
    int curr = ((int)floorf(totalProgress)) % 6;
    int next = (curr + 1) % 6;
    float tt = totalProgress - floorf(totalProgress);
    float smoothT = tt * tt * (3.0f - 2.0f * tt);
    bool glitching = (seq[curr] == 3);

    uint16_t ctop, cbot;
    if (glitching) { ctop = 0xFFE0; cbot = 0xF808; }   // Watch Dogs yellow -> magenta
    else {
      ctop = mix565(WD_CYAN, 0xFFFF, 0.55f);
      cbot = mix565(WD_CYAN, 0x0000, 0.75f);
    }
    for (int y = 0; y < WIFI_ROWS; y++)
      rowcol[y] = mix565(ctop, cbot, (float)y / (WIFI_ROWS - 1));

    if (use_sprite) spr.fillSprite(TFT_BLACK);
    else            tft.fillRect(sx, sy, SW, SH, TFT_BLACK);

    for (int y = 0; y < WIFI_ROWS; y++) {
      float gb = sinf(time * 5.0f - y * 3.0f);
      float glitchEffect = (gb > 0.96f) ? 0.6f : 0.0f;
      for (int x = 0; x < WIFI_COLS; x++) {
        float dA = wdens[seq[curr]][y][x] / 255.0f;
        float dB = wdens[seq[next]][y][x] / 255.0f;
        float base = dA + (dB - dA) * smoothT;
        if (base < 0.05f) continue;

        float wave = sinf(time * 4.0f + x * 0.2f) * 0.1f;
        rng = rng * 1664525u + 1013904223u;
        float flicker = (((rng >> 8) & 0xFFFF) / 65535.0f) * 0.2f - 0.1f;
        float fd = base + wave + glitchEffect + flicker;
        if (fd < 0) fd = 0;
        if (fd > 1) fd = 1;

        char ch;
        rng = rng * 1664525u + 1013904223u;
        float rnd2 = ((rng >> 8) & 0xFFFF) / 65535.0f;
        if (glitching || gb > 0.98f || (smoothT > 0.4f && smoothT < 0.6f && rnd2 > 0.75f)) {
          rng = rng * 1664525u + 1013904223u;
          ch = glitchc[(rng >> 8) % glitchN];
        } else {
          int idx = (int)(fd * rampMax);
          if (idx < 0) idx = 0;
          if (idx > rampMax) idx = rampMax;
          ch = ramp[idx];
        }
        if (ch == ' ') continue;

        uint16_t col = rowcol[y];
        if (use_sprite) {
          spr.setTextColor(col, TFT_BLACK);
          spr.drawChar((uint16_t)ch, x * CELL_W, y * CELL_H, 1);
        } else {
          tft.setTextColor(col, TFT_BLACK);
          tft.drawChar((uint16_t)ch, sx + x * CELL_W, sy + y * CELL_H, 1);
        }
      }
    }
    if (use_sprite) spr.pushSprite(sx, sy);
    introChrome(tft, w, prog, status);
    delay(16);
  }

  if (use_sprite) spr.deleteSprite();
}

// Bluetooth section intro: a live ASCII "liquid morph" Bluetooth glyph (DedSec
// style) that pings and scans over four frames, with Watch Dogs hacking-glyph
// glitches. Same morph engine as the Wi-Fi intro; auto-advances, touch skips.
void SharkRadar::playBluetoothIntro() {
  TFT_eSPI& tft = display_obj.tft;
  const int16_t w = tft.width();
  tft.fillScreen(TFT_BLACK);
  header("// BLUETOOTH");
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(WD_DIM, TFT_BLACK);
  tft.drawString("TOUCH TO SKIP", w / 2, 306, 1);
  tft.setTextDatum(TL_DATUM);

  static const char* const status[3] = {"MAC LOCATED", "INITIATING PAIRING", "BYPASSING PIN"};

  static uint8_t bdens[4][BT_ROWS][BT_COLS];
  for (int f = 0; f < 4; f++)
    for (int y = 0; y < BT_ROWS; y++) {
      const char* row = bt_splash_frames[f][y];
      int len = (int)strlen(row);
      for (int x = 0; x < BT_COLS; x++)
        bdens[f][y][x] = (uint8_t)(wifiCharWeight(x < len ? row[x] : ' ') * 255.0f);
    }

  const char ramp[] = " .,:;i1tfLCG08@#$";
  const int rampMax = 16;
  const char glitchc[] = "0101!@#$%^&*()_+{}[]|:;<>?/~A1B2C3D4E5F67890X";
  const int glitchN = (int)sizeof(glitchc) - 1;
  const int seq[6] = {0, 1, 0, 2, 0, 3};

  const int CELL_W = 5, CELL_H = 8;
  const int SW = BT_COLS * CELL_W + 1;
  const int SH = BT_ROWS * CELL_H;
  const int16_t sx = (w - SW) / 2;
  const int16_t sy = 46;

  TFT_eSprite spr = TFT_eSprite(&tft);
  spr.setColorDepth(16);
  bool use_sprite = (spr.createSprite(SW, SH) != nullptr);
  if (use_sprite) { spr.setTextFont(1); spr.setTextWrap(false); }

  uint16_t rowcol[BT_ROWS];
  uint32_t rng = 0xb1e70075u ^ millis();
  const float cycleDuration = 0.42f;
  const uint32_t t0 = millis();
  const uint32_t dur = (uint32_t)(6 * cycleDuration * 1000.0f);

  while (millis() - t0 < dur) {
    if (exitTouched()) break;
    float time = (millis() - t0) * 0.001f;
    float prog = (millis() - t0) / (float)dur;
    float totalProgress = time / cycleDuration;
    int curr = ((int)floorf(totalProgress)) % 6;
    int next = (curr + 1) % 6;
    float tt = totalProgress - floorf(totalProgress);
    float smoothT = tt * tt * (3.0f - 2.0f * tt);
    bool glitching = (seq[curr] == 3);

    uint16_t ctop, cbot;
    if (glitching) { ctop = 0xFFE0; cbot = 0xF808; }   // yellow -> magenta
    else {
      ctop = mix565(WD_CYAN, 0xFFFF, 0.55f);
      cbot = mix565(WD_CYAN, 0x0000, 0.75f);
    }
    for (int y = 0; y < BT_ROWS; y++)
      rowcol[y] = mix565(ctop, cbot, (float)y / (BT_ROWS - 1));

    if (use_sprite) spr.fillSprite(TFT_BLACK);
    else            tft.fillRect(sx, sy, SW, SH, TFT_BLACK);

    for (int y = 0; y < BT_ROWS; y++) {
      float gb = sinf(time * 5.0f - y * 3.0f);
      float glitchEffect = (gb > 0.96f) ? 0.6f : 0.0f;
      for (int x = 0; x < BT_COLS; x++) {
        float dA = bdens[seq[curr]][y][x] / 255.0f;
        float dB = bdens[seq[next]][y][x] / 255.0f;
        float base = dA + (dB - dA) * smoothT;
        if (base < 0.05f) continue;

        float wave = sinf(time * 4.0f + x * 0.2f) * 0.1f;
        rng = rng * 1664525u + 1013904223u;
        float flicker = (((rng >> 8) & 0xFFFF) / 65535.0f) * 0.2f - 0.1f;
        float fd = base + wave + glitchEffect + flicker;
        if (fd < 0) fd = 0;
        if (fd > 1) fd = 1;

        char ch;
        rng = rng * 1664525u + 1013904223u;
        float rnd2 = ((rng >> 8) & 0xFFFF) / 65535.0f;
        if (glitching || gb > 0.98f || (smoothT > 0.4f && smoothT < 0.6f && rnd2 > 0.75f)) {
          rng = rng * 1664525u + 1013904223u;
          ch = glitchc[(rng >> 8) % glitchN];
        } else {
          int idx = (int)(fd * rampMax);
          if (idx < 0) idx = 0;
          if (idx > rampMax) idx = rampMax;
          ch = ramp[idx];
        }
        if (ch == ' ') continue;

        uint16_t col = rowcol[y];
        if (use_sprite) {
          spr.setTextColor(col, TFT_BLACK);
          spr.drawChar((uint16_t)ch, x * CELL_W, y * CELL_H, 1);
        } else {
          tft.setTextColor(col, TFT_BLACK);
          tft.drawChar((uint16_t)ch, sx + x * CELL_W, sy + y * CELL_H, 1);
        }
      }
    }
    if (use_sprite) spr.pushSprite(sx, sy);
    introChrome(tft, w, prog, status);
    delay(16);
  }

  if (use_sprite) spr.deleteSprite();
}

// GPS section intro: a live ASCII "liquid morph" location pin that acquires
// satellite rings and locks (DedSec style), with Watch Dogs glitches. Same
// morph engine as the Wi-Fi / Bluetooth intros; auto-advances, touch skips.
void SharkRadar::playGpsIntro() {
  TFT_eSPI& tft = display_obj.tft;
  const int16_t w = tft.width();
  tft.fillScreen(TFT_BLACK);
  header("// GPS");
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(WD_DIM, TFT_BLACK);
  tft.drawString("TOUCH TO SKIP", w / 2, 306, 1);
  tft.setTextDatum(TL_DATUM);

  static const char* const status[3] = {"SATELLITE LOCATED", "ACQUIRING FIX", "POSITION LOCKED"};

  static uint8_t gdens[4][GPSI_ROWS][GPSI_COLS];
  for (int f = 0; f < 4; f++)
    for (int y = 0; y < GPSI_ROWS; y++) {
      const char* row = gps_splash_frames[f][y];
      int len = (int)strlen(row);
      for (int x = 0; x < GPSI_COLS; x++)
        gdens[f][y][x] = (uint8_t)(wifiCharWeight(x < len ? row[x] : ' ') * 255.0f);
    }

  const char ramp[] = " .,:;i1tfLCG08@#$";
  const int rampMax = 16;
  const char glitchc[] = "0101!@#$%^&*()_+{}[]|:;<>?/~A1B2C3D4E5F67890X";
  const int glitchN = (int)sizeof(glitchc) - 1;
  const int seq[6] = {0, 1, 0, 2, 0, 3};

  const int CELL_W = 5, CELL_H = 8;
  const int SW = GPSI_COLS * CELL_W + 1;
  const int SH = GPSI_ROWS * CELL_H;
  const int16_t sx = (w - SW) / 2;
  const int16_t sy = 46;

  TFT_eSprite spr = TFT_eSprite(&tft);
  spr.setColorDepth(16);
  bool use_sprite = (spr.createSprite(SW, SH) != nullptr);
  if (use_sprite) { spr.setTextFont(1); spr.setTextWrap(false); }

  uint16_t rowcol[GPSI_ROWS];
  uint32_t rng = 0x6b5104a7u ^ millis();
  const float cycleDuration = 0.42f;
  const uint32_t t0 = millis();
  const uint32_t dur = (uint32_t)(6 * cycleDuration * 1000.0f);

  while (millis() - t0 < dur) {
    if (exitTouched()) break;
    float time = (millis() - t0) * 0.001f;
    float prog = (millis() - t0) / (float)dur;
    float totalProgress = time / cycleDuration;
    int curr = ((int)floorf(totalProgress)) % 6;
    int next = (curr + 1) % 6;
    float tt = totalProgress - floorf(totalProgress);
    float smoothT = tt * tt * (3.0f - 2.0f * tt);
    bool glitching = (seq[curr] == 3);

    uint16_t ctop, cbot;
    if (glitching) { ctop = 0xFFE0; cbot = 0xF808; }   // yellow -> magenta
    else {
      ctop = mix565(WD_CYAN, 0xFFFF, 0.55f);
      cbot = mix565(WD_CYAN, 0x0000, 0.75f);
    }
    for (int y = 0; y < GPSI_ROWS; y++)
      rowcol[y] = mix565(ctop, cbot, (float)y / (GPSI_ROWS - 1));

    if (use_sprite) spr.fillSprite(TFT_BLACK);
    else            tft.fillRect(sx, sy, SW, SH, TFT_BLACK);

    for (int y = 0; y < GPSI_ROWS; y++) {
      float gb = sinf(time * 5.0f - y * 3.0f);
      float glitchEffect = (gb > 0.96f) ? 0.6f : 0.0f;
      for (int x = 0; x < GPSI_COLS; x++) {
        float dA = gdens[seq[curr]][y][x] / 255.0f;
        float dB = gdens[seq[next]][y][x] / 255.0f;
        float base = dA + (dB - dA) * smoothT;
        if (base < 0.05f) continue;

        float wave = sinf(time * 4.0f + x * 0.2f) * 0.1f;
        rng = rng * 1664525u + 1013904223u;
        float flicker = (((rng >> 8) & 0xFFFF) / 65535.0f) * 0.2f - 0.1f;
        float fd = base + wave + glitchEffect + flicker;
        if (fd < 0) fd = 0;
        if (fd > 1) fd = 1;

        char ch;
        rng = rng * 1664525u + 1013904223u;
        float rnd2 = ((rng >> 8) & 0xFFFF) / 65535.0f;
        if (glitching || gb > 0.98f || (smoothT > 0.4f && smoothT < 0.6f && rnd2 > 0.75f)) {
          rng = rng * 1664525u + 1013904223u;
          ch = glitchc[(rng >> 8) % glitchN];
        } else {
          int idx = (int)(fd * rampMax);
          if (idx < 0) idx = 0;
          if (idx > rampMax) idx = rampMax;
          ch = ramp[idx];
        }
        if (ch == ' ') continue;

        uint16_t col = rowcol[y];
        if (use_sprite) {
          spr.setTextColor(col, TFT_BLACK);
          spr.drawChar((uint16_t)ch, x * CELL_W, y * CELL_H, 1);
        } else {
          tft.setTextColor(col, TFT_BLACK);
          tft.drawChar((uint16_t)ch, sx + x * CELL_W, sy + y * CELL_H, 1);
        }
      }
    }
    if (use_sprite) spr.pushSprite(sx, sy);
    introChrome(tft, w, prog, status);
    delay(16);
  }

  if (use_sprite) spr.deleteSprite();
}

#ifdef HAS_GPS

void SharkRadar::runGps() {
  TFT_eSPI& tft = display_obj.tft;
  const int16_t w = tft.width();
  tft.fillScreen(TFT_BLACK);
  menu_function_obj.setSharkHudActivity(SHARK_HUD_GPS_ACTIVITY, true);
  header("// GPS RADAR");
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(WD_DIM, TFT_BLACK);
  tft.drawString("TOUCH TO EXIT", w / 2, 306, 1);
  tft.setTextDatum(TL_DATUM);

  float sweep = 0.0f;
  while (true) {
    if (exitTouched()) break;
    gps_obj.pump();
    tft.fillRect(RCX - RR - 2, RCY - RR - 2, 2 * RR + 4, 2 * RR + 4, TFT_BLACK);
    frame(RCX, RCY, RR, sweep, "GPS");
    // Draw satellites
    int sats = gps_obj.getNumSats();
    for (int i = 0; i < sats; i++) {
      float el = gps_obj.getSatElevation(i);
      float az = gps_obj.getSatAzimuth(i);
      float r = (90.0f - el) / 90.0f * RR;
      float a = az * PI / 180.0f;
      int16_t bx = RCX + (int16_t)(sinf(a) * r);
      int16_t by = RCY - (int16_t)(cosf(a) * r);
      tft.fillCircle(bx, by, 2, WD_CYAN);
    }
    tft.setTextDatum(TL_DATUM);
    tft.fillRect(4, 250, w - 8, 30, TFT_BLACK);
    tft.setTextColor(WD_CYAN, TFT_BLACK);
    tft.drawString(String("Sats: ") + sats, 8, 252, 2);
    sweep += 0.22f;
    if (sweep > 2.0f * PI) sweep -= 2.0f * PI;
    delay(40);
  }
  menu_function_obj.setSharkHudActivity(SHARK_HUD_GPS_ACTIVITY, false);
}

void SharkRadar::runSpeed() {
  TFT_eSPI& tft = display_obj.tft;
  const int16_t w = tft.width();
  tft.fillScreen(TFT_BLACK);
  header("// SPEED");
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(WD_DIM, TFT_BLACK);
  tft.drawString("TOUCH TO EXIT", w / 2, 306, 1);
  tft.setTextDatum(TL_DATUM);

  while (true) {
    if (exitTouched()) break;
    gps_obj.pump();
    bool fix = gps_obj.hasFix();
    float spd = gps_obj.getSpeed();
    float alt = gps_obj.getAltitude();
    float hdg = gps_obj.getHeading();
    tft.fillRect(0, 40, w, 220, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    if (!fix) {
      tft.setTextColor(WD_AMBER, TFT_BLACK);
      tft.drawString("NO FIX", w / 2, 140, 4);
    } else {
      tft.setTextColor(WD_CYAN, TFT_BLACK);
      tft.drawString(String(spd, 1) + " m/s", w / 2, 120, 4);
      tft.setTextColor(WD_BONE, TFT_BLACK);
      tft.drawString(String((int)hdg) + " deg  " + String(alt, 0) + " m", w / 2, 170, 2);
    }
    tft.setTextDatum(TL_DATUM);
    delay(100);
  }
}

void SharkRadar::runCompass() {
  TFT_eSPI& tft = display_obj.tft;
  const int16_t w = tft.width();
  tft.fillScreen(TFT_BLACK);
  header("// COMPASS");
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(WD_DIM, TFT_BLACK);
  tft.drawString("TOUCH TO EXIT", w / 2, 306, 1);
  tft.setTextDatum(TL_DATUM);

  while (true) {
    if (exitTouched()) break;
    gps_obj.pump();
    bool fix = gps_obj.hasFix();
    float hdg = gps_obj.getHeading();
    tft.fillRect(0, 40, w, 220, TFT_BLACK);
    const int16_t cx = 120, cy = 140, r = 80;
    tft.drawCircle(cx, cy, r, WD_EDGE);
    if (fix) {
      float a = hdg * PI / 180.0f;
      int16_t tipx = cx + (int16_t)(sinf(a) * (r - 10));
      int16_t tipy = cy - (int16_t)(cosf(a) * (r - 10));
      tft.drawLine(cx, cy, tipx, tipy, WD_CYAN);
      tft.fillCircle(tipx, tipy, 4, WD_CYAN);
    }
    tft.fillCircle(cx, cy, 3, WD_BONE);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(WD_CYAN, TFT_BLACK);
    tft.drawString(fix ? (String((int)hdg) + " deg") : "NO FIX", w / 2, 240, 2);
    tft.setTextDatum(TL_DATUM);
    delay(100);
  }
}

void SharkRadar::runClock() {
  TFT_eSPI& tft = display_obj.tft;
  const int16_t w = tft.width();
  tft.fillScreen(TFT_BLACK);
  header("// CLOCK");
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(WD_DIM, TFT_BLACK);
  tft.drawString("TOUCH TO EXIT", w / 2, 306, 1);
  tft.setTextDatum(TL_DATUM);

  while (true) {
    if (exitTouched()) break;
    gps_obj.pump();
    bool fix = gps_obj.hasFix();
    tft.fillRect(0, 40, w, 220, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    if (!fix) {
      tft.setTextColor(WD_AMBER, TFT_BLACK);
      tft.drawString("NO FIX", w / 2, 140, 4);
    } else {
      tft.setTextColor(WD_CYAN, TFT_BLACK);
      tft.drawString(gps_obj.getTimeString(), w / 2, 120, 4);
      tft.setTextColor(WD_BONE, TFT_BLACK);
      tft.drawString(gps_obj.getDateString(), w / 2, 170, 2);
    }
    tft.setTextDatum(TL_DATUM);
    delay(200);
  }
}

void SharkRadar::runWaypoint() {
  TFT_eSPI& tft = display_obj.tft;
  const int16_t w = tft.width();
  tft.fillScreen(TFT_BLACK);
  header("// WAYPOINT");
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(WD_DIM, TFT_BLACK);
  tft.drawString("TOUCH ABOVE TO EXIT", w / 2, 306, 1);
  tft.setTextDatum(TL_DATUM);

  static bool home_set = false;
  static double home_lat = 0, home_lon = 0;

  while (true) {
    uint16_t tx, ty;
    if (display_obj.updateTouch(&tx, &ty)) {
      if (ty < 250) {
        while (display_obj.updateTouch(&tx, &ty)) delay(10);
        break;
      }
      // Mark button area
      if (ty >= 262) {
        gps_obj.pump();
        if (gps_obj.hasFix()) {
          home_lat = gps_obj.getLat();
          home_lon = gps_obj.getLon();
          home_set = true;
        }
        while (display_obj.updateTouch(&tx, &ty)) delay(10);
      }
    }
    gps_obj.pump();
    bool fix = gps_obj.hasFix();
    double clat = gps_obj.getLat();
    double clon = gps_obj.getLon();

    double dist = 0, brg = 0;
    if (home_set && fix) {
      const double R = 6371000.0;
      const double la1 = clat * PI / 180.0, la2 = home_lat * PI / 180.0;
      const double dla = (home_lat - clat) * PI / 180.0;
      const double dlo = (home_lon - clon) * PI / 180.0;
      const double a = sin(dla / 2) * sin(dla / 2) +
                       cos(la1) * cos(la2) * sin(dlo / 2) * sin(dlo / 2);
      dist = R * 2 * atan2(sqrt(a), sqrt(1 - a));
      brg = atan2(sin(dlo) * cos(la2),
                  cos(la1) * sin(la2) - sin(la1) * cos(la2) * cos(dlo)) * 180.0 / PI;
      if (brg < 0) brg += 360.0;
    }

    tft.fillRect(0, 40, w, 220, TFT_BLACK);
    const int16_t cx = 120, cy = 120, r = 66;
    tft.drawCircle(cx, cy, r, WD_EDGE);
    if (home_set && fix) {
      const float a = brg * PI / 180.0f;
      const int16_t tipx = cx + (int16_t)(sinf(a) * (r - 8));
      const int16_t tipy = cy - (int16_t)(cosf(a) * (r - 8));
      tft.drawLine(cx, cy, tipx, tipy, WD_CYAN);
      tft.fillCircle(tipx, tipy, 5, WD_CYAN);
    }
    tft.fillCircle(cx, cy, 3, WD_BONE);

    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(WD_GREY, TFT_BLACK);
    if (!home_set) {
      tft.drawString("NO WAYPOINT SET", w / 2, 196, 2);
      tft.setTextColor(WD_DIM, TFT_BLACK);
      tft.drawString(fix ? "mark below to save this spot" : "waiting for fix...", w / 2, 220, 1);
    } else if (!fix) {
      tft.setTextColor(WD_AMBER, TFT_BLACK);
      tft.drawString("NO FIX", w / 2, 200, 4);
    } else {
      String ds = dist < 1000 ? (String((int)dist) + " m")
                              : (String(dist / 1000.0, 2) + " km");
      tft.setTextColor(WD_CYAN, TFT_BLACK);
      tft.drawString(ds, w / 2, 196, 4);
      tft.setTextColor(WD_BONE, TFT_BLACK);
      tft.drawString(String((int)brg) + " deg  " + cardinal8(brg), w / 2, 232, 2);
    }

    tft.fillRect(0, 262, w, 58, home_set ? WD_SURFACE : WD_PANEL);
    tft.drawFastHLine(0, 262, w, WD_CYAN);
    tft.setTextColor(fix ? WD_CYAN : WD_DIM, home_set ? WD_SURFACE : WD_PANEL);
    tft.drawString(home_set ? "RE-MARK HERE" : "MARK HERE", w / 2, 284, 2);
    tft.setTextColor(WD_DIM, home_set ? WD_SURFACE : WD_PANEL);
    tft.drawString("touch above to exit", w / 2, 304, 1);
    tft.setTextDatum(TL_DATUM);
    menu_function_obj.updateStatusBar();
    delay(160);
  }
}

#endif

#endif
