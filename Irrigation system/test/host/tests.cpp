// Host tests: the whole firmware runs against the stub hardware (test/host/stubs).
// Time is simulated: host::ms drives millis() and the RTC.
#include <time.h>
#include <string>
#include <Preferences.h>
#include "System.h"

namespace host {
extern uint32_t ms;
extern int pinLevel[40], analogValue[40];
extern bool bmeOk;
extern float bmeT;
extern uint32_t rtcBase, rtcBaseMs;
extern bool rtcValid;
extern unsigned long pulseUs;
std::map<std::string, std::string>& files();
}  // namespace host

static int fails = 0, checks = 0;
namespace show {
template <typename T> std::string str(const T& v) { return std::to_string(v); }
inline std::string str(const std::string& v) { return v; }
}  // namespace show
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { checks++; auto _a = (a); auto _b = (b); if (!(_a == _b)) { fails++; \
  printf("FAIL %s:%d  %s == %s  (%s vs %s)\n", __FILE__, __LINE__, #a, #b, show::str(_a).c_str(), show::str(_b).c_str()); } } while (0)

static WebServer srv(80);

static void tick(uint32_t msTotal) {
  for (uint32_t t = 0; t < msTotal; t += 10) {
    host::ms += 10;
    sys.loop();
  }
}

// Jump the clock forward without running every 10 ms (long waits); runs a few passes after
static void jump(uint32_t msTotal) {
  host::ms += msTotal;
  tick(2000);
}

static JsonDocument call(HTTPMethod m, const std::string& path, std::map<std::string, std::string> args = {}) {
  std::string body = srv.request(m, path, args);
  JsonDocument d;
  deserializeJson(d, body);
  return d;
}
static JsonDocument post(const std::string& path, std::map<std::string, std::string> args = {}) { return call(HTTP_POST, path, args); }
static JsonDocument state() { return call(HTTP_GET, "/api/state"); }

static bool on(uint8_t pin) { return host::pinLevel[pin] == HIGH; }
static bool logHas(const char* s) { return host::files()["/log.csv"].find(s) != std::string::npos; }
static int rawFor(float pct) { return (int)(2800 - (2800 - 1200) * pct / 100.0f); }   // default calibration

static uint32_t utc(int y, int mo, int d, int h, int mi) {
  tm t{};
  t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d; t.tm_hour = h; t.tm_min = mi;
  return (uint32_t)timegm(&t);
}

// ---------------------------------------------------------------- unit tests
static void testTime() {
  TimeUtil::begin();
  CHECK_EQ(TimeUtil::parseYmd("2026-02-30"), 0u);
  CHECK_EQ(TimeUtil::parseYmd("2026-10-30"), 20261030u);
  CHECK_EQ((int)TimeUtil::weekday(20261030), 4);                 // Friday (0 = Monday)
  CHECK_EQ(TimeUtil::addDays(20261231, 1), 20270101u);
  CHECK_EQ(TimeUtil::addDays(20240301, -1), 20240229u);          // leap year
  CHECK_EQ(TimeUtil::localToUnix(20261030, 600), utc(2026, 10, 30, 9, 0));   // CET = UTC+1
  CHECK_EQ(TimeUtil::localToUnix(20260715, 600), utc(2026, 7, 15, 8, 0));    // CEST = UTC+2
  CHECK_EQ(TimeUtil::periodStart(TimeUtil::Period::WEEK, utc(2026, 10, 30, 9, 0)), utc(2026, 10, 25, 23, 0));   // Monday 26 Oct 00:00 local
  CHECK_EQ(TimeUtil::periodStart(TimeUtil::Period::MONTH, utc(2026, 10, 30, 9, 0)), utc(2026, 9, 30, 22, 0));   // 1 Oct 00:00 CEST
  LTime l;
  CHECK(TimeUtil::toLocal(utc(2026, 10, 30, 9, 30), l) && l.mod == 630 && l.wd == 4);
}

static void testDndWindows() {
  DndList l{};
  l.n = 2;
  l.e[0] = DndWindow{1, (uint8_t)(1 << 4), 1320, 360, 0, 0};             // Fridays 22:00 - 06:00 next day
  l.e[1] = DndWindow{2, 0, 720, 600, 20261102, 20261103};               // one-off: 2 Nov 12:00 - 3 Nov 10:00
  CHECK(Dnd::inWindows(l, TimeUtil::localToUnix(20261030, 23 * 60)));   // Fri 23:00
  CHECK(Dnd::inWindows(l, TimeUtil::localToUnix(20261031, 2 * 60)));    // Sat 02:00, from Friday's window
  CHECK(!Dnd::inWindows(l, TimeUtil::localToUnix(20261031, 7 * 60)));   // Sat 07:00
  CHECK(!Dnd::inWindows(l, TimeUtil::localToUnix(20261030, 21 * 60)));  // Fri 21:00
  CHECK(Dnd::inWindows(l, TimeUtil::localToUnix(20261103, 9 * 60)));    // inside the one-off
  CHECK(!Dnd::inWindows(l, TimeUtil::localToUnix(20261103, 10 * 60)));  // its end is exclusive
}

static void testUtil() {
  char b[97];
  CHECK_EQ(utf8Copy(b, sizeof(b), "Herb bed \xF0\x9F\x8C\xB5", 24), (size_t)10);   // emoji counts as one
  CHECK_EQ(utf8Copy(b, sizeof(b), "abcdefghijklmnopqrstuvwxyz", 24), (size_t)24);
  CHECK(validHostname("kert-1") && !validHostname("-kert") && !validHostname("kért") && !validHostname(""));
  CHECK(fabsf(Weather::hargreavesEt0(6, 14, 47.5f, 303) - 1.2f) < 0.6f);   // late October, Budapest: about 1 mm/day
}

// ---------------------------------------------------------------- system tests
static void boot() {
  host::rtcBase = utc(2026, 10, 30, 9, 0);    // Friday 10:00 local
  host::rtcBaseMs = host::ms;
  host::rtcValid = true;
  for (uint8_t z = 0; z < ZONES; z++) host::analogValue[PIN_SOIL[z]] = rawFor(46);
  host::pinLevel[PIN_RAIN_OUT] = HIGH;          // rain sensor dry (pull-up)
  Preferences p;
  p.begin("wifi");
  p.putString("ssid", "TestNet");
  p.putString("pass", "password1");
  p.end();
  sys.begin();
  sys.api.routes(srv);
  tick(3000);                                 // sensors settle, Wi-Fi connects
}

static void testBoot() {
  CHECK(sys.net.mode() == WifiPortal::Mode::STA);
  CHECK(!on(PIN_PUMP));
  for (uint8_t z = 0; z < ZONES; z++) CHECK(!on(PIN_VALVE[z]));
  JsonDocument s = state();
  CHECK_EQ(std::string(s["name"] | ""), std::string("irrigation"));
  CHECK_EQ((int)s["z"].size(), 4);
  CHECK_EQ((int)s["ok"], 1);
  CHECK_EQ((int)s["tank"]["low"], 0);
  CHECK_EQ((int)s["tank"]["pct"], 66);                       // 30 cm in a 72..8 cm tank
  CHECK_EQ((int)s["z"][0]["soil"], 46);
  CHECK_EQ(std::string(s["z"][0]["st"] | ""), std::string("ON"));
  CHECK(s["z"][0]["last"].isNull());
}

static void testWaterNow() {
  JsonDocument r = post("/api/water", {{"id", "1"}, {"sec", "60"}});
  CHECK_EQ(std::string(r["r"] | ""), std::string("OK"));
  tick(200);
  CHECK(on(PIN_VALVE[0]) && !on(PIN_PUMP));                   // valve first
  tick(1500);
  CHECK(on(PIN_PUMP));                                        // pump 1.5 s later
  CHECK_EQ(std::string(state()["z"][0]["st"] | ""), std::string("WATERING"));

  // a second zone while busy is queued
  r = post("/api/water", {{"id", "2"}, {"sec", "60"}});
  CHECK_EQ(std::string(r["r"] | ""), std::string("BUSY"));
  CHECK_EQ(std::string(state()["z"][1]["st"] | ""), std::string("QUEUED"));

  tick(60000);
  CHECK(!on(PIN_PUMP) && on(PIN_VALVE[0]));                  // pump stops first
  tick(1600);
  CHECK(!on(PIN_VALVE[0]));
  CHECK(logHas(",1,MANUAL,46,46,60,2.0,DONE"));
  CHECK_EQ((int)state()["z"][0]["last"]["sec"], 60);
  CHECK_EQ(std::string(state()["z"][0]["last"]["r"] | ""), std::string("DONE"));
  tick(2000);
  CHECK(on(PIN_VALVE[1]));                                    // zone 2 follows
  post("/api/stop");
  tick(2000);
  CHECK(!on(PIN_VALVE[1]) && !on(PIN_PUMP));
  CHECK(logHas(",2,MANUAL,46,46,"));
  CHECK(logHas("USER"));
  CHECK(call(HTTP_GET, "/api/settings")["tot"]["d"]["sec"].as<int>() >= 60);
}

static void testDndAndCold() {
  uint32_t now = sys.clock.unixNow();
  CHECK(post("/api/dnd", {{"id", "3"}, {"until", std::to_string(now + 3600)}})["ok"] == 1);
  JsonDocument r = post("/api/water", {{"id", "3"}, {"sec", "60"}});
  CHECK_EQ(std::string(r["r"] | ""), std::string("DND"));
  CHECK_EQ(std::string(r["k"] | ""), std::string("z"));
  r = post("/api/water", {{"id", "3"}, {"sec", "60"}, {"dnd", "1"}});
  CHECK_EQ(std::string(r["r"] | ""), std::string("OK"));
  CHECK_EQ(sys.dnd.until(3), 0u);                             // watering cleared the zone's DND
  tick(2000);
  // a DND set now stops the run
  post("/api/dnd", {{"id", "0"}, {"until", std::to_string(now + 600)}});
  tick(2000);
  CHECK(!on(PIN_VALVE[2]));
  CHECK(logHas(",3,MANUAL,") && logHas(",DND\n"));
  post("/api/dnd", {{"id", "0"}, {"until", "0"}});

  host::bmeT = 3;                                             // below the zone limit (4), above frost (2)
  tick(6000);
  r = post("/api/water", {{"id", "1"}, {"sec", "60"}});
  CHECK_EQ(std::string(r["r"] | ""), std::string("COLD"));
  CHECK_EQ((int)r["lim"], 4);
  r = post("/api/water", {{"id", "1"}, {"sec", "60"}, {"force", "1"}});
  CHECK_EQ(std::string(r["r"] | ""), std::string("OK"));
  post("/api/stop");
  host::bmeT = 1;                                             // frost
  tick(6000);
  CHECK_EQ(std::string(post("/api/water", {{"id", "1"}, {"sec", "60"}})["r"] | ""), std::string("FROST"));
  CHECK_EQ(std::string(state()["z"][0]["why"] | ""), std::string("FROST"));
  host::bmeT = 15;
  tick(6000);
}

static void testZoneSettings() {
  JsonDocument r = post("/api/zone", {{"id", "2"}, {"name", "Back lawn \xF0\x9F\x8C\xB1"}, {"m", "1"}});
  CHECK(r["ok"] == 1);
  CHECK_EQ(std::string(state()["z"][1]["name"] | ""), std::string("Back lawn \xF0\x9F\x8C\xB1"));
  CHECK_EQ(std::string(post("/api/zone", {{"id", "2"}, {"plant.lo", "60"}, {"plant.hi", "50"}})["e"] | ""), std::string("band"));
  CHECK_EQ(std::string(post("/api/zone", {{"id", "2"}, {"cold", "1"}})["e"] | ""), std::string("cold"));   // below frost
  CHECK_EQ(std::string(post("/api/settings", {{"name", "kert kert"}})["e"] | ""), std::string("host"));
  CHECK(post("/api/settings", {{"name", "kert"}})["ok"] == 1);
  CHECK_EQ(std::string(post("/api/cal", {{"s", "1"}, {"dry", "1000"}})["e"] | ""), std::string("order"));
  CHECK(post("/api/cal", {{"s", "1"}, {"dry", "2900"}})["ok"] == 1);
  CHECK(post("/api/cal", {{"s", "1"}, {"dry", "2800"}})["ok"] == 1);

  // deficit, zone 2 at 33 %: available (27-12)% x 15 cm = 22.5 mm; (50-33)% of it = 3.8 mm; / 70 % / 20 mm/h = 980 s
  host::analogValue[PIN_SOIL[1]] = rawFor(33);
  tick(1100);
  JsonDocument s = state();
  CHECK_EQ((int)s["z"][1]["soil"], 33);
  CHECK_EQ((int)s["z"][1]["def"]["sec"], 980);
  CHECK(fabs(s["z"][1]["def"]["mm"].as<float>() - 3.8f) < 0.01);
}

static void testSchedule() {
  LTime l;
  TimeUtil::toLocal(sys.clock.unixNow(), l);
  uint16_t at = l.mod + 2;
  // zone 4, Schedule mode, every day in 2 minutes, 120 s
  CHECK(post("/api/sched", {{"id", "4"}, {"sid", "0"}, {"d", "127"}, {"s", std::to_string(at)}, {"sec", "120"}})["ok"] == 1);
  // zone 2, closed loop, same minute: soil 33 % < lo 35 % -> the deficit dose (980 s), queued behind zone 4
  CHECK(post("/api/sched", {{"id", "2"}, {"sid", "0"}, {"d", "127"}, {"s", std::to_string(at)}, {"sec", "0"}})["ok"] == 1);
  // zone 1 with a DND window over that minute -> SKIP_DND
  post("/api/dsch", {{"id", "1"}, {"sid", "0"}, {"d", "127"}, {"s", std::to_string(at - 1)}, {"e", std::to_string(at + 30)}});
  post("/api/sched", {{"id", "1"}, {"sid", "0"}, {"d", "127"}, {"s", std::to_string(at)}, {"sec", "300"}});

  JsonDocument z4 = state()["z"][3];
  CHECK(!z4["next"].isNull());
  CHECK_EQ((int)z4["next"]["sec"], 120);
  CHECK_EQ(std::string(state()["z"][0]["next"]["x"] | ""), std::string("DND"));

  jump(130000);
  CHECK(logHas(",1,SCHEDULE,46,,0,,SKIP_DND"));
  CHECK_EQ((int)sys.water.activeZone(), 1);                   // same minute: zone order, zone 2 first
  CHECK_EQ(sys.water.runTotalS(), 980u);                      // closed loop: the deficit dose
  CHECK_EQ(std::string(state()["z"][3]["st"] | ""), std::string("QUEUED"));
  tick(985000);
  CHECK(logHas(",2,SCHEDULE,33,33,980,32.7,DONE"));
  CHECK_EQ((int)sys.water.activeZone(), 3);                   // then zone 4
  tick(125000);
  CHECK(logHas(",4,SCHEDULE,46,46,120,4.0,DONE"));
  CHECK(sys.irr.idle());
  // the next day's occurrence is predicted, not today's again
  CHECK(state()["z"][3]["next"]["t"].as<uint32_t>() > sys.clock.unixNow() + 23 * 3600);
}

static void press(uint32_t holdMs) {
  host::pinLevel[PIN_BUTTON] = HIGH;
  tick(holdMs);
  host::pinLevel[PIN_BUTTON] = LOW;
  tick(100);
}

static void testButton() {
  // short press: every deficit zone, driest first (zone 2 at 33 % before zone 1 at 46 %)
  press(200);
  CHECK_EQ((int)sys.water.activeZone(), 1);
  CHECK(sys.irr.queued(0));
  CHECK_EQ(sys.water.runTotalS(), 900u);                      // 980 s capped to 15 min
  // long press while watering: stop and winter mode on
  press(3200);
  tick(2000);
  CHECK(!sys.water.busy() && !sys.irr.queued(0));
  CHECK_EQ((int)sys.cfg.g.off, 1);
  CHECK_EQ(std::string(state()["z"][0]["st"] | ""), std::string("ALLOFF"));
  CHECK_EQ(std::string(post("/api/water", {{"id", "1"}, {"sec", "60"}})["r"] | ""), std::string("ALL_OFF"));
  // short press in winter mode: winter mode off, nothing else
  press(200);
  CHECK_EQ((int)sys.cfg.g.off, 0);
  CHECK(!sys.water.busy());
  CHECK(sys.cost.counter(CP_SEASON).f != 0);                  // a new season
  // long press while idle: every enabled zone, 15 min each
  press(3200);
  tick(500);
  CHECK(sys.water.busy());
  CHECK(sys.irr.queued(1) && sys.irr.queued(2) && sys.irr.queued(3));
  press(200);                                                 // stops everything
  tick(2000);
  CHECK(sys.irr.idle());
}

static void testOutputTest() {
  JsonDocument r = post("/api/test", {{"o", "valve"}, {"z", "1"}});
  CHECK_EQ((int)r["sec"], 60);
  tick(100);
  CHECK(on(PIN_VALVE[0]) && !on(PIN_PUMP));
  r = post("/api/test", {{"o", "pump"}});
  CHECK(r["sec"].as<int>() >= 57 && r["sec"].as<int>() <= 58);
  tick(1600);
  CHECK(on(PIN_PUMP));
  CHECK_EQ((int)state()["tst"].size(), 2);
  // a zone may not water during a test: it waits in the queue
  CHECK_EQ(std::string(post("/api/water", {{"id", "3"}, {"sec", "60"}})["r"] | ""), std::string("BUSY"));
  post("/api/stop");                                          // clear that queue entry
  post("/api/test", {{"o", "stop"}, {"t", "valve"}, {"z", "1"}});
  tick(100);
  CHECK(!on(PIN_PUMP) && !on(PIN_VALVE[0]));                 // the pump never runs on closed valves
  r = post("/api/test", {{"o", "pump"}});                     // alone, 10 s
  CHECK_EQ((int)r["sec"], 10);
  tick(200);
  CHECK(on(PIN_PUMP));
  tick(10000);
  CHECK(!on(PIN_PUMP));
  CHECK(!sys.water.testing());
}

static void testTankLow() {
  JsonDocument r = post("/api/water", {{"id", "4"}, {"sec", "300"}});   // zone 1 has a DND window now
  CHECK_EQ(std::string(r["r"] | ""), std::string("OK"));
  tick(3000);
  CHECK(on(PIN_PUMP));
  host::pulseUs = 0;                                          // no echo -> low
  tick(1500);
  CHECK(!on(PIN_PUMP));
  tick(2000);
  CHECK(logHas(",TANK_LOW\n"));
  CHECK_EQ(std::string(post("/api/water", {{"id", "4"}, {"sec", "60"}})["r"] | ""), std::string("TANK_LOW"));
  tick(6000);
  CHECK(logHas(",0,ALERT,TANK_LOW,,,,W"));
  host::pulseUs = 1750;
  tick(8000);
  CHECK(logHas(",0,ALERT,TANK_OK,,,,I"));
}

static void testCostRollover() {
  post("/api/settings", {{"clog", "1"}, {"cmode", "1"}, {"cpw", "1000"}});
  // to 00:05 tomorrow
  LTime l;
  TimeUtil::toLocal(sys.clock.unixNow(), l);
  jump((1440 - l.mod + 5) * 60000UL);
  CHECK(logHas(",0,COST,D,"));
  CHECK_EQ(sys.cost.counter(CP_DAY).sec, 0u);
  CHECK_EQ(sys.water.secondsToday(0), 0u);                    // daily budget reset at midnight
}

static void testLogApi() {
  std::string body = srv.request(HTTP_GET, "/api/log");
  CHECK(body.rfind("utc,local,zone,mode,soil0,soil1,sec,litres,result\n", 0) == 0);
  CHECK(body.find("utc,local", 10) == std::string::npos);    // the file's own header is not repeated
  JsonDocument z = call(HTTP_GET, "/api/zone", {{"id", "1"}});
  CHECK(z["hist"].size() > 0 && z["hist"][0].size() == 9);
  CHECK(std::string(z["hist"][0][8] | "").find('\n') == std::string::npos);
  CHECK_EQ(std::string(call(HTTP_GET, "/api/zone", {{"id", "9"}})["e"] | ""), std::string("id"));
  JsonDocument w = call(HTTP_GET, "/api/weather");
  CHECK_EQ((int)w["t"], 0);                                   // no forecast fetched on the host
}

int main() {
  testTime();
  testDndWindows();
  testUtil();
  boot();
  testBoot();
  testWaterNow();
  testDndAndCold();
  testZoneSettings();
  testSchedule();
  testButton();
  testOutputTest();
  testTankLow();
  testCostRollover();
  testLogApi();
  printf("%d checks, %d failed\n", checks, fails);
  if (fails) {
    printf("---- log ----\n%s", host::files()["/log.csv"].c_str());
  }
  return fails ? 1 : 0;
}
