/*
 * ESP32-C5 — OLED 128×64 + WiFiManager + FreeRTOS
 * Template (ไม่มีฟังก์ชันนาฬิกา — ใส่โค้ดเองได้)
 *
 * OLED I2C Wiring:
 *   SDA -> GPIO 2
 *   SCL -> GPIO 3
 *
 * WiFi Setup:
 *   บูตครั้งแรก → สร้าง AP "ESP32C5-Setup"
 *   เชื่อมต่อ AP → เปิด 192.168.4.1 → เลือก WiFi
 *
 * FreeRTOS Tasks:
 *   - taskWiFi:    จัดการ WiFi connection + reconnect
 *   - taskDisplay: อัพเดท OLED (ใส่โค้ดแสดงผลของคุณที่นี่)
 *   - taskMain:    งานหลัก (ใส่โค้ดของคุณที่นี่)
 */

#include <Arduino.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <WiFiManager.h> // https://github.com/tzapu/WiFiManager
#include <Wire.h>

// ─── SDA & SCL ──────────────────────────────────────────────
#define SDA_PIN 2
#define SCL_PIN 3

// ─── WiFiManager Configuration ──────────────────────────────
const char *AP_NAME = "ESP32C5-Setup";
const char *AP_PASS = "";
const int PORTAL_TIMEOUT = 120;

// ─── OLED Display ───────────────────────────────────────────
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// ─── Shared State ───────────────────────────────────────────
volatile bool wifiConnected = false;
volatile bool portalActive = false;

// ─── Function Prototypes ────────────────────────────────────
void taskWiFi(void *param);
void taskDisplay(void *param);
void taskMain(void *param);
void drawPortalScreen();

// ════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  unsigned long t = millis();
  while (!Serial && (millis() - t < 3000))
    delay(10);
  delay(500);

  Serial.println("\n[BOOT] ESP32-C5");
  Serial.println("[BOOT] OLED + WiFiManager + FreeRTOS");

  // Initialize I2C & OLED
  Wire.begin(SDA_PIN, SCL_PIN);
  u8g2.begin();
  u8g2.setContrast(200);

  // Boot screen
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_helvB10_tr);
  u8g2.drawStr(20, 28, "ESP32-C5");
  u8g2.setFont(u8g2_font_helvR08_tr);
  u8g2.drawStr(30, 46, "Starting up...");
  u8g2.sendBuffer();

  // Create FreeRTOS tasks
  xTaskCreate(taskWiFi, "WiFi", 8192, NULL, 2, NULL);
  xTaskCreate(taskDisplay, "Display", 4096, NULL, 1, NULL);
  xTaskCreate(taskMain, "Main", 4096, NULL, 1, NULL);
}

void loop() { vTaskDelay(portMAX_DELAY); }

// ════════════════════════════════════════════════════════════
//  WiFi TASK — ESP32-C5 (ไม่ต้อง WiFi.mode fix)
// ════════════════════════════════════════════════════════════
void taskWiFi(void *param) {
  WiFiManager wm;

  // ลด TX power สำหรับ Super Mini
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  Serial.println("[WiFi] TX power 8.5dBm");

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
      WiFi.begin();

      int attempts = 0;
      while (WiFi.status() != WL_CONNECTED && attempts < 40) {
        vTaskDelay(pdMS_TO_TICKS(500));
        attempts++;
      }

      if (WiFi.status() == WL_CONNECTED) {
        wifiConnected = true;
        Serial.printf("\n[WiFi] Reconnected!\n");
      } else {
        portalActive = true;
        if (!wm.startConfigPortal(AP_NAME, AP_PASS)) {
          ESP.restart();
        }
        wifiConnected = true;
        portalActive = false;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

// ════════════════════════════════════════════════════════════
//  DISPLAY TASK — ★ ใส่โค้ดแสดงผล OLED ของคุณที่นี่
// ════════════════════════════════════════════════════════════
void taskDisplay(void *param) {
  // รอ WiFi เชื่อมต่อก่อน (ลบได้ถ้าไม่ต้องการรอ)
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

    // ═══════════════════════════════════════════════
    //  ★ ใส่โค้ดแสดงผล OLED ของคุณที่นี่ ★
    // ═══════════════════════════════════════════════
    u8g2.clearBuffer();

    u8g2.setFont(u8g2_font_helvB10_tr);
    u8g2.drawStr(20, 16, "ESP32-C5");
    u8g2.drawHLine(5, 20, 118);

    u8g2.setFont(u8g2_font_helvR08_tr);
    u8g2.drawStr(4, 35, "WiFi: OK");
    u8g2.drawStr(4, 48, "RSSI:");

    char rssi[16];
    snprintf(rssi, sizeof(rssi), "%d dBm", WiFi.RSSI());
    u8g2.drawStr(34, 48, rssi);

    u8g2.drawHLine(5, 52, 118);
    u8g2.drawStr(10, 63, "Ready to use!");

    u8g2.sendBuffer();
    // ═══════════════════════════════════════════════

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// ════════════════════════════════════════════════════════════
//  MAIN TASK — ★ ใส่โค้ดงานหลักของคุณที่นี่
// ════════════════════════════════════════════════════════════
void taskMain(void *param) {
  while (!wifiConnected) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }

  Serial.println("[Main] WiFi ready! Starting main task...");

  for (;;) {
    if (wifiConnected) {
      // ═══════════════════════════════════════════════
      //  ★ ใส่โค้ดของคุณที่นี่ ★
      //  เช่น อ่าน sensor, ส่งข้อมูล, ping, etc.
      // ═══════════════════════════════════════════════

      Serial.printf("[Main] Running... RSSI: %d dBm\n", WiFi.RSSI());
    }

    vTaskDelay(pdMS_TO_TICKS(5000));
  }
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
