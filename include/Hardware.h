#pragma once
// One small class per piece of hardware, plus WaterController, the only code
// allowed to switch the pump and valves.
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_BME280.h>
#include <ThreeWire.h>
#include <RtcDS1302.h>
#include "PINS.h"
#include "Config.h"

// Changing ZONES breaks the build until PINS.h and Config.h have one entry per zone
static_assert(sizeof(PIN_SOIL) == ZONES && sizeof(PIN_VALVE) == ZONES,
              "PINS.h needs one soil and one valve pin per zone");
static_assert(sizeof(SOIL_DRY_RAW) / sizeof(int) == ZONES && sizeof(SOIL_WET_RAW) / sizeof(int) == ZONES,
              "Config.h needs one soil calibration per zone");

// True once spanMs has passed since 'since' (safe across the millis() rollover)
inline bool elapsed(uint32_t since, uint32_t spanMs) {
  return (uint32_t)(millis() - since) >= spanMs;
}

// ----------------------------------------------------------------------
// Relay output with its own on-time cap, a backstop under the controller.
// If the cap trips, the output is forced off and stays off (latched) until clearFault().
// ----------------------------------------------------------------------
class TimedOutput {
public:
  void begin(uint8_t pin, uint32_t hardCapMs) {
    _pin = pin;
    _cap = hardCapMs;
    digitalWrite(_pin, offLevel());   // set the OFF level before the pin becomes an output
    pinMode(_pin, OUTPUT);
    digitalWrite(_pin, offLevel());
    _on = false;
    _fault = false;
  }

  // Returns false (and does nothing) while the output is latched in fault
  bool on() {
    if (_fault) return false;
    if (!_on) {
      _since = millis();
      _on = true;
      digitalWrite(_pin, onLevel());
    }
    return true;
  }

  void off() {
    _on = false;
    digitalWrite(_pin, offLevel());
  }

  bool isOn() const { return _on; }
  bool fault() const { return _fault; }
  void clearFault() { _fault = false; }

  void update() {
    if (_on && elapsed(_since, _cap)) {
      off();
      _fault = true;
    }
  }

private:
  uint8_t onLevel() const  { return RELAY_ACTIVE_LOW ? LOW : HIGH; }
  uint8_t offLevel() const { return RELAY_ACTIVE_LOW ? HIGH : LOW; }

  uint8_t _pin = 0;
  uint32_t _cap = 0, _since = 0;
  bool _on = false, _fault = false;
};

using Valve = TimedOutput;
using Pump  = TimedOutput;

// ----------------------------------------------------------------------
// Soil sensor HW-390 (capacitive, analog)
// ----------------------------------------------------------------------
class SoilSensor {
public:
  void begin(uint8_t pin, int dryRaw, int wetRaw) {
    _pin = pin;
    _dry = dryRaw;
    _wet = wetRaw;
    pinMode(_pin, INPUT);
    analogSetPinAttenuation(_pin, ADC_11db);
  }

  // Average of 16 reads
  void update() {
    uint32_t sum = 0;
    for (uint8_t i = 0; i < 16; i++) {
      sum += analogRead(_pin);
      delayMicroseconds(200);
    }
    _raw = sum / 16;
    _fault = (_raw < SOIL_RAW_MIN_OK || _raw > SOIL_RAW_MAX_OK);
  }

  int raw() const { return _raw; }
  bool fault() const { return _fault; }

  // 0..100 %, NAN on fault
  float percent() const {
    if (_fault) return NAN;
    float p = 100.0f * (float)(_dry - _raw) / (float)(_dry - _wet);
    return constrain(p, 0.0f, 100.0f);
  }

private:
  uint8_t _pin = 0;
  int _dry = 2800, _wet = 1200, _raw = 0;
  bool _fault = true;
};

// ----------------------------------------------------------------------
// Tank level, HC-SR04 looking down at the water.
// Fail-safe: starts LOW, and no echo or an implausible reading counts as LOW.
// ----------------------------------------------------------------------
class TankSensor {
public:
  static constexpr float CM_PER_US = 0.0343f;   // speed of sound at about 20 C

