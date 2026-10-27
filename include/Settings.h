#pragma once
// Runtime settings, stored in NVS.
// Config.h holds the compile-time ceilings (MAX_RUN_S, MAX_ZONE_DAY_S, ...). Every value here is
// clamped to them on load and before every save, so a bad write (web page, flash corruption)
// can only make watering more conservative, never exceed the caps.
#include <Arduino.h>
#include <Preferences.h>
#include <string.h>
#include <math.h>
#include "Config.h"

static_assert(MAX_RUN_S <= 65535, "ZoneSettings::runS is 16 bit");

enum class ZoneMode : uint8_t { SCHEDULE, CLOSED_LOOP, OPEN_LOOP };

struct ZoneSettings {
  uint8_t  mode;           // ZoneMode (only SCHEDULE is implemented; the others fall back to it)
  uint8_t  enabled;        // 0/1
  int8_t   coldC;          // per-zone cold limit, ambient C: below it the zone is not watered by the scheduler
  uint8_t  reserved;
  uint16_t runS;           // run length per day
  uint16_t startMin;       // start window, local minutes since midnight
  uint16_t endMin;         // exclusive
  uint32_t coveredUntil;   // unix UTC: covered plants get no rain credit (rain ban ignored) until then; 0 = not covered
};

struct SettingsData {
  uint16_t version;
  uint8_t  allOff;         // 1 = nothing is watered, "Water now" refused
  uint8_t  winter;         // 1 = system drained for winter: same effect as allOff, different UI text
  float    frostC;         // global frost stop
  ZoneSettings zone[ZONES];
};

class Settings {
public:
  SettingsData d;

  // Load from NVS; first boot or an unknown layout starts from the Config.h defaults
  void begin() {
    if (!load()) {
      setDefaults(d);
      _saved = d;
      save();
    }
  }

  // Clamp, then write only if something changed (flash wear). False if the write did not verify.
  bool save() {
    clamp(d);
    if (memcmp(&d, &_saved, sizeof(d)) == 0) return true;
    Preferences p;
    if (!p.begin("cfg", false)) return false;
    p.putBytes("d", &d, sizeof(d));
    SettingsData check;
    bool ok = p.getBytes("d", &check, sizeof(check)) == sizeof(check) && memcmp(&check, &d, sizeof(d)) == 0;
    p.end();
    if (ok) _saved = d;
    return ok;
  }

  static void setDefaults(SettingsData& s) {
    memset(&s, 0, sizeof(s));
    s.version = VERSION;
    s.frostC = DEFAULT_FROST_C;
    for (auto& z : s.zone) {
      z.mode = (uint8_t)ZoneMode::SCHEDULE;
      z.enabled = 1;
      z.coldC = DEFAULT_ZONE_COLD_C;
      z.runS = DEFAULT_RUN_S;
      z.startMin = DEFAULT_WINDOW_START_MIN;
      z.endMin = DEFAULT_WINDOW_END_MIN;
      z.coveredUntil = 0;
    }
  }

  static void clamp(SettingsData& s) {
    s.allOff = s.allOff ? 1 : 0;
    s.winter = s.winter ? 1 : 0;
    if (isnan(s.frostC)) s.frostC = DEFAULT_FROST_C;
    if (s.frostC < -10.0f) s.frostC = -10.0f;
    if (s.frostC > 10.0f)  s.frostC = 10.0f;

    for (auto& z : s.zone) {
      if (z.mode > (uint8_t)ZoneMode::OPEN_LOOP) z.mode = (uint8_t)ZoneMode::SCHEDULE;
      z.enabled = z.enabled ? 1 : 0;

      int cold = z.coldC;
      if (cold < -10) cold = -10;
      if (cold > 30)  cold = 30;
      z.coldC = (int8_t)cold;

      uint32_t run = z.runS;
      if (run < MIN_RUN_S) run = MIN_RUN_S;
      if (run > MAX_RUN_S) run = MAX_RUN_S;
      z.runS = (uint16_t)run;

      if (z.endMin > 1440) z.endMin = 1440;
      if (z.startMin >= z.endMin) {            // empty or inverted window: back to the default (no windows over midnight)
        z.startMin = DEFAULT_WINDOW_START_MIN;
        z.endMin = DEFAULT_WINDOW_END_MIN;
      }
    }
  }

private:
  static constexpr uint16_t VERSION = 1;       // bump when SettingsData changes: old data is dropped for the defaults

  bool load() {
    Preferences p;
    if (!p.begin("cfg", false)) return false;
    SettingsData tmp;
    bool ok = p.getBytesLength("d") == sizeof(tmp) &&
              p.getBytes("d", &tmp, sizeof(tmp)) == sizeof(tmp) &&
              tmp.version == VERSION;
    p.end();
    if (!ok) return false;
    clamp(tmp);
    d = tmp;
    _saved = tmp;
    return true;
  }

  SettingsData _saved;
};
