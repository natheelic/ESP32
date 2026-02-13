/*
 * ESP32-C3 Super Mini — PM2.5 Air Quality Monitor
 * OLED 128×64 + WiFiManager + FreeRTOS + AQICN API
 *
 * ★ Version for ESP32-C3 Super Mini
 *   - WiFi.mode(WIFI_STA) before setTxPower (required!)
 *   - TX power reduced to 8.5dBm for PCB antenna
 *   - Re-apply TX power after every WiFi state change
 *
 * OLED I2C Wiring:
 *   SDA -> GPIO 5
 *   SCL -> GPIO 6
 *
 * WiFi Setup:
 *   บูตครั้งแรก → สร้าง AP "ESP32C3-Setup"
 *   เชื่อมต่อ AP → เปิด 192.168.4.1 → เลือก WiFi
 *
 * Air Quality Data:
 *   ใช้ AQICN API (api.waqi.info) ดึงค่า PM2.5
 *   อัพเดททุก 5 นาที
 *
 * FreeRTOS Tasks:
 *   - taskWiFi:    จัดการ WiFi connection + reconnect
 *   - taskDisplay: อัพเดท OLED แสดงค่า PM2.5
 *   - taskMain:    ดึงข้อมูล PM2.5 จาก API
 */

#include <Arduino.h>
#include <ArduinoJson.h> // https://github.com/bblanchon/ArduinoJson
#include <HTTPClient.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <WiFiManager.h> // https://github.com/tzapu/WiFiManager
#include <Wire.h>
#include <math.h>

// ─── SDA & SCL ──────────────────────────────────────────────
#define SDA_PIN 5
#define SCL_PIN 6

// ─── WiFiManager Configuration ──────────────────────────────
const char *AP_NAME = "ESP32C3-Setup";
const char *AP_PASS = "";
const int PORTAL_TIMEOUT = 120;

// ─── AQICN API ──────────────────────────────────────────────
const char *AQICN_TOKEN = "666746c307fa56b52b4475e9364cdf1c3ead41bf";
const char *AQICN_CITY = "bangkok";          // เปลี่ยนเป็นเมืองที่ต้องการ
const unsigned long FETCH_INTERVAL = 300000; // 5 นาที (ms)

// ─── OLED Display ───────────────────────────────────────────
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// ─── Shared State ───────────────────────────────────────────
volatile bool wifiConnected = false;
volatile bool portalActive = false;

// ─── PM2.5 Data (shared between tasks) ──────────────────────
SemaphoreHandle_t dataMutex;
int pm25Value = -1; // -1 = ยังไม่มีข้อมูล
int aqiValue = -1;
char cityName[32] = "";
char updateTime[24] = "";
bool dataReady = false;
bool fetchError = false;

// ─── Function Prototypes ────────────────────────────────────
void taskWiFi(void *param);
void taskDisplay(void *param);
void taskMain(void *param);
void drawHeader(const char *title);
void drawWiFiBars(int x, int y);
void drawPortalScreen();
void drawPM25Screen();
void drawLoadingScreen();
void drawErrorScreen();
void drawBootScreen();
const char *getAQILevel(int aqi);
void fetchAirQuality();

// ─── Animation Frame Counter ────────────────────────────────
static unsigned long frameCount = 0;

// ════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  unsigned long t = millis();
  while (!Serial && (millis() - t < 3000))
    delay(10);
  delay(500);

  Serial.println("\n[BOOT] ESP32-C3 Super Mini");
  Serial.println("[BOOT] PM2.5 Air Quality Monitor");

  // Create mutex for shared data
  dataMutex = xSemaphoreCreateMutex();

  // Initialize I2C & OLED
  Wire.begin(SDA_PIN, SCL_PIN);
  u8g2.begin();
  u8g2.setContrast(200);

  // Boot screen
  drawBootScreen();

  // Create FreeRTOS tasks
  xTaskCreate(taskWiFi, "WiFi", 8192, NULL, 2, NULL);
  xTaskCreate(taskDisplay, "Display", 4096, NULL, 1, NULL);
  xTaskCreate(taskMain, "Main", 8192, NULL, 1, NULL);
}

void loop() { vTaskDelay(portMAX_DELAY); }

