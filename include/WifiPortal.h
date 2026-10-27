#pragma once
// WiFi with a setup portal (header-only).
//   Saved credentials                        -> join that network, serve the controller UI (and /health)
//   No credentials, no connection in 30 s,
//   or button held at boot                   -> open setup AP with a captive portal
//
// The portal page is one static, gzipped, bilingual (hu/en) HTML file kept in flash
// (PortalPage.h, generated from portal/index.html by tools/gen_portal.py). The page
// talks to the ESP only through three tiny JSON endpoints: /scan, /rescan and /save.
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <algorithm>
#include <functional>
#include "PortalPage.h"   // setup page (AP mode), generated from portal/index.html
#include "AppPage.h"      // controller UI (station mode), generated from portal/app.html

// ---- Settings ----
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 30000;   // saved network: give up and open the setup AP
constexpr uint32_t WIFI_PRESCAN_TIMEOUT_MS = 20000;   // stop waiting for the first scan and open the AP anyway
constexpr uint32_t WIFI_AP_RETRY_PERIOD_MS = 60000;   // setup AP (not button-forced): retry the saved network...
constexpr uint32_t WIFI_AP_RETRY_WINDOW_MS = 15000;   // ...for this long, only while nobody is on the AP
constexpr uint32_t WIFI_RESTART_DELAY_MS   = 1000;    // after saving: let the browser get the reply, then reboot
constexpr uint8_t  WIFI_SCAN_MAX_RETRIES   = 3;
constexpr uint8_t  WIFI_MAX_NETS           = 24;
constexpr const char* WIFI_HOSTNAME  = "irrigation"; // http://irrigation.local
constexpr const char* WIFI_AP_PREFIX = "Irrigation-Setup-";

// ----------------------------------------------------------------------
// Validation (the server checks again whatever the page already checked)
// ----------------------------------------------------------------------

// Well-formed UTF-8 (an invalid name would break the JSON sent to the page)
static bool wifiValidUtf8(const String& s) {
  const uint8_t* p = (const uint8_t*)s.c_str();
  size_t n = s.length();
  size_t i = 0;
  while (i < n) {
    uint8_t c = p[i];
    size_t extra;                                   // continuation bytes that must follow
    if (c < 0x80) extra = 0;
    else if (c >= 0xC2 && c <= 0xDF) extra = 1;
    else if ((c & 0xF0) == 0xE0) extra = 2;
    else if (c >= 0xF0 && c <= 0xF4) extra = 3;
    else return false;
    if (extra > 0 && i + extra >= n) return false;   // cut off
    for (size_t j = 1; j <= extra; j++) {
      if ((p[i + j] & 0xC0) != 0x80) return false;
    }
    i += extra + 1;
  }
  return true;
}

// SSID: 1..32 bytes, valid UTF-8, no control characters (phone hotspots often have accents or apostrophes)
static bool wifiValidSsid(const String& s) {
  size_t n = s.length();
  if (n < 1 || n > 32) return false;
  for (size_t i = 0; i < n; i++) {
    uint8_t c = (uint8_t)s[i];
    if (c < 0x20 || c == 0x7F) return false;
  }
  return wifiValidUtf8(s);
}

// Password: empty (open network) or a WPA2 passphrase of 8..63 printable ASCII characters
static bool wifiValidPass(const String& p) {
  size_t n = p.length();
  if (n == 0) return true;
  if (n < 8 || n > 63) return false;
  for (size_t i = 0; i < n; i++) {
    uint8_t c = (uint8_t)p[i];
    if (c < 0x20 || c > 0x7E) return false;
  }
  return true;
}

static String jsonEscape(const String& s) {
  String o;
  o.reserve(s.length() + 4);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') o += '\\';
    o += c;
  }
  return o;
}

// ----------------------------------------------------------------------
// WifiPortal
// ----------------------------------------------------------------------
class WifiPortal {
public:
  enum class Mode : uint8_t { OFF, CONNECTING, STA, PRESCAN, AP };
  using HealthFn = String (*)();

  WifiPortal() : _server(80) {}

