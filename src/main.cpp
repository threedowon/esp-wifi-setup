#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_system.h>
#include <time.h>
#include "web_page.h"

// Original 1.14-inch T-Display (ESP32), not T-Display-S3.
constexpr uint8_t SETUP_BUTTON = 35;
constexpr uint32_t CONNECT_TIMEOUT = 25000;
constexpr uint32_t PORTAL_TIMEOUT = 10 * 60 * 1000;
const IPAddress PORTAL_IP(192, 168, 4, 1);
Preferences preferences;
TFT_eSPI display;
WebServer server(80);
DNSServer dns;
String apName, apPassword, csrfToken;
String savedSsid, savedPassword, pendingSsid, pendingPassword;
String connectionState = "idle";
String message;
bool portalOpen = false;
bool candidateConnection = false;
bool use24Hour = true;
uint8_t brightness = 180;
uint32_t connectionStarted = 0, connectionQueued = 0;
uint32_t portalTouched = 0, closeRequested = 0;
uint32_t disconnectedSince = 0, buttonSince = 0;
bool buttonHandled = false;
enum class Attempt { None, Queued, Connecting };
Attempt attempt = Attempt::None;

String randomKey() {
  char key[17];
  snprintf(key, sizeof(key), "%08lx%08lx",
           static_cast<unsigned long>(esp_random()),
           static_cast<unsigned long>(esp_random()));
  return String(key);
}

void sendJson(int status, const JsonDocument &document) {
  String output;
  serializeJson(document, output);
  server.sendHeader("Cache-Control", "no-store");
  server.send(status, "application/json; charset=utf-8", output);
}

void reply(int status, const String &text) {
  StaticJsonDocument<256> document;
  document["message"] = text;
  sendJson(status, document);
}

bool allowRequest(bool mutation = false) {
  // The HTTP server is available only through the setup AP, including AP+STA mode.
  if (!portalOpen || server.client().localIP() != PORTAL_IP) {
    reply(403, "설정용 와이파이에 연결한 뒤 다시 열어주세요.");
    return false;
  }
  if (mutation && server.arg("token") != csrfToken) {
    reply(403, "페이지를 새로고침한 뒤 다시 시도해주세요.");
    return false;
  }
  portalTouched = millis();
  return true;
}

void openPortal() {
  if (portalOpen) return;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(PORTAL_IP, PORTAL_IP, IPAddress(255, 255, 255, 0));
  if (!WiFi.softAP(apName.c_str(), apPassword.c_str())) {
    message = "설정용 와이파이를 열지 못했어요. 전원을 다시 켜주세요.";
    return;
  }
  csrfToken = randomKey();
  dns.start(53, "*", PORTAL_IP);
  server.begin();
  portalOpen = true;
  portalTouched = millis();
  closeRequested = 0;
  // Only the device's setup password is printed, never the router password.
  Serial.printf("Setup Wi-Fi: %s\nSetup password: %s\nhttp://192.168.4.1\n",
                apName.c_str(), apPassword.c_str());
}

void closePortal() {
  server.stop();
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  portalOpen = false;
  closeRequested = 0;
}

void queueConnection(const String &ssid, const String &password, bool candidate) {
  pendingSsid = ssid;
  pendingPassword = password;
  candidateConnection = candidate;
  connectionState = "connecting";
  message = "와이파이에 연결하고 있어요. 최대 25초 정도 걸려요.";
  connectionQueued = millis();
  attempt = Attempt::Queued;
}

void updateConnection() {
  const uint32_t now = millis();
  if (attempt == Attempt::Queued && now - connectionQueued >= 700) {
    // Send the HTTP response before retuning the shared AP/STA radio.
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false, false);
    WiFi.begin(pendingSsid.c_str(), pendingPassword.c_str());
    connectionStarted = now;
    attempt = Attempt::Connecting;
  }
  if (attempt == Attempt::Connecting) {
    if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
      bool stored = true;
      if (candidateConnection) {
        // Commit both fields together only after connection + DHCP succeeds.
        StaticJsonDocument<384> credentials;
        credentials["ssid"] = pendingSsid;
        credentials["password"] = pendingPassword;
        String record;
        serializeJson(credentials, record);
        stored = preferences.putString("wifi", record) == record.length();
        if (stored) {
          savedSsid = pendingSsid;
          savedPassword = pendingPassword;
        }
      }
      pendingPassword = "";
      attempt = Attempt::None;
      connectionState = stored ? "connected" : "storage_error";
      message = stored ? "연결 정보를 저장했어요. 전원을 다시 켜도 자동 연결돼요."
                       : "연결됐지만 저장에 실패했어요. 다시 저장해주세요.";
      WiFi.setAutoReconnect(true);
      configTime(9 * 3600, 0, "pool.ntp.org", "time.google.com");
      disconnectedSince = 0;
    } else if (now - connectionStarted >= CONNECT_TIMEOUT) {
      WiFi.disconnect(false, false);
      pendingPassword = "";
      attempt = Attempt::None;
      connectionState = "failed";
      message = "연결하지 못했어요. 2.4GHz 와이파이와 비밀번호를 확인해주세요. 이전 저장 정보는 유지돼요.";
      openPortal();
    }
  }
  if (attempt == Attempt::None && WiFi.status() != WL_CONNECTED) {
    if (!disconnectedSince) disconnectedSince = now;
    if (!portalOpen && now - disconnectedSince >= 30000) openPortal();
  } else {
    disconnectedSince = 0;
  }
}