  void begin(uint8_t trig, uint8_t echo) {
    _trig = trig;
    _echo = echo;
    pinMode(_trig, OUTPUT);
    digitalWrite(_trig, LOW);
    pinMode(_echo, INPUT);
    _low = true;
  }

  void update() {
    if (!elapsed(_last, TANK_PERIOD_MS)) return;
    _last = millis();
    _dist = measureCm();

    bool echoOk = !isnan(_dist) && _dist >= TANK_MIN_VALID_CM && _dist <= TANK_MAX_VALID_CM;
    bool good   = echoOk && _dist <= TANK_LOW_DIST_CM;

    // Count consecutive readings so one bad echo does not flip the state
    if (good) {
      if (_good < 255) _good++;
      _bad = 0;
    } else {
      if (_bad < 255) _bad++;
      _good = 0;
    }
    if (_bad >= TANK_BAD_TO_LOW) _low = true;
    else if (_good >= TANK_GOOD_TO_OK) _low = false;
  }

  bool low() const { return _low; }
  float distanceCm() const { return _dist; }   // NAN = no echo

private:
  float measureCm() {
    digitalWrite(_trig, LOW);
    delayMicroseconds(2);
    digitalWrite(_trig, HIGH);
    delayMicroseconds(10);
    digitalWrite(_trig, LOW);
    // Wait only as long as the farthest valid distance needs (about 13 ms)
    unsigned long us = pulseIn(_echo, HIGH, (uint32_t)(TANK_MAX_VALID_CM * 2.0f / CM_PER_US) + 1000);
    if (us == 0) return NAN;
    return us * CM_PER_US / 2.0f;
  }

  uint8_t _trig = 0, _echo = 0, _good = 0, _bad = 0;
  uint32_t _last = 0;
  float _dist = NAN;
  bool _low = true;
};

// ----------------------------------------------------------------------
// Rain sensor SA-28 (digital, debounced). Its VCC is switched from a GPIO.
// ----------------------------------------------------------------------
class RainSensor {
public:
  void begin(uint8_t outPin, uint8_t vccPin) {
    _pin = outPin;
    _vcc = vccPin;
    pinMode(_vcc, OUTPUT);
    digitalWrite(_vcc, HIGH);
    pinMode(_pin, INPUT_PULLUP);       // idle = HIGH = dry, so an unplugged sensor also reads dry
    delay(RAIN_POWERUP_MS);            // let the module settle before the first read
    _raw = _state = readWet();
    _t = millis();
  }

  void update() {
    bool r = readWet();
    if (r != _raw) {                   // changed: restart the debounce timer
      _raw = r;
      _t = millis();
    } else if (_state != _raw && elapsed(_t, RAIN_DEBOUNCE_MS)) {
      _state = _raw;
    }
  }

  bool wet() const { return _state; }

private:
  bool readWet() const { return digitalRead(_pin) == LOW; }   // DO goes LOW when wet

  uint8_t _pin = 0, _vcc = 0;
  bool _raw = false, _state = false;
  uint32_t _t = 0;
};

// ----------------------------------------------------------------------
// BME280: temperature, humidity, pressure. The getters return NAN when faulted.
// ----------------------------------------------------------------------
class Environment {
public:
  void begin() {
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    _ok = _bme.begin(0x76, &Wire) || _bme.begin(0x77, &Wire);
    if (_ok) {
      // 1x oversampling and a 1 s standby: low self-heating and no blocking wait
      _bme.setSampling(Adafruit_BME280::MODE_NORMAL,
                       Adafruit_BME280::SAMPLING_X1, Adafruit_BME280::SAMPLING_X1, Adafruit_BME280::SAMPLING_X1,
                       Adafruit_BME280::FILTER_OFF, Adafruit_BME280::STANDBY_MS_1000);
    }
    _fails = 0;
    _last = millis() - ENV_PERIOD_MS;   // first update() reads at once
  }

