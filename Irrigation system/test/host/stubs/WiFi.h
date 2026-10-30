#pragma once
#include <Arduino.h>
typedef enum { WIFI_OFF, WIFI_STA, WIFI_AP, WIFI_AP_STA } wifi_mode_t;
typedef enum { WL_IDLE_STATUS = 0, WL_CONNECTED = 3, WL_DISCONNECTED = 6 } wl_status_t;
typedef enum { WIFI_AUTH_OPEN = 0, WIFI_AUTH_WPA2_PSK = 3 } wifi_auth_mode_t;
typedef int arduino_event_id_t;
#define ARDUINO_EVENT_WIFI_STA_DISCONNECTED 5
#define WIFI_SCAN_RUNNING (-1)
struct arduino_event_info_t { struct { uint8_t reason; } wifi_sta_disconnected; };
namespace host { extern wl_status_t wifiStatus; extern int wifiRssi; }
class WiFiClass {
public:
  void persistent(bool) {}
  void onEvent(void (*)(arduino_event_id_t, arduino_event_info_t), arduino_event_id_t) {}
  bool mode(wifi_mode_t) { return true; }
  wifi_mode_t getMode() { return WIFI_STA; }
  void setHostname(const char*) {}
  void setSleep(bool) {}
  void setAutoReconnect(bool) {}
  void begin(const char*, const char*) {}
  void disconnect(bool, bool) {}
  wl_status_t status() { return host::wifiStatus; }
  String SSID() { return String("TestNet"); }
  String SSID(int) { return String("Net"); }
  int RSSI() { return host::wifiRssi; }
  int RSSI(int) { return -60; }
  wifi_auth_mode_t encryptionType(int) { return WIFI_AUTH_WPA2_PSK; }
  IPAddress localIP() { return IPAddress(192, 168, 1, 50); }
  IPAddress softAPIP() { return IPAddress(192, 168, 4, 1); }
  void scanDelete() {}
  int16_t scanNetworks(bool, bool) { return -1; }
  int16_t scanComplete() { return 0; }
  void macAddress(uint8_t* m) { memset(m, 0xAB, 6); }
  bool softAP(const char*, const char*, int, int, int) { return true; }
  bool softAPConfig(IPAddress, IPAddress, IPAddress) { return true; }
  uint8_t softAPgetStationNum() { return 0; }
};
extern WiFiClass WiFi;
