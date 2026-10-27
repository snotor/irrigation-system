#pragma once
// Schedule job source. Decides WHEN a zone should run; WaterController still decides whether it MAY
// (interlocks and caps stay there, so a scheduler bug cannot get past them).
//
//  - converts the RTC's UTC time to local time (DST included) and resets the daily caps at local midnight
//  - one job per enabled zone per day, started inside its start window, one zone at a time in zone order
//  - precedence: all off / winter / frost / low tank block everything; then per zone: disabled,
//    cold limit, rain (ignored while the zone is covered); blocked = retry until the window ends, then skip and log
//  - every run and every skip is written to /log.csv
//  - modes Closed loop / Open loop are not implemented yet: they run the Schedule logic
#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <time.h>
#include <stdlib.h>
#include "Hardware.h"
#include "Settings.h"

enum class Block : uint8_t { NONE, ALL_OFF, WINTER, FROST, TANK_LOW, ZONE_OFF, COLD, RAIN };

inline const char* blockName(Block b) {
  switch (b) {
    case Block::ALL_OFF:  return "ALL_OFF";
    case Block::WINTER:   return "WINTER";
    case Block::FROST:    return "FROST";
    case Block::TANK_LOW: return "TANK_LOW";
    case Block::ZONE_OFF: return "DISABLED";
    case Block::COLD:     return "COLD";
    case Block::RAIN:     return "RAIN";
    default:              return "NONE";
  }
}

class Scheduler {
public:
  void begin(Settings& cfg, WaterController& water, Clock& rtc, TankSensor& tank,
             RainSensor& rain, Environment& env, SoilSensor* soil) {
    _cfg = &cfg; _water = &water; _rtc = &rtc; _tank = &tank; _rain = &rain; _env = &env; _soil = soil;

    setenv("TZ", LOCAL_TZ, 1);
    tzset();

    _fsOk = LittleFS.begin(true);
    if (!_fsOk) Serial.println("LOG: LittleFS mount failed, runs will not be logged");

    Preferences p;                                // last day each zone was handled: a reboot must not water twice
    if (p.begin("state", false)) {
      for (uint8_t z = 0; z < ZONES; z++) {
        _doneDay[z] = p.getUInt(key('d', z), 0);
        _skipDay[z] = p.getUInt(key('k', z), 0);
      }
      p.end();
    }
  }

  // Call every loop pass, after water.update()
  void update() {
    trackRun();
    pumpDump();
    if (!elapsed(_tTick, 1000)) return;
    _tTick = millis();

    tm lt;
    if (!localNow(lt)) return;                    // no valid clock, no schedule (the error LED already shows it)
    uint32_t day = dayKey(lt);
    uint16_t minute = lt.tm_hour * 60 + lt.tm_min;

    if (day != _day) {                            // local midnight (or first tick after boot)
      if (_day != 0) _water->resetDaily();
      _day = day;
      for (uint8_t z = 0; z < ZONES; z++) { _pend[z] = false; _lastBlock[z] = Block::NONE; }
      Serial.printf("SCHED: new day %s, daily counters reset\n", localTimeStr().c_str());
    }

    Block gb = globalBlock();
    bool started = false;

    for (uint8_t z = 0; z < ZONES; z++) {
      const ZoneSettings& zs = _cfg->d.zone[z];
      if (_doneDay[z] == day || !zs.enabled) continue;
      if (minute < zs.startMin) continue;

      if (minute >= zs.endMin) {                  // window over and it never ran
        skip(z, day, _lastBlock[z] == Block::NONE ? "WINDOW_MISSED" : blockName(_lastBlock[z]));
        continue;
      }

      if (!_pend[z]) {                            // job becomes due
        _pend[z] = true;
        _pendT[z] = millis();
        _lastBlock[z] = Block::NONE;
      }
      if (elapsed(_pendT[z], QUEUE_TIMEOUT_S * 1000UL)) { skip(z, day, "QUEUE_TIMEOUT"); continue; }

      if (_water->busy() || started) continue;    // one zone at a time, lowest zone first

      Block b = zoneBlock(z, gb);
      _lastBlock[z] = b;
      if (b != Block::NONE) continue;             // retry on the next tick until the window ends

      StartResult r = _water->start(z, zs.runS);
      if (r == StartResult::OK) {
        beginRun(z, "SCHEDULE", zs.runS);
        setDone(z, day, nullptr);
        started = true;
      } else if (r != StartResult::BUSY && r != StartResult::TANK_LOW) {
        skip(z, day, resultName(r));              // daily limit, fault: waiting will not help
      }
    }
  }