  void setHealth(HealthFn f) { _health = f; }
  // Extra routes (the controller API), added to the same server. Call before begin().
  void setRoutes(std::function<void(WebServer&)> f) { _routes = f; }
  Mode mode() const { return _mode; }
  bool apMode() const { return _mode == Mode::AP; }
  bool connected() const { return _mode == Mode::STA && WiFi.status() == WL_CONNECTED; }
  bool restartPending() const { return _restartPending; }   // true from "Save" until the reboot (about 1 s)

  // forceAp: button held at boot
  void begin(bool forceAp) {
    WiFi.persistent(false);        // credentials live in our own NVS namespace, not the SDK's
    loadCreds();
    registerRoutes();
    if (forceAp) {
      Serial.println("WiFi: service mode, starting setup AP");
      startAp(true);
    } else if (_ssid.length() == 0) {
      Serial.println("WiFi: no saved credentials, starting setup AP");
      startAp(false);
    } else {
      startSta();
    }
  }

  // Call every loop pass; never blocks for long
  void update() {
    switch (_mode) {
      case Mode::CONNECTING:
        if (WiFi.status() == WL_CONNECTED) {
          onConnected();
        } else if (since(_tMode) >= WIFI_CONNECT_TIMEOUT_MS) {
          Serial.println("WiFi: no connection within 30 s, starting setup AP");
          _connectFailed = true;
          startAp(false);
        }
        break;

      case Mode::STA:
        trackConnection();
        _server.handleClient();
        break;

      case Mode::PRESCAN:
        pollScan();
        if (!_scanning) {
          openAp();
        } else if (since(_tMode) >= WIFI_PRESCAN_TIMEOUT_MS) {
          _scanning = false;
          WiFi.scanDelete();
          openAp();
        }
        break;

      case Mode::AP:
        _dns.processNextRequest();
        _server.handleClient();
        pollScan();
        apRetry();
        break;

      default:
        break;
    }

    if (_restartPending && (int32_t)(millis() - _restartAt) >= 0) restart("WiFi: restarting");
  }

private:
  struct Net { String ssid; int8_t rssi; bool open; };

  static uint32_t since(uint32_t t) { return (uint32_t)(millis() - t); }

  void restart(const char* why) {
    Serial.println(why);
    delay(50);
    ESP.restart();
  }

  // ---------------- credentials ----------------
  void loadCreds() {
    Preferences p;
    p.begin("wifi", false);
    _ssid = p.getString("ssid", "");
    _pass = p.getString("pass", "");
    p.end();
    if (!wifiValidSsid(_ssid) || !wifiValidPass(_pass)) {
      _ssid = "";
      _pass = "";
    }
  }

  bool saveCreds(const String& ssid, const String& pass) {
    Preferences p;
    if (!p.begin("wifi", false)) return false;
    p.putString("ssid", ssid);
    p.putString("pass", pass);
    bool ok = (p.getString("ssid", "") == ssid) && (p.getString("pass", "") == pass);   // read back
    p.end();
    return ok;
  }

  // ---------------- station ----------------
  void startSta() {
    _mode = Mode::CONNECTING;
    _tMode = millis();
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(WIFI_HOSTNAME);     // after mode(STA), before begin
    WiFi.setSleep(false);                // mains powered: keeps mDNS and the web page responsive
    WiFi.setAutoReconnect(true);
    WiFi.begin(_ssid.c_str(), _pass.c_str());
    Serial.printf("WiFi: connecting to \"%s\" (up to %lu s)\n", _ssid.c_str(), (unsigned long)(WIFI_CONNECT_TIMEOUT_MS / 1000));
  }

  void onConnected() {
    _mode = Mode::STA;
    ensureServer();
    startMdns();
    _wasUp = true;
    Serial.printf("WiFi: connected, IP %s, http://%s.local/\n", WiFi.localIP().toString().c_str(), WIFI_HOSTNAME);
  }

