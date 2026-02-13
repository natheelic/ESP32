/*
 * ESP32-C3 Super Mini — WiFiManager + Ping 1.1.1.1
 *
 * เมื่อบูตครั้งแรก (หรือ WiFi ที่บันทึกไว้ใช้ไม่ได้):
 *   1. ESP32 สร้าง AP ชื่อ "ESP32C3-Setup"
 *   2. เชื่อมต่อ WiFi นั้น แล้วเปิด 192.168.4.1
 *   3. เลือก WiFi + ใส่รหัสผ่าน
 *
 * หลังเชื่อมต่อสำเร็จ จะ ping 1.1.1.1 ทุก 5 วินาที
 * แสดงผลผ่าน Serial Monitor
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
IPAddress PING_TARGET(1, 1, 1, 1);        // Cloudflare DNS
const int PING_COUNT = 3;                 // pings per round
const unsigned long PING_INTERVAL = 5000; // ms between rounds

void setup() {
  Serial.begin(115200);

  // Wait for USB CDC (ESP32-C3 native USB)
  unsigned long t = millis();
  while (!Serial && (millis() - t < 3000))
    delay(10);
  delay(500);

  Serial.println("\n================================");
  Serial.println(" ESP32-C3 WiFiManager + Ping");
  Serial.println("================================");

  // ★ ESP32-C3 Super Mini WiFi Fix:
  // 1. ต้องตั้ง WiFi.mode() ก่อน จึงจะ setTxPower ได้
  // 2. ลด TX power เพราะเสาอากาศ PCB ตัวเล็กรับ power เต็มไม่ไหว
  //    ถ้าไม่ลด → สัญญาณบิดเบี้ยว → เชื่อมต่อไม่ได้
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  Serial.println("[WiFi] TX power set to 8.5dBm (ESP32-C3 fix)");

  // ── WiFiManager ───────────────────────────────────────
  WiFiManager wm;
  wm.setConfigPortalTimeout(PORTAL_TIMEOUT);
  wm.setConnectRetries(3);
  wm.setConnectTimeout(20);
  wm.setClass("invert"); // dark theme

  Serial.println("[WiFi] Connecting (or starting config portal)...");
  Serial.printf("[WiFi] If no saved WiFi, connect to AP: %s\n", AP_NAME);

  if (!wm.autoConnect(AP_NAME, AP_PASS)) {
    Serial.println("[WiFi] Portal timed out — restarting...");
    ESP.restart();
  }

  // ★ Re-apply TX power หลังเชื่อมต่อสำเร็จ (สำคัญมาก!)
  WiFi.setTxPower(WIFI_POWER_8_5dBm);

  Serial.println("[WiFi] Connected!");
  Serial.printf("[WiFi] SSID : %s\n", WiFi.SSID().c_str());
  Serial.printf("[WiFi] IP   : %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("[WiFi] RSSI : %d dBm\n", WiFi.RSSI());
  Serial.println("--------------------------------");
  Serial.printf("[Ping] Target: %s  Count: %d  Interval: %lums\n",
                PING_TARGET.toString().c_str(), PING_COUNT, PING_INTERVAL);
  Serial.println("================================\n");
}

void loop() {
  // ── Check WiFi ────────────────────────────────────────
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Disconnected! Restarting...");
    ESP.restart();
  }

  // ── Ping ──────────────────────────────────────────────
  Serial.printf("[Ping] Pinging %s ...\n", PING_TARGET.toString().c_str());

  if (Ping.ping(PING_TARGET, PING_COUNT)) {
    Serial.printf("[Ping] OK — avg %.1f ms\n", Ping.averageTime());
  } else {
    Serial.println("[Ping] FAILED — no response");
  }

  delay(PING_INTERVAL);
}