void configureRoutes() {
  server.on("/", HTTP_GET, []() {
    if (!allowRequest()) return;
    String page(FPSTR(WEB_PAGE));
    page.replace("__TOKEN__", csrfToken);
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/html; charset=utf-8", page);
  });
  server.on("/api/status", HTTP_GET, []() {
    if (!allowRequest()) return;
    StaticJsonDocument<768> result;
    const bool connected = WiFi.status() == WL_CONNECTED;
    result["connected"] = connected;
    result["state"] = connectionState;
    result["message"] = message;
    result["ssid"] = connected ? WiFi.SSID() : savedSsid;
    result["ip"] = connected ? WiFi.localIP().toString() : "";
    result["apName"] = apName;
    result["use24Hour"] = use24Hour;
    result["brightness"] = brightness;
    const time_t now = time(nullptr);
    result["epoch"] = now > 1700000000 ? static_cast<uint32_t>(now) : 0;
    sendJson(200, result);
  });
  server.on("/api/scan", HTTP_GET, []() {
    if (!allowRequest()) return;
    if (attempt != Attempt::None) {
      reply(409, "연결 시도가 끝난 뒤 다시 검색해주세요.");
      return;
    }
    int count = WiFi.scanComplete();
    if (count == WIFI_SCAN_FAILED) {
      WiFi.scanNetworks(true);
      reply(202, "주변 와이파이를 찾고 있어요.");
      return;
    }
    if (count == WIFI_SCAN_RUNNING) {
      reply(202, "주변 와이파이를 찾고 있어요.");
      return;
    }
    DynamicJsonDocument result(4096);
    JsonArray networks = result.createNestedArray("networks");
    for (int i = 0; i < count && networks.size() < 20; ++i) {
      String ssid = WiFi.SSID(i);
      if (ssid.isEmpty()) continue;
      bool duplicate = false;
      for (JsonObject network : networks) {
        if (network["ssid"].as<String>() == ssid) duplicate = true;
      }
      if (duplicate) continue;
      JsonObject network = networks.createNestedObject();
      network["ssid"] = ssid;
      network["rssi"] = WiFi.RSSI(i);
      network["open"] = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
    }
    WiFi.scanDelete();
    sendJson(200, result);
  });
  server.on("/api/wifi", HTTP_POST, []() {
    if (!allowRequest(true)) return;
    if (attempt != Attempt::None || WiFi.scanComplete() == WIFI_SCAN_RUNNING) {
      reply(409, "진행 중인 검색이나 연결이 끝나면 다시 눌러주세요.");
      return;
    }
    String ssid = server.arg("ssid");
    String password = server.arg("password");
    bool openNetwork = server.arg("open") == "1";
    if (ssid.length() == 0 || ssid.length() > 32 ||
        (!openNetwork && (password.length() < 8 || password.length() > 63))) {
      reply(400, "이름은 1~32바이트, 비밀번호는 8~63바이트로 입력해주세요. 비밀번호 없는 네트워크는 체크해주세요.");
      return;
    }
    queueConnection(ssid, openNetwork ? "" : password, true);
    reply(202, message);
  });
  server.on("/api/clock", HTTP_POST, []() {
    if (!allowRequest(true)) return;
    const String value = server.arg("brightness");
    if ((value != "50" && value != "120" && value != "180" && value != "255") ||
        (server.arg("format") != "12" && server.arg("format") != "24")) {
      reply(400, "표시 형식과 밝기를 다시 선택해주세요.");
      return;
    }
    const bool next24Hour = server.arg("format") == "24";
    const uint8_t nextBrightness = value.toInt();
    if (preferences.putBool("24hour", next24Hour) != 1 ||
        preferences.putUChar("brightness", nextBrightness) != 1) {
      reply(500, "설정 저장에 실패했어요. 다시 시도해주세요.");
      return;
    }
    use24Hour = next24Hour;
    brightness = nextBrightness;
    ledcWrite(0, brightness);
    reply(200, "시계 설정을 저장했어요.");
  });
  server.on("/api/finish", HTTP_POST, []() {
    if (!allowRequest(true)) return;
    if (attempt != Attempt::None || WiFi.status() != WL_CONNECTED ||
        connectionState == "storage_error") {
      reply(409, "와이파이 연결과 저장을 마친 뒤 눌러주세요.");
      return;
    }
    closeRequested = millis();
    reply(200, "설정을 마쳤어요. 휴대폰을 평소 와이파이에 다시 연결해주세요.");
  });
  server.onNotFound([]() {
    if (!allowRequest()) return;
    // HTTP captive-portal probes are redirected. HTTPS is not intercepted.
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "Open http://192.168.4.1/");
  });
}