  // "Water now" (button, serial, later the web UI). Refused while all off / winter / frost;
  // below the zone's cold limit it only warns, as designed. Low tank, daily cap etc. are answered by WaterController.
  StartResult startManual(uint8_t zone, uint32_t seconds) {
    if (zone >= ZONES) return StartResult::BAD_ZONE;
    Block gb = globalBlock();
    if (gb == Block::ALL_OFF || gb == Block::WINTER || gb == Block::FROST) {
      Serial.printf("SCHED: Water now refused (%s)\n", blockName(gb));
      return StartResult::BLOCKED;
    }
    float t = _env->temperatureC();
    if (!isnan(t) && t < _cfg->d.zone[zone].coldC) Serial.printf("ALERT: %.1f C is below zone %u cold limit, watering anyway\n", t, zone + 1);

    StartResult r = _water->start(zone, seconds);
    if (r == StartResult::OK) beginRun(zone, "MANUAL", min(seconds, MAX_RUN_S));
    return r;
  }

  // ---- time ----
  bool localNow(tm& out) const {
    if (!_rtc->valid()) return false;
    time_t t = _rtc->unixNow();
    localtime_r(&t, &out);
    return true;
  }

  String localTimeStr() const {
    tm lt;
    if (!localNow(lt)) return String("no valid time");
    char b[32];
    strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &lt);
    return String(b) + (lt.tm_isdst > 0 ? " CEST" : " CET");
  }

  // Local wall-clock time to unix UTC (0 on failure)
  static uint32_t localToUnix(int y, int mo, int d, int h, int mi, int s) {
    tm t = {};
    t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d;
    t.tm_hour = h; t.tm_min = mi; t.tm_sec = s;
    t.tm_isdst = -1;
    time_t r = mktime(&t);
    return r < 0 ? 0 : (uint32_t)r;
  }

  bool setLocalTime(int y, int mo, int d, int h, int mi, int s) {
    uint32_t u = localToUnix(y, mo, d, h, mi, s);
    if (u < RTC_MIN_VALID_UNIX) return false;
    _rtc->setUnix(u);
    return true;
  }

  // ---- status ----
  Block globalBlock() const {
    const SettingsData& d = _cfg->d;
    if (d.allOff) return Block::ALL_OFF;
    if (d.winter) return Block::WINTER;
    float t = _env->temperatureC();               // NAN when the BME280 is faulted: the temperature rules are skipped then
    if (!isnan(t) && t < d.frostC) return Block::FROST;
    if (_tank->low()) return Block::TANK_LOW;
    return Block::NONE;
  }

  enum class ZState : uint8_t { ZONE_OFF, DONE, SKIPPED, QUEUED, BLOCKED, WAITING };

  ZState zoneState(uint8_t z) const {
    if (z >= ZONES || !_cfg->d.zone[z].enabled) return ZState::ZONE_OFF;
    if (_day != 0 && _doneDay[z] == _day) return _skipDay[z] == _day ? ZState::SKIPPED : ZState::DONE;
    if (_pend[z]) return _lastBlock[z] != Block::NONE ? ZState::BLOCKED : ZState::QUEUED;
    return ZState::WAITING;
  }

  // Why a zone is BLOCKED (now) or SKIPPED (today), as a code; "" otherwise or when unknown
  const char* zoneWhy(uint8_t z) const {
    switch (zoneState(z)) {
      case ZState::BLOCKED: return blockName(_lastBlock[z]);
      case ZState::SKIPPED: return _skipWhy[z];
      default:              return "";
    }
  }

  String zoneStatus(uint8_t z) const {
    switch (zoneState(z)) {
      case ZState::ZONE_OFF: return String("disabled");
      case ZState::DONE:     return String("done today");
      case ZState::SKIPPED:  return _skipWhy[z][0] ? String("skipped: ") + _skipWhy[z] : String("skipped");
      case ZState::BLOCKED:  return String("waiting: ") + blockName(_lastBlock[z]);
      case ZState::QUEUED:   return String("queued");
      default: break;
    }
    char b[32];
    snprintf(b, sizeof(b), "window opens %02u:%02u", _cfg->d.zone[z].startMin / 60, _cfg->d.zone[z].startMin % 60);
    return String(b);
  }

  // Ambient temperature is below the zone's cold limit (false when the BME280 is faulted)
  bool belowCold(uint8_t z) const {
    float t = _env->temperatureC();
    return z < ZONES && !isnan(t) && t < _cfg->d.zone[z].coldC;
  }

  // The current log for download (invalid File when there is none)
  File openLog() { return _fsOk ? LittleFS.open(LOG_FILE, FILE_READ) : File(); }

  // ---- log ----
  // Non-blocking dump to the serial port: update() sends a few bytes per pass so the safety loop never stalls
  void startDump() {
    if (!_fsOk || _dump) return;
    _dump = LittleFS.open(LOG_FILE, FILE_READ);
    if (!_dump) Serial.println("LOG: empty");
  }

