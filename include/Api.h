#pragma once
// HTTP API for the controller UI (portal/app.html), added to WifiPortal's web server.
// Same thread as everything else: a handler runs between two loop passes, so the settings and the
// scheduler never race with it. The API only ever REQUESTS things: watering still goes through
// Scheduler::startManual -> WaterController, so every interlock and cap applies.
//
//   GET  /api/state            everything the UI shows (JSON)
//   GET  /api/log              the last ~40 log rows (CSV text, no header)
//   GET  /log.csv              the whole current log (download)
//   POST /api/zone             id, then any of: en run s e cold cov mode
//   POST /api/global           any of: off win frost
//   POST /api/water            id, sec           -> {"r":"OK"|"BUSY"|"TANK_LOW"|...,"cold":0|1}
//   POST /api/stop
//   POST /api/clock            t = unix seconds UTC (the phone's clock)
//   POST /api/faults/clear
// POST bodies are form-encoded (WebServer parses them into args). Answers are {"ok":1} or {"e":"<what>"}.
//
// No login: anyone on the same WiFi can use it. Fine for a home network; do not expose it to the internet.
#include <Arduino.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <stdarg.h>
#include "Hardware.h"
#include "Settings.h"
#include "Scheduler.h"
#include "WifiPortal.h"

class Api {
public:
  void begin(Settings& cfg, Scheduler& sched, WaterController& water, Clock& rtc, TankSensor& tank,
             RainSensor& rain, Environment& env, SoilSensor* soil, WifiPortal& net) {
    _cfg = &cfg; _sched = &sched; _water = &water; _rtc = &rtc; _tank = &tank;
    _rain = &rain; _env = &env; _soil = soil; _net = &net;
  }

  // Passed to WifiPortal::setRoutes()
  void routes(WebServer& s) {
    _s = &s;
    s.on("/api/state",        HTTP_GET,  [this]() { if (guard()) json(state()); });
    s.on("/api/log",          HTTP_GET,  [this]() { if (guard()) logTail(); });
    s.on("/log.csv",          HTTP_GET,  [this]() { if (guard()) logFile(); });
    s.on("/api/zone",         HTTP_POST, [this]() { if (guard()) postZone(); });
    s.on("/api/global",       HTTP_POST, [this]() { if (guard()) postGlobal(); });
    s.on("/api/water",        HTTP_POST, [this]() { if (guard()) postWater(); });
    s.on("/api/stop",         HTTP_POST, [this]() { if (guard()) { _water->stop(StopReason::USER); json("{\"ok\":1}"); } });
    s.on("/api/clock",        HTTP_POST, [this]() { if (guard()) postClock(); });
    s.on("/api/faults/clear", HTTP_POST, [this]() { if (guard()) { _water->clearFaults(); json("{\"ok\":1}"); } });
  }

  // The whole state as JSON (public for the host test)
  String state() {
    String j;
    j.reserve(1600);
    char a[12], b[12], c[12];
    uint32_t now = _rtc->unixNow();

    tm lt;
    char ts[24] = "";
    if (_sched->localNow(lt)) strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &lt);

    app(j, "{\"t\":%lu,\"ok\":%d,\"lt\":\"%s\",\"up\":%lu,", (unsigned long)now, _rtc->valid() ? 1 : 0, ts,
        (unsigned long)(millis() / 1000));
    app(j, "\"wifi\":{\"ssid\":\"%s\",\"rssi\":%d},", jsonEscape(WiFi.SSID()).c_str(), (int)WiFi.RSSI());
    app(j, "\"env\":{\"ok\":%d,\"c\":%s,\"h\":%s,\"p\":%s},", _env->valid() ? 1 : 0,
        fn(a, sizeof(a), _env->temperatureC(), 1), fn(b, sizeof(b), _env->humidity(), 0), fn(c, sizeof(c), _env->pressureHpa(), 0));
    app(j, "\"tank\":{\"low\":%d,\"cm\":%s},\"rain\":%d,", _tank->low() ? 1 : 0, fn(a, sizeof(a), _tank->distanceCm(), 1), _rain->wet() ? 1 : 0);

