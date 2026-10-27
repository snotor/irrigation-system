#include <Arduino.h>
#include <esp_task_wdt.h>
#include <stdarg.h>
#include "Hardware.h"
#include "Settings.h"
#include "Scheduler.h"
#include "WifiPortal.h"
#include "Api.h"

SoilSensor      soil[ZONES];
TankSensor      tank;
RainSensor      rain;
Environment     env;
Clock           rtc;
Button          button;
ErrorLed        errLed;
Buzzer          buzzer;
WaterController water;
Settings        settings;
Scheduler       sched;
WifiPortal      net;
Api             api;

static uint32_t tSoil = 0;          // last soil read
static bool prevError = false;       // lets us beep once when an error appears
static bool prevAp = false;          // same for the setup AP opening
static bool serviceMode = false;     // button held during boot

static char lineBuf[64];             // serial settings commands: collected until Enter
static uint8_t lineLen = 0;

// printf-style append to a String
static void appendf(String& s, const char* fmt, ...) {
  char buf[192];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  s += buf;
}

// Plain-text status page: served at / and /health once on the network, and at /health in setup AP mode
static String healthText() {
  String s;
  s.reserve(1100);

  time_t t = rtc.unixNow();
  struct tm tm;
  gmtime_r(&t, &tm);
  char ts[24];
  strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);

  appendf(s, "Irrigation controller health\n\n");
  appendf(s, "uptime: %lu s | free heap: %lu B\n", (unsigned long)(millis() / 1000), (unsigned long)ESP.getFreeHeap());
  if (net.mode() == WifiPortal::Mode::STA) {
    appendf(s, "wifi: %s | %s | %d dBm\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
  } else {
    appendf(s, "wifi: setup AP (not on your network)\n");
  }
  appendf(s, "rtc: %s UTC (%s) | local: %s\n", ts, rtc.valid() ? "ok" : "BAD", sched.localTimeStr().c_str());
  appendf(s, "env: T %.1f C, H %.0f %%, P %.0f hPa (%s)\n",
          env.temperatureC(), env.humidity(), env.pressureHpa(), env.valid() ? "ok" : "FAULT");
  appendf(s, "tank: %s (%.1f cm)\n", tank.low() ? "LOW" : "ok", tank.distanceCm());
  appendf(s, "rain: %s\n", rain.wet() ? "WET" : "dry");
  appendf(s, "global: %s\n", blockName(sched.globalBlock()));
  appendf(s, "pump: %s | active zone: %d\n", water.pumpOn() ? "ON" : "off", (int)water.activeZone() + 1);
  for (uint8_t i = 0; i < ZONES; i++) {
    appendf(s, "zone %u: soil raw %d, %.0f %%%s | valve %s | watered today %lu s | %s\n",
            i + 1, soil[i].raw(), soil[i].percent(), soil[i].fault() ? " (SENSOR FAULT)" : "",
            water.valveOn(i) ? "OPEN" : "closed", (unsigned long)water.zoneSecondsToday(i),
            sched.zoneStatus(i).c_str());
  }
  appendf(s, "output faults: %s\n", water.faulted() ? "YES (on-time cap tripped, clear with x)" : "none");
  return s;
}

static void printSettings() {
  const SettingsData& d = settings.d;
  Serial.printf("now: %s\n", sched.localTimeStr().c_str());
  Serial.printf("global: allOff %u | winter %u | frost %.1f C\n", d.allOff, d.winter, d.frostC);
  for (uint8_t i = 0; i < ZONES; i++) {
    const ZoneSettings& z = d.zone[i];
    Serial.printf("zone %u: %s | mode %u | run %u s | window %02u:%02u-%02u:%02u | cold %d C | covered until %lu | %s\n",
                  i + 1, z.enabled ? "on" : "off", z.mode, z.runS,
                  z.startMin / 60, z.startMin % 60, z.endMin / 60, z.endMin % 60,
                  (int)z.coldC, (unsigned long)z.coveredUntil, sched.zoneStatus(i).c_str());
  }
}