void drawScreen() {
  display.fillScreen(TFT_BLACK);
  display.setTextColor(TFT_GREEN, TFT_BLACK);
  display.drawString(portalOpen ? "4DOWON / WI-FI SETUP" : "4DOWON / CLOCK", 8, 5, 2);
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  if (portalOpen) {
    display.drawString(apName, 8, 30, 2);
    display.drawString("PW: " + apPassword, 8, 51, 2);
    display.drawString("http://192.168.4.1", 8, 72, 2);
    String status = "Open settings on your phone";
    if (connectionState == "connecting") status = "Connecting...";
    else if (connectionState == "failed") status = "Failed - retry on your phone";
    else if (connectionState == "storage_error") status = "Storage error - retry";
    else if (WiFi.status() == WL_CONNECTED) status = "Connected! Tap Finish on phone";
    display.drawString(status, 8, 106, 2);
    return;
  }
  struct tm local;
  if (getLocalTime(&local, 0)) {
    char clock[10], date[28];
    strftime(clock, sizeof(clock), use24Hour ? "%H:%M" : "%I:%M", &local);
    strftime(date, sizeof(date), use24Hour ? "%Y.%m.%d  KST" : "%Y.%m.%d  %p KST", &local);
    display.drawString(clock, 39, 30, 7);
    display.drawString(date, 8, 85, 2);
  } else {
    display.drawString("Waiting for internet time", 8, 44, 2);
  }
  display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  display.drawString("Hold IO35 for 3s: Wi-Fi setup", 8, 111, 2);
}

void setup() {
  Serial.begin(115200);
  pinMode(SETUP_BUTTON, INPUT); // IO35 has an external pull-up on the original board.
  display.init();
  display.setRotation(1);
  ledcSetup(0, 5000, 8);
  ledcAttachPin(TFT_BL, 0);
  if (!preferences.begin("clock-setup", false)) {
    display.fillScreen(TFT_BLACK);
    display.drawString("NVS error. Reset / erase flash.", 5, 50, 2);
    ledcWrite(0, 180);
    while (true) delay(1000);
  }
  use24Hour = preferences.getBool("24hour", true);
  brightness = preferences.getUChar("brightness", 180);
  ledcWrite(0, brightness);
  StaticJsonDocument<384> credentials;
  if (!deserializeJson(credentials, preferences.getString("wifi", "{}"))) {
    savedSsid = credentials["ssid"] | "";
    savedPassword = credentials["password"] | "";
  }
  WiFi.persistent(false); // Credentials are committed explicitly after a successful trial.
  WiFi.setHostname("4dowon-clock");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  char suffix[7];
  snprintf(suffix, sizeof(suffix), "%06lX", static_cast<unsigned long>(ESP.getEfuseMac() & 0xffffff));
  apName = "4DOWON-Setup-" + String(suffix);
  apPassword = preferences.getString("apPassword", "");
  if (apPassword.length() < 8) {
    apPassword = randomKey().substring(0, 12);
    preferences.putString("apPassword", apPassword);
  }
  configureRoutes();
  if (savedSsid.isEmpty()) {
    message = "집이나 작업실의 2.4GHz 와이파이를 선택해주세요.";
    openPortal();
  } else {
    queueConnection(savedSsid, savedPassword, false);
  }
  drawScreen();
}

void loop() {
  const uint32_t now = millis();
  if (portalOpen) {
    dns.processNextRequest();
    server.handleClient();
    const uint32_t afterRequest = millis();
    if ((closeRequested && afterRequest - closeRequested >= 3000) ||
        (WiFi.status() == WL_CONNECTED && attempt == Attempt::None &&
         connectionState != "storage_error" && afterRequest - portalTouched >= PORTAL_TIMEOUT)) {
      closePortal();
    }
  }
  updateConnection();
  if (digitalRead(SETUP_BUTTON) == LOW) {
    if (!buttonSince) buttonSince = now;
    if (!buttonHandled && now - buttonSince >= 3000) {
      openPortal();
      buttonHandled = true;
    }
  } else {
    buttonSince = 0;
    buttonHandled = false;
  }
  static uint32_t lastDraw = 0;
  if (now - lastDraw >= 1000) {
    drawScreen();
    lastDraw = now;
  }
  delay(2);
}