private:
  static constexpr const char* LOG_FILE = "/log.csv";
  static constexpr const char* LOG_OLD  = "/log_old.csv";
  static constexpr const char* LOG_HEADER = "utc,local,zone,trigger,mode,planned_s,pump_s,litres,stop,soil_start_pct,temp_c\n";

  struct Run {
    bool active = false;
    uint8_t zone = 0;
    const char* trigger = "";
    uint32_t plannedS = 0, t0 = 0, startUnix = 0;
    float soil = NAN, temp = NAN;
  };

  static uint32_t dayKey(const tm& t) { return (uint32_t)(t.tm_year + 1900) * 1000UL + t.tm_yday; }
  static const char* key(char prefix, uint8_t z) { static char k[8]; snprintf(k, sizeof(k), "%c%u", prefix, (unsigned)z); return k; }

  Block zoneBlock(uint8_t z, Block gb) const {
    if (gb != Block::NONE) return gb;
    const ZoneSettings& zs = _cfg->d.zone[z];
    if (!zs.enabled) return Block::ZONE_OFF;
    float t = _env->temperatureC();
    if (!isnan(t) && t < zs.coldC) return Block::COLD;
    bool covered = zs.coveredUntil != 0 && _rtc->unixNow() < zs.coveredUntil;
    if (_rain->wet() && !covered) return Block::RAIN;
    return Block::NONE;
  }

  // The zone is handled for this day: it ran (skipReason == nullptr) or was skipped. Both are kept in NVS.
  void setDone(uint8_t z, uint32_t day, const char* skipReason) {
    _doneDay[z] = day;
    _pend[z] = false;
    _skipDay[z] = skipReason ? day : 0;
    snprintf(_skipWhy[z], sizeof(_skipWhy[z]), "%s", skipReason ? skipReason : "");
    Preferences p;
    if (p.begin("state", false)) {
      p.putUInt(key('d', z), day);
      p.putUInt(key('k', z), _skipDay[z]);
      p.end();
    }
  }

  void skip(uint8_t z, uint32_t day, const char* reason) {
    char stop[40];
    snprintf(stop, sizeof(stop), "SKIP_%s", reason);
    logRow(z, "SCHEDULE", _cfg->d.zone[z].runS, 0.0f, stop, _soil[z].percent(), _env->temperatureC(), _rtc->unixNow());
    setDone(z, day, reason);
  }

  void beginRun(uint8_t z, const char* trigger, uint32_t plannedS) {
    _run.active = true;
    _run.zone = z;
    _run.trigger = trigger;
    _run.plannedS = plannedS;
    _run.t0 = millis();
    _run.startUnix = _rtc->unixNow();
    _run.soil = _soil[z].percent();
    _run.temp = _env->temperatureC();
    Serial.printf("SCHED: zone %u started (%s, %lu s)\n", z + 1, trigger, (unsigned long)plannedS);
  }

  // The run is over when WaterController is idle again (after the valve close delay).
  // Pump time = elapsed minus the fixed valve-open and valve-close delays.
  void trackRun() {
    if (!_run.active || _water->busy()) return;
    uint32_t ms = millis() - _run.t0;
    const uint32_t fixedMs = PUMP_START_DELAY_MS + VALVE_CLOSE_DELAY_MS;
    float pumpS = ms > fixedMs ? (ms - fixedMs) / 1000.0f : 0.0f;
    logRow(_run.zone, _run.trigger, _run.plannedS, pumpS, stopName(_water->lastStop()), _run.soil, _run.temp, _run.startUnix);
    _run.active = false;
  }

  void logRow(uint8_t z, const char* trigger, uint32_t plannedS, float pumpS, const char* stop,
              float soilPct, float tempC, uint32_t unixT) {
    static const char* const MODE[] = {"SCHEDULE", "CLOSED_LOOP", "OPEN_LOOP"};
    char local[24] = "", soilS[8] = "", tempS[8] = "", litS[12] = "";
    time_t t = unixT;
    tm lt;
    localtime_r(&t, &lt);
    strftime(local, sizeof(local), "%Y-%m-%d %H:%M:%S", &lt);
    if (!isnan(soilPct)) snprintf(soilS, sizeof(soilS), "%.0f", soilPct);
    if (!isnan(tempC))   snprintf(tempS, sizeof(tempS), "%.1f", tempC);
    if (FLOW_LPM > 0.0f) snprintf(litS, sizeof(litS), "%.2f", pumpS * FLOW_LPM / 60.0f);

    char row[220];
    snprintf(row, sizeof(row), "%lu,%s,%u,%s,%s,%lu,%.1f,%s,%s,%s,%s\n",
             (unsigned long)unixT, local, z + 1, trigger, MODE[_cfg->d.zone[z].mode],
             (unsigned long)plannedS, pumpS, litS, stop, soilS, tempS);
    Serial.print("LOG: ");
    Serial.print(row);
    appendLog(row);
  }

  void appendLog(const char* row) {
    if (!_fsOk) return;
    File f = LittleFS.open(LOG_FILE, FILE_APPEND);
    if (f && f.size() > LOG_MAX_BYTES) {          // rotate: keep one old file
      f.close();
      LittleFS.remove(LOG_OLD);
      LittleFS.rename(LOG_FILE, LOG_OLD);
      f = LittleFS.open(LOG_FILE, FILE_APPEND);
    }
    if (!f) { Serial.println("LOG: cannot open log file"); return; }
    if (f.size() == 0) f.print(LOG_HEADER);
    f.print(row);
    f.close();
  }

  void pumpDump() {
    if (!_dump) return;
    uint8_t buf[64];
    int room = Serial.availableForWrite();
    if (room <= 0) return;
    size_t n = _dump.read(buf, min((size_t)room, sizeof(buf)));
    if (n > 0) Serial.write(buf, n);
    if (!_dump.available()) _dump.close();
  }

  Settings* _cfg = nullptr;
  WaterController* _water = nullptr;
  Clock* _rtc = nullptr;
  TankSensor* _tank = nullptr;
  RainSensor* _rain = nullptr;
  Environment* _env = nullptr;
  SoilSensor* _soil = nullptr;

  uint32_t _day = 0;                              // dayKey of the current local day
  uint32_t _doneDay[ZONES] = {};                  // dayKey the zone was last handled (run or skip), kept in NVS
  uint32_t _skipDay[ZONES] = {};                  // dayKey of the last skip (== _doneDay when that handling was a skip), kept in NVS
  char _skipWhy[ZONES][20] = {};                  // reason of that skip; empty after a reboot (the log has it)
  bool _pend[ZONES] = {};
  uint32_t _pendT[ZONES] = {};
  Block _lastBlock[ZONES] = {};
  uint32_t _tTick = 0;
  bool _fsOk = false;
  Run _run;
  File _dump;
};
