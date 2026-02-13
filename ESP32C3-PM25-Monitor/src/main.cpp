/*
 * ESP32-C3 Super Mini — Air Quality Monitor
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
 *   ใช้ AQICN API (api.waqi.info)
 *   แสดง 5 มลพิษ: PM2.5, PM10, O₃, NO₂, CO
 *   สลับหน้าจออัตโนมัติทุก 5 วินาที
 *   อัพเดทข้อมูลทุก 5 นาที
 *
 * Display Pages:
 *   Page 0: Overview (แสดงทุกค่าในหน้าเดียว)
 *   Page 1: PM2.5 (ฝุ่นละอองขนาดเล็ก)
 *   Page 2: PM10  (อนุภาคหยาบ)
 *   Page 3: O₃    (โอโซน)
 *   Page 4: NO₂   (ไนโตรเจนไดออกไซด์)
 *   Page 5: CO    (คาร์บอนมอนอกไซด์)
 *
 * FreeRTOS Tasks:
 *   - taskWiFi:    จัดการ WiFi connection + reconnect
 *   - taskDisplay: อัพเดท OLED สลับหน้าจอมลพิษ
 *   - taskMain:    ดึงข้อมูลจาก API
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

// ─── Pollutant Data (shared between tasks) ──────────────────
SemaphoreHandle_t dataMutex;

struct PollutantData {
  int value;            // ค่าที่วัดได้ (-1 = ไม่มีข้อมูล)
  const char *name;     // ชื่อย่อ เช่น "PM2.5"
  const char *fullName; // ชื่อเต็ม
  const char *unit;     // หน่วย
  int maxScale;         // ค่าสูงสุดสำหรับ gauge bar
};

// Index: 0=PM2.5, 1=PM10, 2=O3, 3=NO2, 4=CO
#define NUM_POLLUTANTS 5
PollutantData pollutants[NUM_POLLUTANTS] = {
    {-1, "PM2.5", "Fine Dust", "ug/m3", 500},
    {-1, "PM10", "Coarse Dust", "ug/m3", 500},
    {-1, "O3", "Ozone", "ppb", 300},
    {-1, "NO2", "Nitrogen Dioxide", "ppb", 400},
    {-1, "CO", "Carbon Monoxide", "ppm", 200},
};

int aqiValue = -1;
char cityName[32] = "";
char updateTime[24] = "";
bool dataReady = false;
bool fetchError = false;

// ─── Page Cycling ───────────────────────────────────────────
#define NUM_PAGES (NUM_POLLUTANTS + 1) // overview + 5 pollutants
#define PAGE_DURATION 5                // วินาทีต่อหน้า
int currentPage = 0;                   // 0 = overview, 1-5 = each pollutant

// ─── Function Prototypes ────────────────────────────────────
void taskWiFi(void *param);
void taskDisplay(void *param);
void taskMain(void *param);
void drawHeader(const char *title);
void drawWiFiBars(int x, int y);
void drawPageDots(int current, int total);
void drawPortalScreen();
void drawOverviewPage();
void drawPollutantPage(int index);
void drawLoadingScreen();
void drawErrorScreen();
void drawBootScreen();
const char *getAQILevel(int aqi);
const char *getPollutantLevel(int index, int value);
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
//  DISPLAY TASK — สลับหน้าจอมลพิษอัตโนมัติ
// ════════════════════════════════════════════════════════════
void taskDisplay(void *param) {
  // รอ WiFi เชื่อมต่อก่อน
  while (!wifiConnected && !portalActive) {
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  unsigned long lastPageChange = 0;

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
      // ═══ Auto page cycling ═══
      unsigned long now = millis();
      if (now - lastPageChange >= (unsigned long)PAGE_DURATION * 1000) {
        currentPage = (currentPage + 1) % NUM_PAGES;
        lastPageChange = now;
      }

      if (currentPage == 0) {
        drawOverviewPage();
      } else {
        drawPollutantPage(currentPage - 1);
      }
    }

    vTaskDelay(pdMS_TO_TICKS(500)); // 500ms for smoother transitions
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

      // Parse all pollutants from iaqi
      const char *apiKeys[] = {"pm25", "pm10", "o3", "no2", "co"};
      for (int i = 0; i < NUM_POLLUTANTS; i++) {
        JsonVariant val = data["iaqi"][apiKeys[i]]["v"];
        if (!val.isNull()) {
          pollutants[i].value = (int)val.as<float>();
        } else {
          pollutants[i].value = -1;
        }
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

      Serial.printf("[API] AQI:%d PM2.5:%d PM10:%d O3:%d NO2:%d CO:%d\n",
                    aqiValue, pollutants[0].value, pollutants[1].value,
                    pollutants[2].value, pollutants[3].value,
                    pollutants[4].value);
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
//  Pollutant Level — แปลงค่ามลพิษแต่ละชนิดเป็นระดับ
// ════════════════════════════════════════════════════════════
const char *getPollutantLevel(int index, int value) {
  if (value < 0)
    return "N/A";
  // ใช้เกณฑ์ AQI breakpoints โดยประมาณ
  switch (index) {
  case 0: // PM2.5 (ug/m3)
    if (value <= 12)
      return "Good";
    if (value <= 35)
      return "Moderate";
    if (value <= 55)
      return "Sensitive";
    if (value <= 150)
      return "Unhealthy";
    if (value <= 250)
      return "Very Bad";
    return "Hazardous";
  case 1: // PM10 (ug/m3)
    if (value <= 54)
      return "Good";
    if (value <= 154)
      return "Moderate";
    if (value <= 254)
      return "Sensitive";
    if (value <= 354)
      return "Unhealthy";
    return "Very Bad";
  case 2: // O3 (ppb)
    if (value <= 54)
      return "Good";
    if (value <= 70)
      return "Moderate";
    if (value <= 85)
      return "Sensitive";
    if (value <= 105)
      return "Unhealthy";
    return "Very Bad";
  case 3: // NO2 (ppb)
    if (value <= 53)
      return "Good";
    if (value <= 100)
      return "Moderate";
    if (value <= 360)
      return "Sensitive";
    if (value <= 649)
      return "Unhealthy";
    return "Very Bad";
  case 4: // CO (ppm)
    if (value <= 4)
      return "Good";
    if (value <= 9)
      return "Moderate";
    if (value <= 12)
      return "Sensitive";
    if (value <= 15)
      return "Unhealthy";
    return "Very Bad";
  }
  return "N/A";
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

  u8g2.setDrawColor(0);
  for (int i = 0; i < 4; i++) {
    int bx = x + i * 4;
    int bh = 3 + i * 2;
    int by = y - bh + 1;
    if (i < bars)
      u8g2.drawBox(bx, by, 3, bh);
    else
      u8g2.drawFrame(bx, by, 3, bh);
  }
  u8g2.setDrawColor(1);
}

// ════════════════════════════════════════════════════════════
//  DRAW HELPER — Page Indicator Dots
// ════════════════════════════════════════════════════════════
void drawPageDots(int current, int total) {
  int dotSpacing = 8;
  int totalWidth = (total - 1) * dotSpacing;
  int startX = (128 - totalWidth) / 2;
  int y = 62;

  for (int i = 0; i < total; i++) {
    int x = startX + i * dotSpacing;
    if (i == current) {
      u8g2.drawDisc(x, y, 2); // filled = current page
    } else {
      u8g2.drawCircle(x, y, 1); // outline = other pages
    }
  }
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
  const char *t1 = "Air Quality";
  int w1 = u8g2.getStrWidth(t1);
  u8g2.drawStr((128 - w1) / 2, 24, t1);

  u8g2.setFont(u8g2_font_helvB10_tr);
  const char *t2 = "Monitor";
  int w2 = u8g2.getStrWidth(t2);
  u8g2.drawStr((128 - w2) / 2, 40, t2);

  u8g2.drawHLine(20, 44, 88);

  u8g2.setFont(u8g2_font_5x7_tr);
  const char *t3 = "ESP32-C3 Super Mini";
  int w3 = u8g2.getStrWidth(t3);
  u8g2.drawStr((128 - w3) / 2, 56, t3);

  u8g2.sendBuffer();
}

// ════════════════════════════════════════════════════════════
//  DRAW — Overview Page (แสดงทุกค่าในหน้าเดียว)
//
//  ┌───────────────────────────────┐  Y=0
//  │▓▓ OVERVIEW    AQI:156  ▂▄▆█ ▓│  Y=0-13  Header
//  ├───────────────────────────────┤  Y=14
//  │  PM2.5  156    PM10  85      │  Y=25
//  │  O3      42    NO2   18      │  Y=35
//  │  CO       3          12:30   │  Y=45
//  ├───────────────────────────────┤  Y=51
//  │     ● ○ ○ ○ ○ ○              │  Y=58
//  └───────────────────────────────┘  Y=63
//
// ════════════════════════════════════════════════════════════
void drawOverviewPage() {
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  int localAQI = aqiValue;
  int localVals[NUM_POLLUTANTS];
  for (int i = 0; i < NUM_POLLUTANTS; i++)
    localVals[i] = pollutants[i].value;
  char localTime[24];
  strncpy(localTime, updateTime, sizeof(localTime));
  xSemaphoreGive(dataMutex);

  u8g2.clearBuffer();

  // ═══ HEADER ═══
  drawHeader("OVERVIEW");
  drawWiFiBars(110, 11);

  // AQI badge in header
  if (localAQI >= 0) {
    char aqiBuf[12];
    snprintf(aqiBuf, sizeof(aqiBuf), "AQI:%d", localAQI);
    u8g2.setDrawColor(0);
    u8g2.setFont(u8g2_font_5x7_tr);
    int aw = u8g2.getStrWidth(aqiBuf);
    u8g2.drawStr(108 - aw, 10, aqiBuf);
    u8g2.setDrawColor(1);
  }

  // ═══ Pollutant Grid (2 columns × 3 rows) ═══
  // ใช้ font ขนาดเล็ก 5x7 เพื่อให้พอดีหน้าจอ
  int row1Y = 25;
  int row2Y = 35;
  int row3Y = 45;
  int col1X = 2;
  int col2X = 66;
  int valOffset = 34;

  // Helper: draw name + value
  auto drawItem = [&](int x, int y, int idx) {
    u8g2.setFont(u8g2_font_helvB08_tr);
    u8g2.drawStr(x, y, pollutants[idx].name);
    u8g2.setFont(u8g2_font_5x7_tr);
    char val[8];
    if (localVals[idx] >= 0)
      snprintf(val, sizeof(val), "%d", localVals[idx]);
    else
      snprintf(val, sizeof(val), "--");
    u8g2.drawStr(x + valOffset, y, val);
  };

  // Row 1: PM2.5 | PM10
  drawItem(col1X, row1Y, 0);
  drawItem(col2X, row1Y, 1);

  // Row 2: O3 | NO2
  drawItem(col1X, row2Y, 2);
  drawItem(col2X, row2Y, 3);

  // Row 3: CO | Time
  drawItem(col1X, row3Y, 4);

  // Time on right of row 3
  if (strlen(localTime) >= 16) {
    char timeOnly[6];
    strncpy(timeOnly, localTime + 11, 5);
    timeOnly[5] = '\0';
    u8g2.setFont(u8g2_font_5x7_tr);
    int tw = u8g2.getStrWidth(timeOnly);
    u8g2.drawStr(128 - tw - 2, row3Y, timeOnly);
  }

  // ═══ Separator + Page dots ═══
  u8g2.drawHLine(0, 51, 128);
  drawPageDots(0, NUM_PAGES);

  u8g2.sendBuffer();
}

// ════════════════════════════════════════════════════════════
//  DRAW — Individual Pollutant Page
//
//  ┌───────────────────────────────┐  Y=0
//  │▓▓ PM2.5  Fine Dust  ▂▄▆█ ▓▓│  Y=0-13  Header
//  ├──────────────────┬────────────┤  Y=14
//  │                  │  ug/m3     │  Y=25
//  │     156          │  ──────    │  Y=28
//  │                  │  Good      │  Y=38
//  │                  │  AQI: 156  │  Y=47
//  ├──────────────────┴────────────┤  Y=51
//  │ [████████░░░░]  ● ○ ○ ○ ○ ○  │  Y=53-61
//  └───────────────────────────────┘  Y=63
//
// ════════════════════════════════════════════════════════════
void drawPollutantPage(int index) {
  if (index < 0 || index >= NUM_POLLUTANTS)
    return;

  xSemaphoreTake(dataMutex, portMAX_DELAY);
  int localVal = pollutants[index].value;
  int localAQI = aqiValue;
  int localMaxScale = pollutants[index].maxScale;
  const char *localName = pollutants[index].name;
  const char *localFullName = pollutants[index].fullName;
  const char *localUnit = pollutants[index].unit;
  xSemaphoreGive(dataMutex);

  u8g2.clearBuffer();

  // ═══ HEADER with pollutant name ═══
  char headerBuf[24];
  snprintf(headerBuf, sizeof(headerBuf), "%s", localName);
  drawHeader(headerBuf);
  drawWiFiBars(110, 11);

  // Full name in header (right of title)
  u8g2.setDrawColor(0);
  u8g2.setFont(u8g2_font_5x7_tr);
  int fnW = u8g2.getStrWidth(localFullName);
  if (fnW < 70)
    u8g2.drawStr(108 - fnW, 10, localFullName);
  u8g2.setDrawColor(1);

  // ═══ MAIN: Large Value (Y=18 to Y=46) ═══
  if (localVal >= 0) {
    char valStr[8];
    snprintf(valStr, sizeof(valStr), "%d", localVal);

    u8g2.setFont(u8g2_font_logisoso28_tn);
    int numW = u8g2.getStrWidth(valStr);
    int numX = (78 - numW) / 2;
    if (numX < 2)
      numX = 2;
    u8g2.drawStr(numX, 46, valStr);
  } else {
    u8g2.setFont(u8g2_font_helvB12_tr);
    u8g2.drawStr(20, 36, "N/A");
  }

  // ═══ RIGHT: Info Panel (X=83 to X=127) ═══
  u8g2.drawVLine(82, 15, 35);

  // Unit
  u8g2.setFont(u8g2_font_helvR08_tr);
  u8g2.drawStr(87, 25, localUnit);

  // Separator line
  u8g2.drawHLine(87, 28, 38);

  // Level text
  const char *level = getPollutantLevel(index, localVal);
  u8g2.setFont(u8g2_font_helvB08_tr);
  u8g2.drawStr(87, 39, level);

  // AQI number
  if (localAQI >= 0) {
    char aqiStr[12];
    snprintf(aqiStr, sizeof(aqiStr), "AQI: %d", localAQI);
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(87, 48, aqiStr);
  }

  // ═══ BOTTOM: Gauge Bar + Page Dots (Y=51 to Y=63) ═══
  u8g2.drawHLine(0, 51, 128);

  // Gauge bar (left side: X=2 to X=70)
  u8g2.drawRFrame(2, 54, 68, 8, 2);
  if (localVal >= 0) {
    int fillW =
        map(constrain(localVal, 0, localMaxScale), 0, localMaxScale, 0, 64);
    if (fillW > 0) {
      u8g2.drawBox(4, 56, fillW, 4);
    }
  }

  // Page dots (right side: X=76 to X=126)
  int dotSpacing = 7;
  int dotsWidth = (NUM_PAGES - 1) * dotSpacing;
  int dotStartX = 76 + (52 - dotsWidth) / 2;
  int dotY = 58;
  for (int i = 0; i < NUM_PAGES; i++) {
    int dx = dotStartX + i * dotSpacing;
    if (i == index + 1)
      u8g2.drawDisc(dx, dotY, 2);
    else
      u8g2.drawCircle(dx, dotY, 1);
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