// ════════════════════════════════════════════════════════════
//  WiFi TASK — ★ ESP32-C3 Super Mini WiFi Fix
// ════════════════════════════════════════════════════════════
void taskWiFi(void *param) {
  WiFiManager wm;

  // ★ ESP32-C3 Fix: ต้อง WiFi.mode() ก่อน setTxPower
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  Serial.println("[WiFi] TX power 8.5dBm (ESP32-C3 fix)");

  wm.setConfigPortalTimeout(PORTAL_TIMEOUT);
  wm.setConnectRetries(3);
  wm.setConnectTimeout(20);
  wm.setClass("invert");

  wm.setAPCallback([](WiFiManager *wm) {
    Serial.printf("[WiFi] Portal started — AP: %s\n", AP_NAME);
    portalActive = true;
  });
  wm.setSaveConfigCallback([]() { portalActive = false; });

  Serial.println("[WiFi] Connecting...");
  if (!wm.autoConnect(AP_NAME, AP_PASS)) {
    Serial.println("[WiFi] Portal timed out. Restarting...");
    ESP.restart();
  }

  // ★ Re-apply TX power หลัง autoConnect
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  wifiConnected = true;
  portalActive = false;
  Serial.printf("[WiFi] Connected! IP: %s\n",
                WiFi.localIP().toString().c_str());

  // Monitor + reconnect
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      wifiConnected = false;
      Serial.println("[WiFi] Disconnected! Reconnecting...");

      WiFi.disconnect(true);
      vTaskDelay(pdMS_TO_TICKS(1000));
      WiFi.mode(WIFI_STA);
      WiFi.setTxPower(WIFI_POWER_8_5dBm);
      WiFi.begin();

      int attempts = 0;
      while (WiFi.status() != WL_CONNECTED && attempts < 40) {
        vTaskDelay(pdMS_TO_TICKS(500));
        attempts++;
      }

      if (WiFi.status() == WL_CONNECTED) {
        WiFi.setTxPower(WIFI_POWER_8_5dBm);
        wifiConnected = true;
        Serial.printf("\n[WiFi] Reconnected!\n");
      } else {
        portalActive = true;
        if (!wm.startConfigPortal(AP_NAME, AP_PASS)) {
          ESP.restart();
        }
        WiFi.setTxPower(WIFI_POWER_8_5dBm);
        wifiConnected = true;
        portalActive = false;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

// ════════════════════════════════════════════════════════════
//  DISPLAY TASK — แสดงค่า PM2.5 บน OLED
// ════════════════════════════════════════════════════════════
void taskDisplay(void *param) {
  // รอ WiFi เชื่อมต่อก่อน
  while (!wifiConnected && !portalActive) {
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  for (;;) {
    frameCount++;

    // แสดงหน้า WiFi Setup ถ้า portal เปิดอยู่
    if (portalActive) {
      drawPortalScreen();
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    if (fetchError) {
      drawErrorScreen();
    } else if (!dataReady) {
      drawLoadingScreen();
    } else {
      drawPM25Screen();
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// ════════════════════════════════════════════════════════════
//  MAIN TASK — ดึงข้อมูล PM2.5 จาก AQICN API
// ════════════════════════════════════════════════════════════
void taskMain(void *param) {
  while (!wifiConnected) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }

  Serial.println("[Main] WiFi ready! Starting PM2.5 monitor...");

  for (;;) {
    if (wifiConnected) {
      fetchAirQuality();
    }

    // รอ 5 นาทีก่อนดึงข้อมูลใหม่
    vTaskDelay(pdMS_TO_TICKS(FETCH_INTERVAL));
  }
}

// ════════════════════════════════════════════════════════════
//  FETCH — ดึงข้อมูลคุณภาพอากาศจาก AQICN API
// ════════════════════════════════════════════════════════════
void fetchAirQuality() {
  HTTPClient http;

  // สร้าง URL: https://api.waqi.info/feed/{city}/?token=xxx
  char url[128];
  snprintf(url, sizeof(url), "https://api.waqi.info/feed/%s/?token=%s",
           AQICN_CITY, AQICN_TOKEN);

  Serial.printf("[API] Fetching: %s\n", url);

  http.begin(url);
  http.setTimeout(10000);
  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    Serial.printf("[API] Response (%d bytes)\n", payload.length());

    // Parse JSON
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);

    if (error) {
      Serial.printf("[API] JSON parse error: %s\n", error.c_str());
      xSemaphoreTake(dataMutex, portMAX_DELAY);
      fetchError = true;
      xSemaphoreGive(dataMutex);
      http.end();
      return;
    }

    const char *status = doc["status"];
    if (status && strcmp(status, "ok") == 0) {
      JsonObject data = doc["data"];

      xSemaphoreTake(dataMutex, portMAX_DELAY);

      // AQI value
      aqiValue = data["aqi"] | -1;

      // PM2.5 value from iaqi
      if (data["iaqi"]["pm25"]["v"].is<float>()) {
        pm25Value = (int)data["iaqi"]["pm25"]["v"].as<float>();
      } else if (data["iaqi"]["pm25"]["v"].is<int>()) {
        pm25Value = data["iaqi"]["pm25"]["v"].as<int>();
      }

      // City name
      const char *city = data["city"]["name"];
      if (city) {
        strncpy(cityName, city, sizeof(cityName) - 1);
        cityName[sizeof(cityName) - 1] = '\0';
        // ตัดชื่อเมืองให้สั้นลง (แสดงเฉพาะส่วนแรก)
        char *comma = strchr(cityName, ',');
        if (comma)
          *comma = '\0';
      }

      // Update time
      const char *timeStr = data["time"]["s"];
      if (timeStr) {
        strncpy(updateTime, timeStr, sizeof(updateTime) - 1);
        updateTime[sizeof(updateTime) - 1] = '\0';
      }

      dataReady = true;
      fetchError = false;
      xSemaphoreGive(dataMutex);

      Serial.printf("[API] AQI: %d | PM2.5: %d | City: %s\n", aqiValue,
                    pm25Value, cityName);
    } else {
      Serial.println("[API] Status not OK");
      xSemaphoreTake(dataMutex, portMAX_DELAY);
      fetchError = true;
      xSemaphoreGive(dataMutex);
    }
  } else {
    Serial.printf("[API] HTTP error: %d\n", httpCode);
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    fetchError = true;
    xSemaphoreGive(dataMutex);
  }

  http.end();
}

// ════════════════════════════════════════════════════════════
//  AQI Level — แปลงค่า AQI เป็นระดับ
// ════════════════════════════════════════════════════════════
const char *getAQILevel(int aqi) {
  if (aqi <= 50)
    return "Good";
  if (aqi <= 100)
    return "Moderate";
  if (aqi <= 150)
    return "Sensitive Grp";
  if (aqi <= 200)
    return "Unhealthy";
  if (aqi <= 300)
    return "Very Unhealthy";
  return "Hazardous";
}

// ════════════════════════════════════════════════════════════
//  DRAW HELPER — Inverted Header Bar (shared by all screens)
// ════════════════════════════════════════════════════════════
void drawHeader(const char *title) {
  // ▓▓▓ Inverted header — white bar, black text ▓▓▓
  u8g2.drawBox(0, 0, 128, 14);
  u8g2.setDrawColor(0); // black on white
  u8g2.setFont(u8g2_font_helvB08_tr);
  u8g2.drawStr(4, 11, title);
  u8g2.setDrawColor(1); // reset to white on black
}

// ════════════════════════════════════════════════════════════
//  DRAW HELPER — WiFi Signal Bars Icon
// ════════════════════════════════════════════════════════════
void drawWiFiBars(int x, int y) {
  // วาด WiFi signal bars ใน header (inverted = black on white)
  int rssiVal = WiFi.RSSI();
  int bars = 0;
  if (rssiVal > -50)
    bars = 4;
  else if (rssiVal > -60)
    bars = 3;
  else if (rssiVal > -70)
    bars = 2;
  else if (rssiVal > -85)
    bars = 1;

  u8g2.setDrawColor(0); // black (inside inverted header)
  for (int i = 0; i < 4; i++) {
    int bx = x + i * 4;
    int bh = 3 + i * 2; // bar heights: 3, 5, 7, 9
    int by = y - bh + 1;
    if (i < bars)
      u8g2.drawBox(bx, by, 3, bh); // filled = active
    else
      u8g2.drawFrame(bx, by, 3, bh); // outline = inactive
  }
  u8g2.setDrawColor(1); // reset
}

// ════════════════════════════════════════════════════════════
//  DRAW — Boot Screen
// ════════════════════════════════════════════════════════════
void drawBootScreen() {
  u8g2.clearBuffer();

  // Outer frame
  u8g2.drawRFrame(0, 0, 128, 64, 4);
  u8g2.drawRFrame(2, 2, 124, 60, 3);

  // Title
  u8g2.setFont(u8g2_font_helvB12_tr);
  const char *t1 = "PM2.5";
  int w1 = u8g2.getStrWidth(t1);
  u8g2.drawStr((128 - w1) / 2, 24, t1);

  u8g2.setFont(u8g2_font_helvB10_tr);
  const char *t2 = "Air Monitor";
  int w2 = u8g2.getStrWidth(t2);
  u8g2.drawStr((128 - w2) / 2, 40, t2);

  // Decorative line
  u8g2.drawHLine(20, 44, 88);

  // Subtitle
  u8g2.setFont(u8g2_font_5x7_tr);
  const char *t3 = "ESP32-C3 Super Mini";
  int w3 = u8g2.getStrWidth(t3);
  u8g2.drawStr((128 - w3) / 2, 56, t3);

  u8g2.sendBuffer();
}

// ════════════════════════════════════════════════════════════
//  DRAW — PM2.5 Main Screen ★ NEW UI ★
//
//  ┌────────────────────────────────┐
//  │▓▓ AIR QUALITY       ▂▄▆█ ▓▓│  <- Inverted header + WiFi bars
//  ├────────────────────────────────┤
//  │                   │  ug/m3    │
//  │      156          │  ──────── │  <- Large PM2.5 + info panel
//  │                   │  AQI: 156 │
//  │                   │  Good     │
//  ├────────────────────────────────┤
//  │ [████████████░░░░░░░░]  12:30 │  <- Gauge bar + time
//  │ ● Good              Bangkok   │  <- Status dot + city
//  └────────────────────────────────┘
//
// ════════════════════════════════════════════════════════════
void drawPM25Screen() {
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  int localPM25 = pm25Value;
  int localAQI = aqiValue;
  char localCity[32];
  char localTime[24];
  strncpy(localCity, cityName, sizeof(localCity));
  strncpy(localTime, updateTime, sizeof(localTime));
  xSemaphoreGive(dataMutex);

  u8g2.clearBuffer();

  // ═══ HEADER BAR (inverted) ═══
  drawHeader("AIR QUALITY");
  drawWiFiBars(110, 11);

  // ═══ LEFT: Large PM2.5 Number ═══
  char pm25Str[8];
  snprintf(pm25Str, sizeof(pm25Str), "%d", localPM25);

  u8g2.setFont(u8g2_font_logisoso28_tn);
  int numW = u8g2.getStrWidth(pm25Str);
  int numX = (78 - numW) / 2; // center in left 78px zone
  if (numX < 2)
    numX = 2;
  u8g2.drawStr(numX, 46, pm25Str);

  // ═══ RIGHT: Info Panel ═══
  // Vertical divider
  u8g2.drawVLine(82, 15, 33);

  // Unit
  u8g2.setFont(u8g2_font_helvR08_tr);
  u8g2.drawStr(87, 24, "ug/m3");

  // Thin separator
  u8g2.drawHLine(87, 27, 38);

  // AQI number
  char aqiStr[12];
  snprintf(aqiStr, sizeof(aqiStr), "AQI: %d", localAQI);
  u8g2.setFont(u8g2_font_helvB08_tr);
  u8g2.drawStr(87, 38, aqiStr);

  // AQI Level text
  const char *level = getAQILevel(localAQI);
  u8g2.setFont(u8g2_font_helvR08_tr);
  u8g2.drawStr(87, 48, level);

  // ═══ BOTTOM SECTION ═══
  u8g2.drawHLine(0, 50, 128);

  // ── Gauge Bar ──
  // Background frame (rounded)
  u8g2.drawRFrame(2, 53, 96, 9, 2);

  // Fill proportional to AQI (max 500)
  int fillW = map(constrain(localAQI, 0, 500), 0, 500, 0, 92);
  if (fillW > 0) {
    u8g2.drawBox(4, 55, fillW, 5);
  }

  // Tick marks on gauge (at 100, 200, 300)
  for (int tick = 100; tick <= 300; tick += 100) {
    int tx = map(tick, 0, 500, 4, 96);
    u8g2.drawPixel(tx, 53);
    u8g2.drawPixel(tx, 61);
  }

  // ── Time (HH:MM) ──
  if (strlen(localTime) >= 16) {
    char timeOnly[6];
    strncpy(timeOnly, localTime + 11, 5);
    timeOnly[5] = '\0';
    u8g2.setFont(u8g2_font_helvB08_tr);
    int tw = u8g2.getStrWidth(timeOnly);
    u8g2.drawStr(128 - tw - 1, 62, timeOnly);
  }

  u8g2.sendBuffer();
}

// ════════════════════════════════════════════════════════════
//  DRAW — Loading Screen (animated spinner)
// ════════════════════════════════════════════════════════════
void drawLoadingScreen() {
  u8g2.clearBuffer();

  // ═══ HEADER ═══
  drawHeader("AIR QUALITY");

  // ── Animated spinner (rotating segments) ──
  int cx = 64, cy = 36;
  int r = 10;
  int phase = frameCount % 8;

  // Draw 8 dots in a circle, highlight current phase
  for (int i = 0; i < 8; i++) {
    float angle = i * 0.785398; // 45 degrees in radians
    int dx = cx + (int)(r * cos(angle));
    int dy = cy + (int)(r * sin(angle));

    // Current dot and trailing dots are bigger
    int dist = (i - phase + 8) % 8;
    if (dist == 0) {
      u8g2.drawDisc(dx, dy, 2); // current = large filled
    } else if (dist <= 2) {
      u8g2.drawCircle(dx, dy, 2); // trailing = outline
    } else {
      u8g2.drawPixel(dx, dy); // rest = tiny dot
    }
  }

  // ── Text ──
  u8g2.setFont(u8g2_font_helvR08_tr);
  const char *msg = "Fetching air quality...";
  int mw = u8g2.getStrWidth(msg);
  u8g2.drawStr((128 - mw) / 2, 58, msg);

  u8g2.sendBuffer();
}

// ════════════════════════════════════════════════════════════
//  DRAW — Error Screen
// ════════════════════════════════════════════════════════════
void drawErrorScreen() {
  u8g2.clearBuffer();

  // ═══ HEADER ═══
  drawHeader("AIR QUALITY");

  // ── Warning icon: triangle with ! ──
  // Triangle outline
  u8g2.drawTriangle(64, 20, 50, 42, 78, 42);
  // Inner triangle (slightly smaller for outline effect)
  u8g2.setDrawColor(0);
  u8g2.drawTriangle(64, 24, 53, 40, 75, 40);
  u8g2.setDrawColor(1);
  // Exclamation mark
  u8g2.drawBox(63, 28, 3, 6); // stem
  u8g2.drawBox(63, 36, 3, 3); // dot

  // ── Text ──
  u8g2.setFont(u8g2_font_helvB08_tr);
  const char *err = "Connection Error";
  int ew = u8g2.getStrWidth(err);
  u8g2.drawStr((128 - ew) / 2, 54, err);

  u8g2.setFont(u8g2_font_5x7_tr);
  const char *retry = "Auto-retry in 5 min";
  int rw = u8g2.getStrWidth(retry);
  u8g2.drawStr((128 - rw) / 2, 63, retry);

  // WiFi bars in header
  drawWiFiBars(110, 11);

  u8g2.sendBuffer();
}

// ════════════════════════════════════════════════════════════
//  DRAW — WiFi Config Portal screen
// ════════════════════════════════════════════════════════════
void drawPortalScreen() {
  u8g2.clearBuffer();

  // ═══ HEADER ═══
  drawHeader("WIFI SETUP");

  // ── WiFi icon: concentric arcs ──
  int cx = 22, cy = 38;
  // Animate arcs
  int phase = frameCount % 4;
  if (phase >= 1)
    u8g2.drawCircle(cx, cy, 6, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
  if (phase >= 2)
    u8g2.drawCircle(cx, cy, 10, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
  if (phase >= 3)
    u8g2.drawCircle(cx, cy, 14, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
  u8g2.drawDisc(cx, cy, 2); // center dot always visible

  // ── Instructions ──
  u8g2.setFont(u8g2_font_helvR08_tr);
  u8g2.drawStr(42, 24, "Connect to:");

  u8g2.setFont(u8g2_font_helvB08_tr);
  char apBuf[20];
  snprintf(apBuf, sizeof(apBuf), "%s", AP_NAME);
  u8g2.drawStr(42, 36, apBuf);

  u8g2.setFont(u8g2_font_helvR08_tr);
  u8g2.drawStr(42, 48, "Open browser:");

  u8g2.setFont(u8g2_font_helvB08_tr);
  u8g2.drawStr(42, 60, "192.168.4.1");

  u8g2.sendBuffer();
}