    const SettingsData& d = _cfg->d;
    app(j, "\"g\":{\"off\":%d,\"win\":%d,\"frost\":%s,\"blk\":\"%s\"},", d.allOff, d.winter, fn(a, sizeof(a), d.frostC, 1),
        blockName(_sched->globalBlock()));
    app(j, "\"pump\":%d,\"az\":%d,\"el\":%lu,\"tot\":%lu,\"flt\":%d,\"z\":[", _water->pumpOn() ? 1 : 0, (int)_water->activeZone(),
        (unsigned long)_water->runElapsedS(), (unsigned long)_water->runTotalS(), _water->faulted() ? 1 : 0);

    for (uint8_t z = 0; z < ZONES; z++) {
      const ZoneSettings& zs = d.zone[z];
      char cov[12] = "";
      if (zs.coveredUntil > now) {                           // an expired date shows as empty
        time_t u = zs.coveredUntil;
        tm ct;
        localtime_r(&u, &ct);
        strftime(cov, sizeof(cov), "%Y-%m-%d", &ct);
      }
      app(j, "%s{\"en\":%d,\"m\":%d,\"run\":%u,\"s\":%u,\"e\":%u,\"cold\":%d,\"cov\":\"%s\",\"soil\":%s,\"sf\":%d,\"today\":%lu,\"st\":\"%s\",\"why\":\"%s\"}",
          z ? "," : "", zs.enabled, zs.mode, zs.runS, zs.startMin, zs.endMin, (int)zs.coldC, cov,
          fn(a, sizeof(a), _soil[z].percent(), 0), _soil[z].fault() ? 1 : 0, (unsigned long)_water->zoneSecondsToday(z),
          stateName(_sched->zoneState(z)), _sched->zoneWhy(z));
    }
    j += "]}";
    return j;
  }

