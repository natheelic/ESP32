/*
 * ESP32-C3 Super Mini — WiFiManager + Ping 1.1.1.1 (FreeRTOS)
 *
 * เมื่อบูตครั้งแรก (หรือ WiFi ที่บันทึกไว้ใช้ไม่ได้):
 *   1. ESP32 สร้าง AP ชื่อ "ESP32C3-Setup"
 *   2. เชื่อมต่อ WiFi นั้น แล้วเปิด 192.168.4.1
 *   3. เลือก WiFi + ใส่รหัสผ่าน
 *
 * หลังเชื่อมต่อสำเร็จ จะ ping 1.1.1.1 ทุก 5 วินาที
 * แสดงผลผ่าน Serial Monitor
 *
 * FreeRTOS Tasks:
 *   - taskWiFi:  manages WiFi connection & reconnection
 *   - taskPing:  pings 1.1.1.1 periodically
 */

#include <Arduino.h>
#include <ESP32Ping.h> // https://github.com/marian-craciunescu/ESP32Ping
#include <WiFi.h>
#include <WiFiManager.h> // https://github.com/tzapu/WiFiManager

// ─── WiFiManager Configuration ──────────────────────────────
const char *AP_NAME = "ESP32C3-Setup";
const char *AP_PASS = "";       // empty = open AP
const int PORTAL_TIMEOUT = 120; // seconds

// ─── Ping Configuration ─────────────────────────────────────
IPAddress PING_TARGET(1, 1, 1, 1);           // Cloudflare DNS
const int PING_COUNT = 3;                    // pings per round
const unsigned long PING_INTERVAL_MS = 5000; // ms between rounds

// ─── Shared State ───────────────────────────────────────────
volatile bool wifiConnected = false;

// ─── Function Prototypes ────────────────────────────────────
void taskWiFi(void *param);
void taskPing(void *param);

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

  Serial.println("\n================================");
  Serial.println(" ESP32-C3 WiFiManager + Ping");
  Serial.println(" (FreeRTOS Version)");
  Serial.println("================================");

  // Create FreeRTOS tasks
  xTaskCreate(taskWiFi, "WiFi", 8192, NULL, 2, NULL); // priority 2 (higher)
  xTaskCreate(taskPing, "Ping", 4096, NULL, 1, NULL); // priority 1
}

void loop() {
  // Not used — all work is in FreeRTOS tasks
  vTaskDelay(portMAX_DELAY);
}

// ════════════════════════════════════════════════════════════
//  WiFi TASK — connect via WiFiManager + monitor connection
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

  Serial.println("[WiFi] Connecting (or starting config portal)...");
  Serial.printf("[WiFi] If no saved WiFi, connect to AP: %s\n", AP_NAME);

  // ── First connection via WiFiManager ──────────────────
  if (!wm.autoConnect(AP_NAME, AP_PASS)) {
    Serial.println("[WiFi] Portal timed out — restarting...");
    ESP.restart();
  }

  // ★ Re-apply TX power หลังเชื่อมต่อสำเร็จ
  WiFi.setTxPower(WIFI_POWER_8_5dBm);

  wifiConnected = true;
  Serial.println("[WiFi] Connected!");
  Serial.printf("[WiFi] SSID : %s\n", WiFi.SSID().c_str());
  Serial.printf("[WiFi] IP   : %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("[WiFi] RSSI : %d dBm\n", WiFi.RSSI());
  Serial.println("================================\n");

  // ── Monitor connection and reconnect if needed ────────
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      wifiConnected = false;
      Serial.println("[WiFi] Disconnected! Reconnecting...");

      WiFi.disconnect(true);
      vTaskDelay(pdMS_TO_TICKS(1000));
      WiFi.mode(WIFI_STA);
      WiFi.setTxPower(WIFI_POWER_8_5dBm);
      WiFi.begin(); // ใช้ credentials ที่บันทึกไว้

      // รอสูงสุด 20 วินาที
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
        // Reconnect ไม่ได้ → เปิด config portal ใหม่
        Serial.println("\n[WiFi] Reconnect failed. Starting portal...");

        if (!wm.startConfigPortal(AP_NAME, AP_PASS)) {
          Serial.println("[WiFi] Portal timed out — restarting...");
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

    vTaskDelay(pdMS_TO_TICKS(5000)); // ตรวจสอบทุก 5 วินาที
  }
}

// ════════════════════════════════════════════════════════════
//  PING TASK — ping 1.1.1.1 periodically
// ════════════════════════════════════════════════════════════
void taskPing(void *param) {
  // รอ WiFi เชื่อมต่อก่อน
  Serial.println("[Ping] Waiting for WiFi...");
  while (!wifiConnected) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }

  Serial.printf("[Ping] Target: %s  Count: %d  Interval: %lums\n",
                PING_TARGET.toString().c_str(), PING_COUNT, PING_INTERVAL_MS);
  Serial.println("--------------------------------");

  for (;;) {
    if (wifiConnected) {
      Serial.printf("[Ping] Pinging %s ...\n", PING_TARGET.toString().c_str());

      if (Ping.ping(PING_TARGET, PING_COUNT)) {
        Serial.printf("[Ping] OK — avg %.1f ms\n", Ping.averageTime());
      } else {
        Serial.println("[Ping] FAILED — no response");
      }
    } else {
      Serial.println("[Ping] Skipped — WiFi not connected");
    }

    vTaskDelay(pdMS_TO_TICKS(PING_INTERVAL_MS));
  }
}