static void saveAndShow() {
  Serial.println(settings.save() ? "saved" : "SAVE FAILED");
  printSettings();
}

// Serial settings commands (until the web UI exists):
//   z<n> on|off | run <s> | win HH:MM HH:MM | cold <C> | mode <0-2> | cover YYYY-MM-DD|off
//   g alloff 0|1 | winter 0|1 | frost <C>
//   t YYYY-MM-DD HH:MM[:SS]     set the clock (local time)
static void handleLine(char* s) {
  while (*s == ' ') s++;
  SettingsData& d = settings.d;
  int z = 0, n = 0, a = 0, b = 0, c = 0, e = 0, f = 0;
  char w[12] = "";

  if (sscanf(s, "z%d %11s%n", &z, w, &n) == 2 && z >= 1 && z <= ZONES) {
    ZoneSettings& zs = d.zone[z - 1];
    const char* rest = s + n;
    if (!strcmp(w, "on"))        zs.enabled = 1;
    else if (!strcmp(w, "off"))  zs.enabled = 0;
    else if (!strcmp(w, "run")  && sscanf(rest, "%d", &a) == 1) zs.runS = (uint16_t)constrain(a, 0, 65535);
    else if (!strcmp(w, "cold") && sscanf(rest, "%d", &a) == 1) zs.coldC = (int8_t)constrain(a, -128, 127);
    else if (!strcmp(w, "mode") && sscanf(rest, "%d", &a) == 1) zs.mode = (uint8_t)constrain(a, 0, 255);
    else if (!strcmp(w, "win")  && sscanf(rest, "%d:%d %d:%d", &a, &b, &c, &e) == 4) {
      zs.startMin = (uint16_t)constrain(a * 60 + b, 0, 65535);
      zs.endMin   = (uint16_t)constrain(c * 60 + e, 0, 65535);
    }
    else if (!strcmp(w, "cover")) {
      if (strstr(rest, "off")) zs.coveredUntil = 0;
      else if (sscanf(rest, "%d-%d-%d", &a, &b, &c) == 3) zs.coveredUntil = Scheduler::localToUnix(a, b, c, 23, 59, 59);
      else { Serial.println("usage: z<n> cover YYYY-MM-DD|off"); return; }
    }
    else { Serial.println("usage: z<n> on|off|run <s>|win HH:MM HH:MM|cold <C>|mode <0-2>|cover <date>|off"); return; }
    saveAndShow();
    return;
  }

  if (sscanf(s, "g %11s %d%n", w, &a, &n) == 2) {
    if (!strcmp(w, "alloff"))      d.allOff = a ? 1 : 0;
    else if (!strcmp(w, "winter")) d.winter = a ? 1 : 0;
    else if (!strcmp(w, "frost"))  d.frostC = (float)a;
    else { Serial.println("usage: g alloff|winter 0|1, g frost <C>"); return; }
    saveAndShow();
    return;
  }

  if (s[0] == 't' && (n = sscanf(s, "t %d-%d-%d %d:%d:%d", &a, &b, &c, &e, &f, &z)) >= 5) {
    if (n < 6) z = 0;
    bool ok = a >= 2026 && b >= 1 && b <= 12 && c >= 1 && c <= 31 && e >= 0 && e <= 23 && f >= 0 && f <= 59 &&
              z >= 0 && z <= 59 && sched.setLocalTime(a, b, c, e, f, z);
    Serial.println(ok ? "clock set" : "bad time (usage: t YYYY-MM-DD HH:MM[:SS], local time)");
    Serial.printf("now: %s\n", sched.localTimeStr().c_str());
    return;
  }

  Serial.println("commands: 1-4 water zone, s stop, x clear faults, p settings, l log | z<n> ..., g ..., t ...");
}