private:
  bool guard() {
    if (_net->mode() == WifiPortal::Mode::STA) return true;   // the setup AP is an open network: no API there
    _s->send(404, "text/plain", "not found");
    return false;
  }

  void json(const String& j) {
    _s->sendHeader("Cache-Control", "no-store");
    _s->send(200, "application/json", j);
  }

  bool has(const char* k) { return _s->hasArg(k); }
  int num(const char* k) { return (int)_s->arg(k).toInt(); }

  // ---- settings ----
  void postZone() {
    int id = num("id");
    if (id < 1 || id > ZONES) { json("{\"e\":\"id\"}"); return; }
    ZoneSettings nz = _cfg->d.zone[id - 1];                  // edit a copy, apply only if everything is valid

    if (has("en"))   nz.enabled = num("en") ? 1 : 0;
    if (has("mode")) nz.mode = (uint8_t)constrain(num("mode"), 0, 255);
    if (has("run"))  nz.runS = (uint16_t)constrain(num("run"), 0, 65535);
    if (has("cold")) nz.coldC = (int8_t)constrain(num("cold"), -128, 127);
    if (has("s"))    nz.startMin = (uint16_t)constrain(num("s"), 0, 65535);
    if (has("e"))    nz.endMin = (uint16_t)constrain(num("e"), 0, 65535);
    if (nz.endMin > 1440 || nz.startMin >= nz.endMin) { json("{\"e\":\"win\"}"); return; }
    if (has("cov")) {
      int y, m, dd;
      String v = _s->arg("cov");
      if (v.length() == 0 || v == "0") nz.coveredUntil = 0;
      else if (sscanf(v.c_str(), "%d-%d-%d", &y, &m, &dd) == 3 && y >= 2026 && m >= 1 && m <= 12 && dd >= 1 && dd <= 31)
        nz.coveredUntil = Scheduler::localToUnix(y, m, dd, 23, 59, 59);
      else { json("{\"e\":\"cov\"}"); return; }
    }

    _cfg->d.zone[id - 1] = nz;
    json(_cfg->save() ? "{\"ok\":1}" : "{\"e\":\"save\"}");   // save() clamps to the Config.h limits
  }

  void postGlobal() {
    if (has("off"))   _cfg->d.allOff = num("off") ? 1 : 0;
    if (has("win"))   _cfg->d.winter = num("win") ? 1 : 0;
    if (has("frost")) _cfg->d.frostC = _s->arg("frost").toFloat();
    json(_cfg->save() ? "{\"ok\":1}" : "{\"e\":\"save\"}");
  }

  // ---- actions ----
  void postWater() {
    int id = num("id");
    int sec = num("sec");
    if (id < 1 || id > ZONES || sec < 0) { json("{\"e\":\"id\"}"); return; }
    bool cold = _sched->belowCold(id - 1);
    StartResult r = _sched->startManual(id - 1, (uint32_t)sec);
    char b[64];
    snprintf(b, sizeof(b), "{\"r\":\"%s\",\"cold\":%d}", resultName(r), (r == StartResult::OK && cold) ? 1 : 0);
    json(b);
  }

  // The phone sets the controller's clock (UTC, so the phone's time zone does not matter)
  void postClock() {
    uint32_t t = (uint32_t)strtoul(_s->arg("t").c_str(), nullptr, 10);
    if (t < RTC_MIN_VALID_UNIX || t > 4102444800UL) { json("{\"e\":\"time\"}"); return; }
    int32_t diff = (int32_t)(t - _rtc->unixNow());
    if (!_rtc->valid() || diff > 1 || diff < -1) _rtc->setUnix(t);
    json("{\"ok\":1}");
  }

  // ---- log ----
  // Only the tail: seek near the end instead of reading a whole file that may be 256 KB
  void logTail() {
    File f = _sched->openLog();
    if (!f) { _s->send(200, "text/plain", ""); return; }
    size_t size = f.size();
    size_t start = size > TAIL_BYTES ? size - TAIL_BYTES : 0;
    f.seek(start);
    String out;
    out.reserve(TAIL_BYTES + 8);
    while (f.available()) { char ch = (char)f.read(); out += ch; }
    f.close();
    if (start > 0) {                                         // began mid-row: drop the partial first row
      int nl = out.indexOf('\n');
      out = nl >= 0 ? out.substring(nl + 1) : String("");
    }
    _s->sendHeader("Cache-Control", "no-store");
    _s->send(200, "text/plain", out);
  }

  // Streams the file (blocks the loop for the transfer, about a second at most with the 256 KB cap)
  void logFile() {
    File f = _sched->openLog();
    if (!f) { _s->send(404, "text/plain", "no log yet"); return; }
    _s->sendHeader("Content-Disposition", "attachment; filename=irrigation-log.csv");
    _s->streamFile(f, "text/csv");
    f.close();
  }

  // ---- helpers ----
  static constexpr size_t TAIL_BYTES = 6000;

  static const char* stateName(Scheduler::ZState s) {
    switch (s) {
      case Scheduler::ZState::ZONE_OFF: return "DISABLED";
      case Scheduler::ZState::DONE:     return "DONE";
      case Scheduler::ZState::SKIPPED:  return "SKIPPED";
      case Scheduler::ZState::QUEUED:   return "QUEUED";
      case Scheduler::ZState::BLOCKED:  return "BLOCKED";
      default:                          return "WAITING";
    }
  }

  // printf-style append to a String
  static void app(String& s, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

  // A float as JSON: "null" for NAN
  static const char* fn(char* buf, size_t n, float v, int dec) {
    if (isnan(v)) snprintf(buf, n, "null");
    else snprintf(buf, n, "%.*f", dec, v);
    return buf;
  }

  Settings* _cfg = nullptr;
  Scheduler* _sched = nullptr;
  WaterController* _water = nullptr;
  Clock* _rtc = nullptr;
  TankSensor* _tank = nullptr;
  RainSensor* _rain = nullptr;
  Environment* _env = nullptr;
  SoilSensor* _soil = nullptr;
  WifiPortal* _net = nullptr;
  WebServer* _s = nullptr;
};

inline void Api::app(String& s, const char* fmt, ...) {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  s += buf;
}
