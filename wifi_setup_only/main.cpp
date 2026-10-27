#include <Arduino.h>
#include <esp_task_wdt.h>
#include <stdarg.h>
#include <esp_system.h>
#include "Hardware.h"
#include "WifiPortal.h"

// TEMPORARY test: start the setup AP automatically on every boot (no button needed) and print troubleshooting
// logs: reset reason, WiFi driver events and a status line every 5 s. Comment this line out for the normal firmware.
//#define TEST_SETUP_AUTO

SoilSensor      soil[ZONES];
TankSensor      tank;
RainSensor      rain;
Environment     env;
Clock           rtc;
Button          button;
ErrorLed        errLed;
Buzzer          buzzer;
WaterController water;
WifiPortal      net;

static uint32_t tSoil = 0;
static bool prevError = false;
static bool prevAp = false;
static bool serviceMode = false;   // set when the button is held during boot

static const char* resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "POWERON (normal power-up)";
    case ESP_RST_EXT:       return "EXT (reset pin)";
    case ESP_RST_SW:        return "SW (software restart)";
    case ESP_RST_PANIC:     return "PANIC (crash)";
    case ESP_RST_INT_WDT:   return "INT_WDT (interrupt watchdog)";
    case ESP_RST_TASK_WDT:  return "TASK_WDT (loop hung)";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT (supply dipped below threshold!)";
    default:                return "other";
  }
}

#ifdef TEST_SETUP_AUTO
static const char* portalModeName(WifiPortal::Mode m) {
  switch (m) {
    case WifiPortal::Mode::OFF:        return "OFF";
    case WifiPortal::Mode::CONNECTING: return "CONNECTING";
    case WifiPortal::Mode::STA:        return "STA";
    case WifiPortal::Mode::PRESCAN:    return "PRESCAN";
    default:                           return "AP";
  }
}

// WiFi driver events with timestamps. SCAN_DONE status 0 = finished normally; anything else = aborted.
// If a scan fails and no SCAN_DONE line appears, the done event never arrived.
static void wifiEvent(arduino_event_id_t ev, arduino_event_info_t info) {
  unsigned long t = millis();
  switch (ev) {
    case ARDUINO_EVENT_WIFI_STA_START:   Serial.printf("[evt %lu] STA_START\n", t); break;
    case ARDUINO_EVENT_WIFI_STA_STOP:    Serial.printf("[evt %lu] STA_STOP\n", t); break;
    case ARDUINO_EVENT_WIFI_SCAN_DONE:
      Serial.printf("[evt %lu] SCAN_DONE status=%u found=%u\n", t, (unsigned)info.wifi_scan_done.status, (unsigned)info.wifi_scan_done.number);
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.printf("[evt %lu] STA_DISCONNECTED reason=%u\n", t, (unsigned)info.wifi_sta_disconnected.reason);
      break;
    case ARDUINO_EVENT_WIFI_AP_START:    Serial.printf("[evt %lu] AP_START\n", t); break;
    case ARDUINO_EVENT_WIFI_AP_STOP:     Serial.printf("[evt %lu] AP_STOP\n", t); break;
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:    Serial.printf("[evt %lu] AP: a device joined\n", t); break;
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED: Serial.printf("[evt %lu] AP: a device left\n", t); break;
    default: Serial.printf("[evt %lu] event %d\n", t, (int)ev); break;
  }
}
#endif

static const char* resultName(StartResult r) {
  switch (r) {
    case StartResult::OK: return "OK";
    case StartResult::BUSY: return "BUSY";
    case StartResult::BAD_ZONE: return "BAD_ZONE";
    case StartResult::TANK_LOW: return "TANK_LOW";
    case StartResult::TOO_SHORT: return "TOO_SHORT";
    case StartResult::DAILY_LIMIT: return "DAILY_LIMIT";
    case StartResult::VALVE_FAULT: return "VALVE_FAULT";
    default: return "PUMP_FAULT";
  }
}

static void addf(String& s, const char* fmt, ...) {
  char b[192];
  va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
  s += b;
}

// Plain-text health page (served at / and /health once on the network; also reachable in setup AP mode at /health)
static String healthText() {
  String s; s.reserve(800);
  time_t t = rtc.unixNow(); struct tm tm; gmtime_r(&t, &tm);
  char ts[24]; strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);

  addf(s, "Irrigation controller health\n\n");
  addf(s, "uptime: %lu s | free heap: %lu B\n", (unsigned long)(millis() / 1000), (unsigned long)ESP.getFreeHeap());
  if (net.mode() == WifiPortal::Mode::STA)
    addf(s, "wifi: %s | %s | %d dBm\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
  else
    addf(s, "wifi: setup AP (not on your network)\n");
  addf(s, "rtc: %s UTC (%s)\n", ts, rtc.valid() ? "ok" : "BAD");
  addf(s, "env: T %.1f C, H %.0f %%, P %.0f hPa (%s)\n", env.temperatureC(), env.humidity(), env.pressureHpa(), env.valid() ? "ok" : "FAULT");
  addf(s, "tank: %s (%.1f cm)\n", tank.low() ? "LOW" : "ok", tank.distanceCm());
  addf(s, "rain: %s\n", rain.wet() ? "WET" : "dry");
  addf(s, "pump: %s | active zone: %d\n", water.pumpOn() ? "ON" : "off", (int)water.activeZone() + 1);
  for (uint8_t i = 0; i < ZONES; i++)
    addf(s, "zone %u: soil raw %d, %.0f %%%s | valve %s | watered today %lu s\n",
         i + 1, soil[i].raw(), soil[i].percent(), soil[i].fault() ? " (SENSOR FAULT)" : "",
         water.valveOn(i) ? "OPEN" : "closed", (unsigned long)water.zoneSecondsToday(i));
  addf(s, "output faults: %s\n", water.faulted() ? "YES (on-time cap tripped, clear with x)" : "none");
  return s;
}