  void update() {
    if (!elapsed(_last, ENV_PERIOD_MS)) return;
    _last = millis();
    if (!_ok) {                         // faulted: try to start it again
      begin();
      if (!_ok) return;
    }

    float t = _bme.readTemperature();
    float h = _bme.readHumidity();
    float p = _bme.readPressure() / 100.0f;
    bool good = !isnan(t) && !isnan(h) && !isnan(p) && t > -40 && t < 85 && h >= 0 && h <= 100;
    if (good) {
      _t = t;
      _h = h;
      _p = p;
      _fails = 0;
    } else if (++_fails >= ENV_FAULT_COUNT) {
      _ok = false;                      // re-init on the next period
    }
  }

  bool valid() const { return _ok; }
  float temperatureC() const { return _ok ? _t : NAN; }
  float humidity() const { return _ok ? _h : NAN; }
  float pressureHpa() const { return _ok ? _p : NAN; }

private:
  Adafruit_BME280 _bme;
  bool _ok = false;
  uint8_t _fails = 0;
  float _t = NAN, _h = NAN, _p = NAN;
  uint32_t _last = 0;
};

// ----------------------------------------------------------------------
// DS1302 real-time clock, kept in UTC
// ----------------------------------------------------------------------
class Clock {
public:
  Clock() : _wire(PIN_RTC_DAT, PIN_RTC_CLK, PIN_RTC_RST), _rtc(_wire) {}

  void begin() {
    _rtc.Begin();
    if (_rtc.GetIsWriteProtected()) _rtc.SetIsWriteProtected(false);
    if (!_rtc.GetIsRunning()) _rtc.SetIsRunning(true);
    refresh();
  }

  // The DS1302 is bit-banged, so read it only once a second
  void update() {
    if (elapsed(_t, 1000)) refresh();
  }

  bool valid() const { return _valid; }
  uint32_t unixNow() const { return _unix; }   // up to 1 s old

  void setUnix(uint32_t t) {
    RtcDateTime dt;
    dt.InitWithUnix32Time(t);
    _rtc.SetDateTime(dt);
    refresh();
  }

private:
  void refresh() {
    _t = millis();
    _unix = _rtc.GetDateTime().Unix32Time();
    _valid = _rtc.IsDateTimeValid() && _unix >= RTC_MIN_VALID_UNIX;
  }

  ThreeWire _wire;
  RtcDS1302<ThreeWire> _rtc;
  uint32_t _unix = 0, _t = 0;
  bool _valid = false;
};

// ----------------------------------------------------------------------
// Button: debounced. SHORT is sent on release, LONG once while still held.
// The PCB has a 1k pull-down and the button goes to 3.3 V, so pressed = HIGH.
// ----------------------------------------------------------------------
enum class ButtonEvent : uint8_t { NONE, SHORT, LONG };

class Button {
public:
  void begin(uint8_t pin) {
    _pin = pin;
#ifdef BENCH_BUTTON_GPIO2
    // Bench wiring (bare ESP32, no PCB): button between the button pin and GPIO2,
    // GPIO2 driven LOW as a ground. Idle = HIGH (pull-up), pressed = LOW.
    pinMode(2, OUTPUT);
    digitalWrite(2, LOW);
    pinMode(_pin, INPUT_PULLUP);
    _activeLow = true;
#else
    pinMode(_pin, INPUT);              // PCB: 1k pull-down, button to 3.3 V, pressed = HIGH
#endif
  }

  bool pressed() const { return (digitalRead(_pin) == HIGH) != _activeLow; }   // raw, not debounced

  ButtonEvent update() {
    bool down = pressed();
    if (down != _raw) {                                  // level changed: restart the debounce timer
      _raw = down;
      _t = millis();
      return ButtonEvent::NONE;
    }
    if (!elapsed(_t, BUTTON_DEBOUNCE_MS)) return ButtonEvent::NONE;   // not stable yet

    if (down && !_down) {                                // press confirmed
      _down = true;
      _longSent = false;
      _pressT = millis();
    } else if (down && !_longSent && elapsed(_pressT, BUTTON_LONG_MS)) {
      _longSent = true;
      return ButtonEvent::LONG;
    } else if (!down && _down) {                         // released
      _down = false;
      if (!_longSent) return ButtonEvent::SHORT;
    }
    return ButtonEvent::NONE;
  }

private:
  uint8_t _pin = 0;
  bool _raw = false, _down = false, _longSent = false, _activeLow = false;
  uint32_t _t = 0, _pressT = 0;
};

