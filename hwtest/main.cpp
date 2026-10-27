// Hardware bench test.
// Run: pio run -e hwtest -t upload && pio device monitor
// Type "help" in the serial monitor (115200 baud, send with newline).
#include <Arduino.h>
#include <esp_system.h>
#include "Hardware.h"

SoilSensor soil[ZONES];
TankSensor tank;
RainSensor rain;
Environment env;
Clock      rtc;
Button     button;
ErrorLed   errLed;
Buzzer     buzzer;
Valve      valve[ZONES];
Pump       pump;

static uint32_t valveEnd[ZONES] = {};   // millis() deadline for each valve, 0 = no auto-off armed
static uint32_t pumpEnd = 0;            // same for the pump
static bool pumpDry  = false;           // dry-run relay test: ignore the tank
static bool live     = false;           // print status every 2 s
static bool ledOn    = false;
static bool lastRain = false;
static uint32_t tSoil = 0, tLive = 0;
static String line;                     // serial command being typed

static bool due(uint32_t end) { return end && (int32_t)(millis() - end) >= 0; }

static const char* resetName() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "panic (crash)";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_BROWNOUT: return "BROWNOUT (supply dipped)";
    default:               return "other";
  }
}

static void allOff() {
  pump.off();
  for (auto& v : valve) v.off();
  pumpEnd = 0;
  pumpDry = false;
  for (auto& e : valveEnd) e = 0;
}

// Sensors, timed auto-off, tank interlock, event printing. Call often.
static void poll() {
  tank.update();
  rain.update();
  env.update();
  rtc.update();
  if (elapsed(tSoil, 500)) {
    tSoil = millis();
    for (auto& s : soil) s.update();
  }

  pump.update();
  for (auto& v : valve) v.update();

  if (pump.isOn() && !pumpDry && tank.low()) {
    pump.off();
    Serial.println("!! tank LOW: pump stopped");
  }
  if (pump.isOn() && due(pumpEnd)) {
    pump.off();
    pumpEnd = 0;
    pumpDry = false;
    Serial.println("pump off");
  }
  for (uint8_t i = 0; i < ZONES; i++) {
    if (valve[i].isOn() && due(valveEnd[i])) {
      valve[i].off();
      valveEnd[i] = 0;
      Serial.printf("valve %u off\n", i + 1);
    }
  }

  if (rain.wet() != lastRain) {
    lastRain = rain.wet();
    Serial.printf("rain sensor: %s\n", lastRain ? "WET" : "dry");
  }

  ButtonEvent be = button.update();
  if (be == ButtonEvent::SHORT) { Serial.println("button: SHORT"); buzzer.beep(1); }
  if (be == ButtonEvent::LONG)  { Serial.println("button: LONG");  buzzer.beep(2); }

  buzzer.update();
}

// Wait while polling. Returns false on 's' from serial or, if needWater, when the tank is LOW.
static bool waitMs(uint32_t ms, bool needWater) {
  uint32_t t0 = millis();
  while (!elapsed(t0, ms)) {
    poll();
    while (Serial.available()) {
      if (Serial.read() == 's') {
        Serial.println("aborted");
        return false;
      }
    }
    if (needWater && tank.low()) {
      Serial.println("!! tank LOW: abort");
      return false;
    }
    delay(2);
  }
  return true;
}

static void status() {
  bool anyFault = pump.fault();
  for (auto& v : valve) anyFault |= v.fault();

  Serial.printf("tank:%s %.1fcm | rain:%s | T:%.1f H:%.0f P:%.0f%s | rtc:%s %lu | pump:%d valves:%d%d%d%d | faults:%d\n",
                tank.low() ? "LOW" : "ok", tank.distanceCm(), rain.wet() ? "WET" : "dry",
                env.temperatureC(), env.humidity(), env.pressureHpa(), env.valid() ? "" : " (BME FAULT)",
                rtc.valid() ? "ok" : "BAD", (unsigned long)rtc.unixNow(),
                pump.isOn(), valve[0].isOn(), valve[1].isOn(), valve[2].isOn(), valve[3].isOn(),
                anyFault);
  for (uint8_t i = 0; i < ZONES; i++) {
    Serial.printf("  soil%u raw:%d %.0f%%%s\n", i + 1, soil[i].raw(), soil[i].percent(), soil[i].fault() ? " FAULT" : "");
  }
}

