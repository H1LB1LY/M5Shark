#ifndef SHARK_RADAR_H
#define SHARK_RADAR_H

#include <Arduino.h>
#include "WiFiScan.h"
#include "configs.h"

class SharkRadar {
public:
  SharkRadar();
  void runSharkSense(bool bleOnly = false);

  // GPS helpers used by other SHARK modules
  void gpsSetup();
  void gpsPoll();
  void gpsPowerOn();
  void gpsPowerOff();
  void gpsSleep();
  void gpsWake();
  bool gpsHasFix();
  void gpsGetLatLon(double &lat, double &lon);
  void gpsGetSpeedHeading(float &speed, float &heading);

private:
  float rssiToDistance(int rssi);
  uint16_t channelColor(int channel);
  String shortenSSID(const String &ssid);
  void drawCalibrationScreen();
  void drawRadarFrame(float scaleMeters, float heading);
  void drawBlip(float angleDeg, float distMeters, float scaleMeters,
                uint16_t color, const String &label, float heading);
  float getHeading(); // IMU stub — returns 0 until sensor present
};

#endif