void setup() {
  // 1) Outputs to the safe state before anything else
  water.begin(&tank);

  Serial.begin(115200);
  esp_task_wdt_init(WDT_TIMEOUT_S, true);   // panic + reboot if loop() hangs
  esp_task_wdt_add(NULL);

  // 2) Service mode = button held at boot. Wait for the release, otherwise it
  //    would count as a SHORT press and start zone 1.
  button.begin(PIN_BUTTON);
  delay(50);                                // let the line settle
  serviceMode = button.pressed();
  if (serviceMode) {
    while (button.pressed()) {
      esp_task_wdt_reset();
      delay(10);
    }
    delay(BUTTON_DEBOUNCE_MS);
  }

  for (uint8_t i = 0; i < ZONES; i++) soil[i].begin(PIN_SOIL[i], SOIL_DRY_RAW[i], SOIL_WET_RAW[i]);
  tank.begin(PIN_TANK_TRIG, PIN_TANK_ECHO);
  rain.begin(PIN_RAIN_OUT, PIN_RAIN_VCC);
  env.begin();
  rtc.begin();
  errLed.begin(PIN_ERROR_LED);
  buzzer.begin(PIN_BUZZER);

  // 3) Settings and scheduler (after the sensors they read)
  settings.begin();
  sched.begin(settings, water, rtc, tank, rain, env, soil);

  // 4) WiFi: saved credentials, or (also in service mode) the open setup AP
  net.setHealth(healthText);
  api.begin(settings, sched, water, rtc, tank, rain, env, soil, net);
  net.setRoutes([](WebServer& s) { api.routes(s); });   // before net.begin(), which registers the routes
  net.begin(serviceMode);

  if (serviceMode) {
    buzzer.beep(3);
    Serial.println("SERVICE MODE (button held at boot): WiFi setup AP. TODO: calibration, factory reset");
  }
  Serial.println("Ready. 1-4 water zone, s stop, x clear faults, p settings, l log; z/g/t commands set values (see main.cpp)");
  printSettings();
}

void loop() {
  esp_task_wdt_reset();

  // Sensors
  tank.update();
  rain.update();
  env.update();
  rtc.update();
  if (elapsed(tSoil, 1000)) {
    tSoil = millis();
    for (auto& s : soil) s.update();
  }

  // Safety layer: every pass
  water.update();

  // Schedule (asks the safety layer for runs; never touches the outputs itself)
  sched.update();

  // Network (non-blocking)
  net.update();

  // Bench controls: button = zone 1 / stop
  ButtonEvent be = button.update();
  if (be == ButtonEvent::SHORT) Serial.println(resultName(sched.startManual(0, MIN_RUN_S)));
  if (be == ButtonEvent::LONG) water.stop(StopReason::USER);

  // Serial: single keys act at once (only as the first character of a line), longer commands end with Enter
  while (Serial.available()) {
    char c = Serial.read();
    if (lineLen == 0 && c >= '1' && c < '1' + ZONES) Serial.println(resultName(sched.startManual(c - '1', MIN_RUN_S)));
    else if (lineLen == 0 && c == 's') water.stop(StopReason::USER);
    else if (lineLen == 0 && c == 'x') water.clearFaults();
    else if (lineLen == 0 && c == 'p') printSettings();
    else if (lineLen == 0 && c == 'l') sched.startDump();
    else if (c == '\n' || c == '\r') {
      if (lineLen) {
        lineBuf[lineLen] = 0;
        lineLen = 0;
        handleLine(lineBuf);
      }
    }
    else if (lineLen < sizeof(lineBuf) - 1) lineBuf[lineLen++] = c;
    else lineLen = 0;
  }

  // Indicators
  bool anyError = tank.low() || !env.valid() || water.faulted() || !rtc.valid();
  for (auto& s : soil) anyError |= s.fault();
  errLed.set(anyError);
  if (anyError && !prevError) buzzer.beep(2);
  prevError = anyError;

  if (net.apMode() && !prevAp && !serviceMode) buzzer.beep(3);   // AP opened after a failed connection
  prevAp = net.apMode();

  errLed.update();
  buzzer.update();
}