  // Log drops and re-announce mDNS after a reconnect (the SDK reconnects by itself)
  void trackConnection() {
    bool up = WiFi.status() == WL_CONNECTED;
    if (up && !_wasUp) {
      startMdns();
      Serial.printf("WiFi: reconnected, IP %s\n", WiFi.localIP().toString().c_str());
    } else if (!up && _wasUp) {
      Serial.println("WiFi: connection lost, retrying automatically");
    }
    _wasUp = up;
  }

  void startMdns() {
    if (_mdnsOn) MDNS.end();
    _mdnsOn = MDNS.begin(WIFI_HOSTNAME);
    if (_mdnsOn) MDNS.addService("http", "tcp", 80);
  }

  // ---------------- setup access point ----------------
  // Scan first with no AP running: once a phone is on the AP, scans fail or come back empty.
  // PRESCAN waits for that scan, then openAp() starts the AP.
  void startAp(bool forced) {
    _apForced = forced;
    _retrying = false;
    _tRetry = millis();
    WiFi.disconnect(false, false);       // stop any connection attempt, keep the radio on
    WiFi.mode(WIFI_STA);
    delay(100);                          // let the STA interface come up, otherwise the scan fails with -2
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(false);        // background reconnect scans hop channels and drop AP clients
    _mode = Mode::PRESCAN;
    _tMode = millis();
    _scanRetries = 0;
    startScan();
    Serial.println("WiFi: scanning for networks, then opening the setup AP");
  }

  void openAp() {
    WiFi.mode(WIFI_AP_STA);              // keep the STA half for the "Scan again" button
    WiFi.setSleep(false);

    uint8_t mac[6];
    WiFi.macAddress(mac);
    char name[40];
    snprintf(name, sizeof(name), "%s%02X%02X", WIFI_AP_PREFIX, mac[4], mac[5]);
    WiFi.softAP(name, nullptr, 1, 0, 4);   // open network, channel 1, max 4 clients
    delay(200);

    const IPAddress apIp(192, 168, 4, 1);
    WiFi.softAPConfig(apIp, apIp, IPAddress(255, 255, 255, 0));   // after softAP, so DHCP hands out us as DNS server
    _dns.setErrorReplyCode(DNSReplyCode::NoError);
    _dns.start(53, "*", apIp);           // captive portal: every name resolves to us

    _mode = Mode::AP;
    ensureServer();
    Serial.printf("WiFi: setup AP \"%s\" (open), http://%s/\n", name, WiFi.softAPIP().toString().c_str());
  }

  // After a failed connection (AP not forced by the button) and with nobody on the AP,
  // try the saved network again now and then: hotspot switched on late, router still rebooting...
  void apRetry() {
    if (_apForced || _restartPending || _ssid.length() == 0) return;
    uint8_t clients = WiFi.softAPgetStationNum();

    if (!_retrying) {
      if (clients == 0 && !_scanning && since(_tRetry) >= WIFI_AP_RETRY_PERIOD_MS) {
        WiFi.begin(_ssid.c_str(), _pass.c_str());
        _retrying = true;
        _tTry = millis();
      }
      return;
    }

    if (clients > 0) {                                        // someone arrived: leave them alone
      stopRetry();
    } else if (WiFi.status() == WL_CONNECTED) {
      restart("WiFi: saved network is back, restarting");     // clean restart into normal station mode
    } else if (since(_tTry) >= WIFI_AP_RETRY_WINDOW_MS) {    // no luck this time
      stopRetry();
    }
  }

  void stopRetry() {
    WiFi.disconnect(false, false);
    _retrying = false;
    _tRetry = millis();
  }

  // ---------------- scan (async, so the safety layer keeps running) ----------------
  void startScan() {
    WiFi.scanDelete();
    int16_t rc = WiFi.scanNetworks(true, false);   // -1 = started, -2 = the radio refused to start it
    Serial.printf("WiFi: scan start rc=%d, wifi mode=%d\n", (int)rc, (int)WiFi.getMode());
    _scanning = true;
  }

  // The first scan after boot can fail (-2), and later scans then return nothing until the radio is restarted.
  // Only safe before the AP exists: restarting the radio would drop the phone.
  void resetRadio() {
    WiFi.scanDelete();
    WiFi.mode(WIFI_OFF);
    delay(100);
    WiFi.mode(WIFI_STA);
    delay(100);
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(false);
  }

