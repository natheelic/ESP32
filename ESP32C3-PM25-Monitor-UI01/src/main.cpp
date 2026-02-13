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
void drawPortalScreen();
void drawPM25Screen();
void drawLoadingScreen();
void drawErrorScreen();
const char *getAQILevel(int aqi);
void fetchAirQuality();

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
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_helvB10_tr);
  u8g2.drawStr(10, 16, "PM2.5 Monitor");
  u8g2.drawHLine(5, 20, 118);
  u8g2.setFont(u8g2_font_helvR08_tr);
  u8g2.drawStr(20, 40, "Starting up...");
  u8g2.drawStr(15, 55, "ESP32-C3 Super Mini");
  u8g2.sendBuffer();

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
    return "Unhealthy*";
  if (aqi <= 200)
    return "Unhealthy";
  if (aqi <= 300)
    return "Very Unhealthy";
  return "Hazardous";
}

// ════════════════════════════════════════════════════════════
//  DRAW — PM2.5 Main Screen
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

  // ─── Header ───
  u8g2.setFont(u8g2_font_helvB08_tr);
  u8g2.drawStr(4, 10, "PM2.5 Monitor");

  // WiFi RSSI indicator
  char rssi[12];
  snprintf(rssi, sizeof(rssi), "%ddBm", WiFi.RSSI());
  int rssiWidth = u8g2.getStrWidth(rssi);
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(128 - rssiWidth - 2, 8, rssi);

  u8g2.drawHLine(0, 13, 128);

  // ─── PM2.5 Value (large) ───
  char pm25Str[8];
  snprintf(pm25Str, sizeof(pm25Str), "%d", localPM25);

  u8g2.setFont(u8g2_font_logisoso26_tn); // ตัวเลขขนาดใหญ่
  int numWidth = u8g2.getStrWidth(pm25Str);
  u8g2.drawStr((80 - numWidth) / 2, 46, pm25Str);

  // Unit label
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(68, 22, "ug/m3");

  // ─── AQI Level ───
  u8g2.setFont(u8g2_font_helvB08_tr);
  const char *level = getAQILevel(localAQI);
  u8g2.drawStr(70, 36, level);

  // AQI number
  char aqiStr[16];
  snprintf(aqiStr, sizeof(aqiStr), "AQI:%d", localAQI);
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(70, 46, aqiStr);

  u8g2.drawHLine(0, 50, 128);

  // ─── Bottom: City + Time ───
  u8g2.setFont(u8g2_font_5x7_tr);

  // City name (truncate if too long)
  char displayCity[20];
  strncpy(displayCity, localCity, 19);
  displayCity[19] = '\0';
  u8g2.drawStr(2, 62, displayCity);

  // Time (HH:MM only)
  if (strlen(localTime) >= 16) {
    char timeOnly[6];
    strncpy(timeOnly, localTime + 11, 5);
    timeOnly[5] = '\0';
    int tw = u8g2.getStrWidth(timeOnly);
    u8g2.drawStr(128 - tw - 2, 62, timeOnly);
  }

  u8g2.sendBuffer();
}

// ════════════════════════════════════════════════════════════
//  DRAW — Loading Screen
// ════════════════════════════════════════════════════════════
void drawLoadingScreen() {
  static int d = 0;
  d = (d + 1) % 4;
  char dots[5] = "";
  for (int i = 0; i < d; i++)
    strcat(dots, ".");

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_helvB10_tr);
  u8g2.drawStr(10, 16, "PM2.5 Monitor");
  u8g2.drawHLine(5, 20, 118);

  u8g2.setFont(u8g2_font_helvR08_tr);
  u8g2.drawStr(10, 38, "Fetching data");

  char loadBuf[24];
  snprintf(loadBuf, sizeof(loadBuf), "Please wait%s", dots);
  u8g2.drawStr(10, 52, loadBuf);

  u8g2.sendBuffer();
}

// ════════════════════════════════════════════════════════════
//  DRAW — Error Screen
// ════════════════════════════════════════════════════════════
void drawErrorScreen() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_helvB10_tr);
  u8g2.drawStr(10, 16, "PM2.5 Monitor");
  u8g2.drawHLine(5, 20, 118);

  u8g2.setFont(u8g2_font_helvR08_tr);
  u8g2.drawStr(10, 36, "API Error!");
  u8g2.drawStr(10, 50, "Retrying in 5 min...");

  // แสดง WiFi status
  char rssi[24];
  snprintf(rssi, sizeof(rssi), "WiFi: %d dBm", WiFi.RSSI());
  u8g2.drawStr(10, 63, rssi);

  u8g2.sendBuffer();
}

// ════════════════════════════════════════════════════════════
//  DRAW — WiFi Config Portal screen
// ════════════════════════════════════════════════════════════
void drawPortalScreen() {
  static int d = 0;
  d = (d + 1) % 4;
  char dots[5] = "";
  for (int i = 0; i < d; i++)
    strcat(dots, ".");

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_helvB10_tr);
  u8g2.drawStr(14, 12, "WiFi Setup");
  u8g2.drawHLine(5, 16, 118);

  u8g2.setFont(u8g2_font_helvR08_tr);
  u8g2.drawStr(4, 28, "1. Connect to WiFi:");
  u8g2.setFont(u8g2_font_helvB08_tr);
  char apBuf[32];
  snprintf(apBuf, sizeof(apBuf), "   %s", AP_NAME);
  u8g2.drawStr(4, 39, apBuf);
  u8g2.setFont(u8g2_font_helvR08_tr);
  u8g2.drawStr(4, 50, "2. Open: 192.168.4.1");

  char waitBuf[24];
  snprintf(waitBuf, sizeof(waitBuf), "Waiting for config%s", dots);
  u8g2.drawStr(10, 63, waitBuf);
  u8g2.sendBuffer();
}