static void i2cScan() {
  Serial.print("I2C devices:");
  uint8_t found = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf(" 0x%02X", a);
      found++;
    }
  }
  Serial.println(found ? " (BME280 should be 0x76 or 0x77)" : " none (check SDA/SCL, pull-ups, power)");
}

// Average the soil readings for 10 s. Run once in dry air and once in water, then copy the values into Config.h.
static void calibrate() {
  Serial.println("Sampling soil sensors for 10 s...");
  int mn[ZONES], mx[ZONES];
  uint32_t sum[ZONES] = {};
  uint16_t n = 0;
  for (uint8_t i = 0; i < ZONES; i++) {
    mn[i] = 9999;
    mx[i] = 0;
  }

  uint32_t t0 = millis();
  while (!elapsed(t0, 10000)) {
    poll();
    for (uint8_t i = 0; i < ZONES; i++) {
      soil[i].update();
      int r = soil[i].raw();
      mn[i] = min(mn[i], r);
      mx[i] = max(mx[i], r);
      sum[i] += r;
    }
    n++;
    delay(200);
  }

  for (uint8_t i = 0; i < ZONES; i++) {
    Serial.printf("  soil%u avg:%lu min:%d max:%d\n", i + 1, (unsigned long)(sum[i] / n), mn[i], mx[i]);
  }
}

// Real valve + pump order with the interlock delays, one zone after another. Needs water in the tank.
static void sequence() {
  if (tank.low()) {
    Serial.println("refused: tank LOW (needs water, or check the HC-SR04)");
    return;
  }
  allOff();
  for (uint8_t z = 0; z < ZONES; z++) {
    Serial.printf("zone %u: valve open\n", z + 1);
    valve[z].on();
    bool ok = waitMs(PUMP_START_DELAY_MS, true);
    if (ok) {
      pump.on();
      Serial.println("  pump on 3 s");
      ok = waitMs(3000, true);
    }
    pump.off();
    waitMs(VALVE_CLOSE_DELAY_MS, false);
    valve[z].off();
    if (!ok) {
      allOff();
      return;
    }
    waitMs(1000, false);
  }
  allOff();
  Serial.println("sequence done");
}

static void selfTest() {
  Serial.println("--- self test ---");
  waitMs(1600, false);   // let the tank sensor take a few readings

  for (uint8_t i = 0; i < ZONES; i++) {
    soil[i].update();
    Serial.printf("soil%u raw %d  %s\n", i + 1, soil[i].raw(), soil[i].fault() ? "FAIL (disconnected/shorted?)" : "PASS");
  }

  float d = tank.distanceCm();
  bool tankOk = !isnan(d) && d >= TANK_MIN_VALID_CM && d <= TANK_MAX_VALID_CM;
  Serial.printf("tank echo %.1f cm  %s, level %s\n", d,
                tankOk ? "PASS" : "FAIL (no echo: wiring/divider/power)", tank.low() ? "LOW" : "ok");

  env.begin();
  delay(50);
  env.update();
  Serial.printf("BME280 T:%.1f H:%.0f P:%.0f  %s\n",
                env.temperatureC(), env.humidity(), env.pressureHpa(), env.valid() ? "PASS" : "FAIL");

  uint32_t a = rtc.unixNow();
  waitMs(2100, false);
  uint32_t b = rtc.unixNow();
  bool ticking = (b - a) >= 1 && (b - a) <= 3;
  Serial.printf("RTC %lu valid:%s ticking:%s  %s\n", (unsigned long)b,
                rtc.valid() ? "yes" : "NO", ticking ? "yes" : "NO",
                (rtc.valid() && ticking) ? "PASS" : "FAIL (set with: t <unix>)");

  Serial.printf("rain sensor reads %s: drop water on it and watch for the WET message\n", rain.wet() ? "WET" : "dry");
  Serial.println("Manual: button (press/hold), valves 1-4, p, beep, led. Relays must click, nothing else may.");
}