  void pollScan() {
    if (!_scanning) return;
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return;

    if (n < 0) Serial.printf("WiFi: scan failed (code %d, try %u)\n", n, (unsigned)_scanRetries);
    if (n <= 0 && _scanRetries < WIFI_SCAN_MAX_RETRIES) {    // failed or empty: try again
      _scanRetries++;
      if (_mode == Mode::PRESCAN) resetRadio();
      startScan();
      return;
    }

    _scanning = false;
    if (n <= 0 && _netCount > 0) {                           // failed rescan: keep the earlier list
      WiFi.scanDelete();
      return;
    }
    collectNetworks(n);
    WiFi.scanDelete();
    Serial.printf("WiFi: scan done, %d found, %u listed\n", n, (unsigned)_netCount);
  }

  // Rebuild _nets from the scan results: usable names only, strongest first
  void collectNetworks(int n) {
    _netCount = 0;
    for (int i = 0; i < n; i++) {
      String name = WiFi.SSID(i);
      if (!wifiValidSsid(name)) continue;       // hidden or odd names: the user types them by hand
      addNet(name, (int8_t)WiFi.RSSI(i), WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
    }
    std::sort(_nets, _nets + _netCount, [](const Net& a, const Net& b) { return a.rssi > b.rssi; });
  }

  // Same name seen on several access points: keep the strongest
  void addNet(const String& name, int8_t rssi, bool open) {
    for (uint8_t i = 0; i < _netCount; i++) {
      if (_nets[i].ssid != name) continue;
      if (rssi > _nets[i].rssi) {
        _nets[i].rssi = rssi;
        _nets[i].open = open;
      }
      return;
    }
    if (_netCount < WIFI_MAX_NETS) _nets[_netCount++] = Net{name, rssi, open};
  }

  // ---------------- web server ----------------
  void ensureServer() {
    if (_serverOn) return;
    _server.begin();
    _serverOn = true;
  }

  void registerRoutes() {
    // In AP mode "/" is the setup page, otherwise the controller UI (the plain-text health page stays at /health)
    _server.on("/", HTTP_GET, [this]() {
      if (_mode == Mode::AP) sendPortal();
      else sendApp();
    });
    _server.on("/health", HTTP_GET, [this]() { sendHealth(); });

    // "ap" lets the page tell the setup AP from the real controller
    _server.on("/ping", HTTP_GET, [this]() {
      _server.sendHeader("Access-Control-Allow-Origin", "*");
      _server.send(200, "text/plain", _mode == Mode::AP ? "ap" : "ok");
    });

    // The page carries its own inline SVG icon; browsers that still ask get an empty answer
    _server.on("/favicon.ico", HTTP_GET, [this]() { _server.send(204); });

    // Phones probe these URLs to detect a captive portal; any redirect opens the setup page
    for (const char* probe : {"/generate_204", "/gen_204", "/hotspot-detect.html", "/library/test/success.html",
                              "/connecttest.txt", "/ncsi.txt", "/redirect", "/canonical.html"}) {
      _server.on(probe, HTTP_GET, [this]() {
        if (_mode == Mode::AP) redirectHome();
        else notFound();
      });
    }

    _server.on("/scan", HTTP_GET, [this]() {
      if (_mode == Mode::AP) sendScan();
      else notFound();
    });
    _server.on("/rescan", HTTP_GET, [this]() {
      if (_mode != Mode::AP) { notFound(); return; }
      if (_retrying) stopRetry();
      _scanRetries = 0;
      startScan();
      _server.send(200, "text/plain", "ok");
    });
    _server.on("/save", HTTP_POST, [this]() { handleSave(); });

    if (_routes) _routes(_server);       // the controller API; its handlers refuse to answer outside station mode

    _server.onNotFound([this]() {
      if (_mode == Mode::AP) {
        Serial.printf("WiFi: captive probe http://%s%s\n", _server.hostHeader().c_str(), _server.uri().c_str());
        redirectHome();
      } else notFound();
    });
  }

  void notFound() { _server.send(404, "text/plain", "not found"); }

  void redirectHome() {
    _server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
    _server.sendHeader("Cache-Control", "no-store");
    _server.send(302, "text/plain", "");
  }

  // The gzipped setup page, straight from flash (no String copy, no templating).
  // Static, so the browser may keep it for a few minutes.
  void sendPortal() {
    _server.sendHeader("Content-Encoding", "gzip");
    _server.sendHeader("Cache-Control", "max-age=600");
    _server.send_P(200, "text/html; charset=utf-8", (PGM_P)PORTAL_GZ, PORTAL_GZ_LEN);
  }

  // The gzipped controller UI. A short max-age: a firmware update ships a new page.
  void sendApp() {
    _server.sendHeader("Content-Encoding", "gzip");
    _server.sendHeader("Cache-Control", "max-age=60");
    _server.send_P(200, "text/html; charset=utf-8", (PGM_P)APP_GZ, APP_GZ_LEN);
  }

  void sendJson(const String& j) {
    _server.sendHeader("Cache-Control", "no-store");
    _server.send(200, "application/json", j);
  }

  // {"w":1}   while scanning
  // {"w":0,"f":0|1,"v":"saved ssid","h":"hostname","n":[{"s":name,"r":rssi,"o":1 if open},...]}
  // f = the last connection attempt failed (v is the network that failed)
  void sendScan() {
    Serial.printf("WiFi: /scan requested, scanning=%d, listed=%u\n", (int)_scanning, (unsigned)_netCount);
    String j;
    if (_scanning) {
      j = "{\"w\":1}";
    } else {
      j.reserve(96 + _netCount * 56);
      j = "{\"w\":0,\"f\":";
      j += _connectFailed ? '1' : '0';
      j += ",\"v\":\"";
      j += jsonEscape(_ssid);
      j += "\",\"h\":\"";
      j += WIFI_HOSTNAME;
      j += "\",\"n\":[";
      for (uint8_t i = 0; i < _netCount; i++) {
        if (i) j += ',';
        j += "{\"s\":\"";
        j += jsonEscape(_nets[i].ssid);
        j += "\",\"r\":";
        j += (int)_nets[i].rssi;
        j += ",\"o\":";
        j += _nets[i].open ? '1' : '0';
        j += '}';
      }
      j += "]}";
    }
    sendJson(j);
  }

  void sendHealth() {
    String s = _health ? _health() : String("ok\n");
    _server.sendHeader("Cache-Control", "no-store");
    _server.send(200, "text/plain; charset=utf-8", s);
  }

  // Answers {"ok":1} or {"e":"ssid"|"pass"|"save"}; the page shows the (translated) message
  void handleSave() {
    if (_mode != Mode::AP) { notFound(); return; }

    String ssid = _server.arg("ssid");
    ssid.trim();                          // stray spaces from keyboards; the password is never trimmed
    String pass = _server.arg("pass");

    if (!wifiValidSsid(ssid)) { sendJson("{\"e\":\"ssid\"}"); return; }
    if (!wifiValidPass(pass)) { sendJson("{\"e\":\"pass\"}"); return; }
    if (!saveCreds(ssid, pass)) { sendJson("{\"e\":\"save\"}"); return; }

    Serial.printf("WiFi: saved credentials for \"%s\"\n", ssid.c_str());
    sendJson("{\"ok\":1}");
    _restartPending = true;
    _restartAt = millis() + WIFI_RESTART_DELAY_MS;
  }

  // ---------------- state ----------------
  HealthFn _health = nullptr;
  std::function<void(WebServer&)> _routes;
  WebServer _server;
  DNSServer _dns;
  Mode _mode = Mode::OFF;

  String _ssid, _pass;
  Net _nets[WIFI_MAX_NETS];
  uint8_t _netCount = 0;

  uint32_t _tMode = 0, _tRetry = 0, _tTry = 0, _restartAt = 0;
  uint8_t _scanRetries = 0;
  bool _serverOn = false, _mdnsOn = false, _wasUp = false, _scanning = false;
  bool _apForced = false, _retrying = false, _connectFailed = false, _restartPending = false;
};
