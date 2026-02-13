/*
 * ESP32-C3 Super Mini — FreeRTOS + WiFiManager Template
 *
 * โครงสร้างพื้นฐานสำหรับ ESP32-C3 Super Mini:
 *   - WiFiManager (Captive Portal) สำหรับตั้งค่า WiFi
 *   - FreeRTOS tasks สำหรับจัดการงานขนาน
 *   - WiFi Fix: WiFi.mode(WIFI_STA) + ลด TX power
 *
 * WiFi Setup:
 *   บูตครั้งแรก → สร้าง AP "ESP32C3-Setup"
 *   เชื่อมต่อ AP → เปิด 192.168.4.1 → เลือก WiFi
 *
 * FreeRTOS Tasks:
 *   - taskWiFi: จัดการเชื่อมต่อ WiFi + reconnect
 *   - taskMain: งานหลัก (ใส่โค้ดของคุณที่นี่)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h> // https://github.com/tzapu/WiFiManager

// ─── WiFiManager Configuration ──────────────────────────────
const char *AP_NAME = "ESP32C3-Setup";
const char *AP_PASS = "";       // empty = open AP
const int PORTAL_TIMEOUT = 120; // seconds

// ─── Shared State ───────────────────────────────────────────
volatile bool wifiConnected = false;

// ─── Function Prototypes ────────────────────────────────────
void taskWiFi(void *param);
void taskMain(void *param);

// ════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);

  // Wait for USB CDC (ESP32-C3 native USB)
  unsigned long t = millis();
  while (!Serial && (millis() - t < 3000))
    delay(10);
  delay(500);

  Serial.println("\n[BOOT] ESP32-C3 Super Mini");
  Serial.println("[BOOT] FreeRTOS + WiFiManager");

  // Create FreeRTOS tasks
  xTaskCreate(taskWiFi, "WiFi", 8192, NULL, 2, NULL); // priority 2 (สูง)
  xTaskCreate(taskMain, "Main", 4096, NULL, 1, NULL); // priority 1
}

void loop() { vTaskDelay(portMAX_DELAY); }

// ════════════════════════════════════════════════════════════
//  WiFi TASK — WiFiManager + reconnect
//  ★ ESP32-C3 Super Mini WiFi Fix included
// ════════════════════════════════════════════════════════════
void taskWiFi(void *param) {
  WiFiManager wm;

  // ★ ESP32-C3 Super Mini WiFi Fix:
  // ต้องตั้ง WiFi.mode() ก่อน จึงจะ setTxPower ได้
  // ลด TX power เพราะเสาอากาศ PCB ตัวเล็กรับ power เต็มไม่ไหว
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  Serial.println("[WiFi] TX power set to 8.5dBm (ESP32-C3 fix)");

  // WiFiManager settings
  wm.setConfigPortalTimeout(PORTAL_TIMEOUT);
  wm.setConnectRetries(3);
  wm.setConnectTimeout(20);
  wm.setClass("invert"); // dark theme

  // ── First connection ──────────────────────────────────
  Serial.println("[WiFi] Connecting...");
  Serial.printf("[WiFi] If no saved WiFi, connect to AP: %s\n", AP_NAME);

  if (!wm.autoConnect(AP_NAME, AP_PASS)) {
    Serial.println("[WiFi] Portal timed out. Restarting...");
    ESP.restart();
  }

  // ★ Re-apply TX power หลัง autoConnect
  WiFi.setTxPower(WIFI_POWER_8_5dBm);

  wifiConnected = true;
  Serial.printf("[WiFi] Connected! IP: %s\n",
                WiFi.localIP().toString().c_str());
  Serial.printf("[WiFi] SSID: %s  RSSI: %d dBm\n", WiFi.SSID().c_str(),
                WiFi.RSSI());

  // ── Monitor + reconnect ───────────────────────────────
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      wifiConnected = false;
      Serial.println("[WiFi] Disconnected! Reconnecting...");

      WiFi.disconnect(true);
      vTaskDelay(pdMS_TO_TICKS(1000));

      // ★ set mode + TX power ใหม่หลัง disconnect
      WiFi.mode(WIFI_STA);
      WiFi.setTxPower(WIFI_POWER_8_5dBm);
      WiFi.begin();

      int attempts = 0;
      while (WiFi.status() != WL_CONNECTED && attempts < 40) {
        vTaskDelay(pdMS_TO_TICKS(500));
        Serial.print(".");
        attempts++;
      }

      if (WiFi.status() == WL_CONNECTED) {
        WiFi.setTxPower(WIFI_POWER_8_5dBm);
        wifiConnected = true;
        Serial.printf("\n[WiFi] Reconnected! IP: %s\n",
                      WiFi.localIP().toString().c_str());
      } else {
        Serial.println("\n[WiFi] Failed. Starting portal...");

        if (!wm.startConfigPortal(AP_NAME, AP_PASS)) {
          Serial.println("[WiFi] Portal timed out. Restarting...");
          ESP.restart();
        }

        WiFi.setTxPower(WIFI_POWER_8_5dBm);
        wifiConnected = true;
        Serial.printf("[WiFi] Connected via portal! IP: %s\n",
                      WiFi.localIP().toString().c_str());
      }
    } else {
      wifiConnected = true;
    }

    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

// ════════════════════════════════════════════════════════════
//  MAIN TASK — ใส่โค้ดงานหลักของคุณที่นี่
// ════════════════════════════════════════════════════════════
void taskMain(void *param) {
  // รอ WiFi เชื่อมต่อก่อน
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

      Serial.printf("[Main] Running... WiFi RSSI: %d dBm\n", WiFi.RSSI());
    }

    vTaskDelay(pdMS_TO_TICKS(5000)); // ทำงานทุก 5 วินาที
  }
}