// ----------------------------------------------------------------------
// Error LED and buzzer
// ----------------------------------------------------------------------
class ErrorLed {
public:
  void begin(uint8_t pin) {
    _pin = pin;
    pinMode(_pin, OUTPUT);
    digitalWrite(_pin, LOW);
  }
  void set(bool on) { digitalWrite(_pin, on); }
  void update() {}   // src/main.cpp calls this; nothing to do yet (placeholder for blinking)

private:
  uint8_t _pin = 0;
};

// Active buzzer, non-blocking: beep(n) queues n beeps, update() plays them
class Buzzer {
public:
  void begin(uint8_t pin) {
    _pin = pin;
    pinMode(_pin, OUTPUT);
    digitalWrite(_pin, LOW);
  }

  void beep(uint8_t count) {
    _left = count * 2;               // one beep = an on step and an off step
    _t = millis() - 100;             // start at once
  }

  void update() {
    if (!_left || !elapsed(_t, 100)) return;
    _t = millis();
    _left--;
    digitalWrite(_pin, (_left % 2) ? HIGH : LOW);
  }

private:
  uint8_t _pin = 0, _left = 0;
  uint32_t _t = 0;
};

// ----------------------------------------------------------------------
// WaterController
// Interlocks: one zone at a time, valve opens before the pump starts, pump stops
// before the valve closes, tank-low stop, max run time, per-zone daily cap,
// everything off at boot, stray outputs forced off while idle.
// ----------------------------------------------------------------------
// BLOCKED is never returned by WaterController itself: the Scheduler uses it for "Water now" refused (all off, winter, frost)
enum class StartResult : uint8_t { OK, BUSY, BAD_ZONE, TANK_LOW, TOO_SHORT, DAILY_LIMIT, VALVE_FAULT, PUMP_FAULT, BLOCKED };
enum class StopReason  : uint8_t { NONE, DONE, USER, TANK_LOW, FAULT };

inline const char* resultName(StartResult r) {
  switch (r) {
    case StartResult::OK:          return "OK";
    case StartResult::BUSY:        return "BUSY";
    case StartResult::BAD_ZONE:    return "BAD_ZONE";
    case StartResult::TANK_LOW:    return "TANK_LOW";
    case StartResult::TOO_SHORT:   return "TOO_SHORT";
    case StartResult::DAILY_LIMIT: return "DAILY_LIMIT";
    case StartResult::VALVE_FAULT: return "VALVE_FAULT";
    case StartResult::PUMP_FAULT:  return "PUMP_FAULT";
    default:                       return "BLOCKED";
  }
}

inline const char* stopName(StopReason r) {
  switch (r) {
    case StopReason::DONE:     return "DONE";
    case StopReason::USER:     return "USER";
    case StopReason::TANK_LOW: return "TANK_LOW";
    case StopReason::FAULT:    return "FAULT";
    default:                   return "NONE";
  }
}

class WaterController {
public:
  void begin(TankSensor* tank) {
    _tank = tank;
    const uint32_t capMs = MAX_RUN_S * 1000UL + HARD_CAP_MARGIN_MS;
    _pump.begin(PIN_PUMP, capMs);    // pump first
    for (uint8_t i = 0; i < ZONES; i++) _valve[i].begin(PIN_VALVE[i], capMs);
  }