void setup() {
  // 1) Outputs to safe state before anything else
  water.begin(&tank);

  Serial.begin(115200);
  Serial.printf("\nBOOT: reset reason: %s | %s rev %d | %u MHz | free heap %lu B\n",
                resetReasonName(esp_reset_reason()), ESP.getChipModel(), (int)ESP.getChipRevision(),
                (unsigned)ESP.getCpuFreqMHz(), (unsigned long)ESP.getFreeHeap());
#ifdef TEST_SETUP_AUTO
  Serial.println("TEST MODE: setup AP starts automatically (comment out TEST_SETUP_AUTO in main.cpp to disable)");
  WiFi.onEvent(wifiEvent);
#endif
  esp_task_wdt_init(WDT_TIMEOUT_S, true);   // panic + reboot if loop() hangs
  esp_task_wdt_add(NULL);

  // 2) Service mode: button held at boot. Wait for release so the button does not
  //    report it as a SHORT press (which would start zone 1).
  button.begin(PIN_BUTTON);
#ifdef BENCH_BUTTON_GPIO2
  pinMode(PIN_BUTTON, INPUT_PULLUP);        // Button::begin sets a plain INPUT
  pinMode(2, OUTPUT); digitalWrite(2, LOW); // GPIO2 is the button's ground
#endif
  delay(50);                                // let the line settle
  serviceMode = button.pressed();
#ifdef TEST_SETUP_AUTO
  serviceMode = true;
#endif
  if (serviceMode) {
    while (button.pressed()) { esp_task_wdt_reset(); delay(10); }
    delay(BUTTON_DEBOUNCE_MS);
  }

  for (uint8_t i = 0; i < ZONES; i++) soil[i].begin(PIN_SOIL[i], SOIL_DRY_RAW[i], SOIL_WET_RAW[i]);
  tank.begin(PIN_TANK_TRIG, PIN_TANK_ECHO);
  rain.begin(PIN_RAIN_OUT, PIN_RAIN_VCC);
  env.begin();
  rtc.begin();
  errLed.begin(PIN_ERROR_LED);
  buzzer.begin(PIN_BUZZER);

  // 3) WiFi: saved credentials, else (or in service mode) the open setup AP
  net.setHealth(healthText);
  net.begin(serviceMode);

  if (serviceMode) {
    buzzer.beep(3);
    Serial.println("SERVICE MODE (button held at boot): WiFi setup AP. TODO: calibration, factory reset");
  }
  Serial.printf("Ready. Commands: 1-%u water zone (min run), s stop, x clear faults\n", ZONES);
}

void loop() {
  esp_task_wdt_reset();

  // Sensors
  tank.update();
  rain.update();
  env.update();
  rtc.update();
  if (elapsed(tSoil, 1000)) { tSoil = millis(); for (auto& s : soil) s.update(); }

  // Safety layer runs every pass
  water.update();

  // Network (non-blocking)
  net.update();

#ifdef TEST_SETUP_AUTO
  static uint32_t tDiag = 0, passes = 0;
  passes++;
  if (elapsed(tDiag, 5000)) {
    Serial.printf("[diag %lu s] portal=%s clients=%u loops/s=%lu heap=%lu (min %lu)\n", (unsigned long)(millis() / 1000),
                  portalModeName(net.mode()), net.apMode() ? (unsigned)WiFi.softAPgetStationNum() : 0u,
                  (unsigned long)(passes / 5), (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap());
    passes = 0; tDiag = millis();
  }
#endif

  // Bench controls
  ButtonEvent be = button.update();
  if (be == ButtonEvent::SHORT) Serial.println(resultName(water.start(0, MIN_RUN_S)));
  if (be == ButtonEvent::LONG)  water.stop(StopReason::USER);

  while (Serial.available()) {
    char c = Serial.read();
    if (c >= '1' && c < '1' + ZONES) Serial.println(resultName(water.start(c - '1', MIN_RUN_S)));
    else if (c == 's') water.stop(StopReason::USER);
    else if (c == 'x') water.clearFaults();
  }

  // Indicators
  bool anyError = tank.low() || !env.valid() || water.faulted() || !rtc.valid();
  for (auto& s : soil) anyError |= s.fault();
  errLed.set(anyError);
  if (anyError && !prevError) buzzer.beep(2);
  prevError = anyError;
  if (net.apMode() && !prevAp && !serviceMode) buzzer.beep(3);   // setup AP opened after a failed connection
  prevAp = net.apMode();
  errLed.update();
  buzzer.update();
}