static void help() {
  Serial.println(
    "Commands:\n"
    "  all      self test of the sensors\n"
    "  st       print status once;  live = toggle status every 2 s\n"
    "  1-4      open valve N for 3 s (relay click test, no pump)\n"
    "  p        pump relay for 2 s (DRY RUN, ignores tank)\n"
    "  seq      each zone: valve, 1.5 s, pump 3 s, pump off, 1.5 s, valve (needs water)\n"
    "  s        stop everything now\n"
    "  x        clear output faults\n"
    "  cal      average soil readings for 10 s (dry air, then water)\n"
    "  scan     I2C scan\n"
    "  beep, led   buzzer and error LED tests\n"
    "  t        show RTC time;  t <unix> = set RTC (UTC)\n"
    "Button presses and rain changes are printed automatically.");
}

static void handle(String s) {
  s.trim();
  s.toLowerCase();

  if (s == "help" || s == "?") {
    help();
  } else if (s == "all") {
    selfTest();
  } else if (s == "st") {
    status();
  } else if (s == "live") {
    live = !live;
    Serial.println(live ? "live on" : "live off");
  } else if (s.length() == 1 && s[0] >= '1' && s[0] < '1' + ZONES) {
    uint8_t i = s[0] - '1';
    if (valve[i].on()) {
      valveEnd[i] = millis() + 3000;
      Serial.printf("valve %u ON 3 s\n", i + 1);
    } else {
      Serial.println("valve fault: send x");
    }
  } else if (s == "p") {
    pumpDry = true;
    if (pump.on()) {
      pumpEnd = millis() + 2000;
      Serial.println("pump relay ON 2 s (dry run!)");
    } else {
      Serial.println("pump fault: send x");
    }
  } else if (s == "seq") {
    sequence();
  } else if (s == "s") {
    allOff();
    Serial.println("all off");
  } else if (s == "x") {
    pump.clearFault();
    for (auto& v : valve) v.clearFault();
    Serial.println("faults cleared");
  } else if (s == "cal") {
    calibrate();
  } else if (s == "scan") {
    i2cScan();
  } else if (s == "beep") {
    buzzer.beep(3);
  } else if (s == "led") {
    ledOn = !ledOn;
    errLed.set(ledOn);
    Serial.println(ledOn ? "LED on" : "LED off");
  } else if (s[0] == 't') {
    String arg = s.substring(1);
    arg.trim();
    if (arg.length()) rtc.setUnix(strtoul(arg.c_str(), nullptr, 10));
    Serial.printf("rtc: %lu (valid:%s)\n", (unsigned long)rtc.unixNow(), rtc.valid() ? "yes" : "NO");
  } else {
    Serial.println("unknown command, try: help");
  }
}

void setup() {
  // Outputs to the safe state first
  pump.begin(PIN_PUMP, 15000);
  for (uint8_t i = 0; i < ZONES; i++) valve[i].begin(PIN_VALVE[i], 15000);

  Serial.begin(115200);
  delay(300);
  Serial.printf("\n=== HARDWARE TEST === reset reason: %s\n", resetName());

  for (uint8_t i = 0; i < ZONES; i++) soil[i].begin(PIN_SOIL[i], SOIL_DRY_RAW[i], SOIL_WET_RAW[i]);
  tank.begin(PIN_TANK_TRIG, PIN_TANK_ECHO);
  rain.begin(PIN_RAIN_OUT, PIN_RAIN_VCC);
  env.begin();
  rtc.begin();
  button.begin(PIN_BUTTON);
  errLed.begin(PIN_ERROR_LED);
  buzzer.begin(PIN_BUZZER);
  lastRain = rain.wet();
  help();
}

void loop() {
  poll();

  // Collect a line from the serial monitor, run it on newline
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r' || c == '\n') {
      if (line.length()) {
        handle(line);
        line = "";
      }
    } else if (line.length() < 40) {
      line += c;
    }
  }

  if (live && elapsed(tLive, 2000)) {
    tLive = millis();
    status();
  }
}