  StartResult start(uint8_t zone, uint32_t seconds) {
    if (zone >= ZONES) return StartResult::BAD_ZONE;
    if (_state != State::IDLE) return StartResult::BUSY;
    if (_tank->low()) return StartResult::TANK_LOW;
    if (seconds < MIN_RUN_S) return StartResult::TOO_SHORT;
    if (_pump.fault()) return StartResult::PUMP_FAULT;
    if (_valve[zone].fault()) return StartResult::VALVE_FAULT;
    if (_today[zone] >= MAX_ZONE_DAY_S) return StartResult::DAILY_LIMIT;

    seconds = min(seconds, MAX_RUN_S);
    seconds = min(seconds, MAX_ZONE_DAY_S - _today[zone]);   // trim to what is left of today's budget
    if (!_valve[zone].on()) return StartResult::VALVE_FAULT;

    _zone = zone;
    _runMs = seconds * 1000UL;
    _t0 = millis();
    _state = State::VALVE_OPEN;
    _last = StopReason::NONE;
    return StartResult::OK;
  }

  // Pump off now; update() closes the valve after VALVE_CLOSE_DELAY_MS
  void stop(StopReason reason = StopReason::USER) {
    if (_state == State::IDLE || _state == State::STOPPING) return;
    _pump.off();
    _today[_zone] += (millis() - _t0) / 1000UL;
    _last = reason;
    _tStop = millis();
    _state = State::STOPPING;
  }

  // Call every loop pass
  void update() {
    _pump.update();
    for (uint8_t i = 0; i < ZONES; i++) _valve[i].update();

    // A tripped on-time cap ends the run
    bool running = _state != State::IDLE && _state != State::STOPPING;
    if (running && (_pump.fault() || _valve[_zone].fault())) stop(StopReason::FAULT);

    switch (_state) {
      case State::IDLE:
        // Nothing may be on while idle
        if (_pump.isOn()) _pump.off();
        for (uint8_t i = 0; i < ZONES; i++) {
          if (_valve[i].isOn()) _valve[i].off();
        }
        break;

      case State::VALVE_OPEN:
        if (_tank->low()) {
          stop(StopReason::TANK_LOW);
        } else if (elapsed(_t0, PUMP_START_DELAY_MS)) {
          if (_pump.on()) _state = State::RUNNING;
          else stop(StopReason::FAULT);
        }
        break;

      case State::RUNNING:
        if (_tank->low()) stop(StopReason::TANK_LOW);
        else if (elapsed(_t0, _runMs)) stop(StopReason::DONE);
        break;

      case State::STOPPING:
        if (elapsed(_tStop, VALVE_CLOSE_DELAY_MS)) {
          _valve[_zone].off();
          _state = State::IDLE;
        }
        break;
    }
  }

  bool busy() const { return _state != State::IDLE; }
  int8_t activeZone() const { return busy() ? (int8_t)_zone : -1; }
  uint32_t runElapsedS() const { return busy() ? (millis() - _t0) / 1000UL : 0; }   // read-only, for the UI progress bar
  uint32_t runTotalS() const { return busy() ? _runMs / 1000UL : 0; }
  StopReason lastStop() const { return _last; }
  uint32_t zoneSecondsToday(uint8_t z) const { return z < ZONES ? _today[z] : 0; }
  void resetDaily() { for (uint8_t i = 0; i < ZONES; i++) _today[i] = 0; }
  bool pumpOn() const { return _pump.isOn(); }
  bool valveOn(uint8_t z) const { return z < ZONES && _valve[z].isOn(); }

  void clearFaults() {
    _pump.clearFault();
    for (uint8_t i = 0; i < ZONES; i++) _valve[i].clearFault();
  }

  bool faulted() const {
    bool any = _pump.fault();
    for (uint8_t i = 0; i < ZONES; i++) any |= _valve[i].fault();
    return any;
  }

private:
  enum class State : uint8_t { IDLE, VALVE_OPEN, RUNNING, STOPPING };

  TankSensor* _tank = nullptr;
  Pump _pump;
  Valve _valve[ZONES];
  State _state = State::IDLE;
  uint8_t _zone = 0;
  StopReason _last = StopReason::NONE;
  uint32_t _t0 = 0, _tStop = 0, _runMs = 0;
  uint32_t _today[ZONES] = {};   // seconds watered today, per zone
};
