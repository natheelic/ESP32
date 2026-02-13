/*
 * ESP32-C5 Super Mini — OLED 128×64 Online Clock
 * Thailand Time (UTC+7) via NTP
 * WiFi Configuration via WiFiManager (Captive Portal)
 *
 * OLED I2C Wiring:
 *   SDA -> GPIO 2
 *   SCL -> GPIO 3
 *
 * WiFi Setup:
 *   On first boot (or if saved WiFi fails), the ESP32 creates
 *   an Access Point named "OLED-Clock-Setup".
 *   Connect to it and open 192.168.4.1 to configure WiFi.
 *
 * Uses FreeRTOS tasks:
 *   - taskWiFi:    manages WiFi connection via WiFiManager
 *   - taskNTP:     syncs time from NTP server periodically
 *   - taskDisplay: updates the OLED display every second
 */

#include <Arduino.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <WiFiManager.h> // https://github.com/tzapu/WiFiManager
#include <Wire.h>
#include <time.h>

// ─── SDA & SCL ──────────────────────────────────────────────
#define SDA_PIN 2
#define SCL_PIN 3

// ─── WiFiManager Configuration ──────────────────────────────
const char *AP_NAME = "OLED-Clock-Setup"; // AP name for configuration portal
const char *AP_PASS = "";                 // AP password (empty = open)
const int PORTAL_TIMEOUT = 120;           // Config portal timeout in seconds

// ─── NTP Configuration ──────────────────────────────────────
const char *NTP_SERVER_1 = "pool.ntp.org";
const char *NTP_SERVER_2 = "time.nist.gov";
const char *NTP_SERVER_3 = "time.google.com";
const long GMT_OFFSET = 7 * 3600; // UTC+7 Thailand
const int DST_OFFSET = 0;         // No DST in Thailand

// ─── NTP Sync Interval ──────────────────────────────────────
const unsigned long NTP_SYNC_INTERVAL_MS = 3600000; // Re-sync every 1 hour

// ─── OLED Display (SSD1306 128×64, I2C on GPIO 2 & 3) ──────
// U8G2 constructor: SSD1306 128x64, HW I2C, no reset pin
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/U8X8_PIN_NONE);

// ─── Shared State (protected by mutex) ──────────────────────
SemaphoreHandle_t timeMutex;
struct TimeData {
  int year;
  int month;
  int day;
  int hour;
  int minute;
  int second;
  int weekday; // 0=Sunday
  bool valid;
} currentTime = {0, 0, 0, 0, 0, 0, 0, false};

bool wifiConnected = false;
bool ntpSynced = false;
bool portalActive = false; // true when WiFiManager config portal is active

// ─── Day & Month Names ─────────────────────────────────────
const char *DAY_NAMES[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char *MONTH_NAMES[] = {"",    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                             "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

// ─── Function Prototypes ────────────────────────────────────
void taskWiFi(void *param);
void taskNTP(void *param);
void taskDisplay(void *param);
void drawDisplay();
void drawConnectingScreen();
void drawPortalScreen();

// ════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);

  // Wait for USB CDC to be ready (ESP32-C5 uses native USB)
  unsigned long startWait = millis();
  while (!Serial && (millis() - startWait < 3000)) {
    delay(10);
  }
  delay(500);

  Serial.println("\n[BOOT] ESP32-C5 OLED Clock Starting...");
  Serial.println("[BOOT] Using WiFiManager for WiFi configuration");

  // Initialize I2C on GPIO 2 (SDA) and GPIO 3 (SCL)
  Wire.begin(SDA_PIN, SCL_PIN);

  // Initialize OLED
  u8g2.begin();
  u8g2.setContrast(200);

  // Show boot screen
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_helvB10_tr);
  u8g2.drawStr(20, 28, "OLED Clock");
  u8g2.setFont(u8g2_font_helvR08_tr);
  u8g2.drawStr(30, 46, "Starting up...");
  u8g2.sendBuffer();

  // Create mutex
  timeMutex = xSemaphoreCreateMutex();

  // Configure NTP (configTime stores settings internally)
  configTime(GMT_OFFSET, DST_OFFSET, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);

  // Create FreeRTOS tasks
  xTaskCreate(taskWiFi, "WiFi", 8192, NULL, 2, NULL);
  xTaskCreate(taskNTP, "NTP", 4096, NULL, 1, NULL);
  xTaskCreate(taskDisplay, "Display", 4096, NULL, 1, NULL);
}

void loop() {
  // Not used — all work is in FreeRTOS tasks
  vTaskDelay(portMAX_DELAY);
}

// ════════════════════════════════════════════════════════════
//  WiFi TASK — manages WiFi connection via WiFiManager
// ════════════════════════════════════════════════════════════
void taskWiFi(void *param) {
  WiFiManager wm;

  // ★ Fix for ESP32-C5 Super Mini: reduce TX power
  // The small PCB antenna can't handle full power (20dBm),
  // causing signal distortion and connection failures.
  WiFi.setTxPower(WIFI_POWER_8_5dBm);

  // WiFiManager settings
  wm.setConfigPortalTimeout(PORTAL_TIMEOUT); // Auto-close portal after timeout
  wm.setConnectRetries(3);                   // Retry connecting 3 times
  wm.setConnectTimeout(20);                  // 20 sec timeout per attempt

  // Set dark theme for the portal
  wm.setClass("invert");

  // Callbacks for status tracking
  wm.setAPCallback([](WiFiManager *myWiFiManager) {
    Serial.println("[WiFiManager] Config portal started");
    Serial.printf("[WiFiManager] AP: %s\n", AP_NAME);
    Serial.printf("[WiFiManager] IP: %s\n", WiFi.softAPIP().toString().c_str());
    portalActive = true;
  });

  wm.setSaveConfigCallback([]() {
    Serial.println("[WiFiManager] Config saved");
    portalActive = false;
  });

  // ── First connection attempt using saved credentials ──
  // autoConnect will try saved credentials first.
  // If that fails, it starts a config portal (AP mode).
  Serial.println("[WiFi] Attempting auto-connect with saved credentials...");

  if (!wm.autoConnect(AP_NAME, AP_PASS)) {
    // Portal timed out without getting credentials
    Serial.println("[WiFi] Config portal timed out. Restarting...");
    portalActive = false;
    ESP.restart();
  }

  // Successfully connected!
  wifiConnected = true;
  portalActive = false;
  Serial.printf("[WiFi] Connected! IP: %s\n",
                WiFi.localIP().toString().c_str());
  Serial.printf("[WiFi] SSID: %s\n", WiFi.SSID().c_str());

  // ── Monitor connection and reconnect if needed ──
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      wifiConnected = false;
      Serial.println("[WiFi] Connection lost! Attempting reconnect...");

      // Try reconnecting with saved credentials
      WiFi.disconnect(true);
      vTaskDelay(pdMS_TO_TICKS(1000));
      WiFi.begin(); // Uses saved credentials

      // Wait up to 20 seconds
      int attempts = 0;
      while (WiFi.status() != WL_CONNECTED && attempts < 40) {
        vTaskDelay(pdMS_TO_TICKS(500));
        Serial.print(".");
        attempts++;
      }

      if (WiFi.status() == WL_CONNECTED) {
        wifiConnected = true;
        Serial.printf("\n[WiFi] Reconnected! IP: %s\n",
                      WiFi.localIP().toString().c_str());
      } else {
        // Reconnect failed — restart config portal
        Serial.println("\n[WiFi] Reconnect failed. Starting config portal...");
        portalActive = true;

        if (!wm.startConfigPortal(AP_NAME, AP_PASS)) {
          Serial.println("[WiFi] Portal timed out. Restarting...");
          portalActive = false;
          ESP.restart();
        }

        wifiConnected = true;
        portalActive = false;
        Serial.printf("[WiFi] Connected via portal! IP: %s\n",
                      WiFi.localIP().toString().c_str());
      }
    } else {
      wifiConnected = true;
    }

    vTaskDelay(pdMS_TO_TICKS(5000)); // Check every 5 seconds
  }
}

// ════════════════════════════════════════════════════════════
//  NTP TASK — syncs time periodically
// ════════════════════════════════════════════════════════════
void taskNTP(void *param) {
  // Wait for WiFi to be connected first
  while (!wifiConnected) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }

  for (;;) {
    if (wifiConnected) {
      struct tm timeinfo;
      if (getLocalTime(&timeinfo, 10000)) {
        if (xSemaphoreTake(timeMutex, pdMS_TO_TICKS(100))) {
          currentTime.year = timeinfo.tm_year + 1900;
          currentTime.month = timeinfo.tm_mon + 1;
          currentTime.day = timeinfo.tm_mday;
          currentTime.hour = timeinfo.tm_hour;
          currentTime.minute = timeinfo.tm_min;
          currentTime.second = timeinfo.tm_sec;
          currentTime.weekday = timeinfo.tm_wday;
          currentTime.valid = true;
          xSemaphoreGive(timeMutex);
        }

        if (!ntpSynced) {
          ntpSynced = true;
          Serial.printf("[NTP] Time synced: %02d:%02d:%02d\n", timeinfo.tm_hour,
                        timeinfo.tm_min, timeinfo.tm_sec);
        }
      } else {
        Serial.println("[NTP] Failed to get time");
      }
    }

    // Re-sync every hour
    vTaskDelay(pdMS_TO_TICKS(NTP_SYNC_INTERVAL_MS));
  }
}

// ════════════════════════════════════════════════════════════
//  DISPLAY TASK — updates OLED every second
// ════════════════════════════════════════════════════════════
void taskDisplay(void *param) {
  for (;;) {
    // If config portal is active, show portal screen
    if (portalActive) {
      drawPortalScreen();
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    // Read current system time (updated continuously by the system)
    struct tm timeinfo;
    bool timeValid = false;

    if (ntpSynced && getLocalTime(&timeinfo, 100)) {
      if (xSemaphoreTake(timeMutex, pdMS_TO_TICKS(100))) {
        currentTime.year = timeinfo.tm_year + 1900;
        currentTime.month = timeinfo.tm_mon + 1;
        currentTime.day = timeinfo.tm_mday;
        currentTime.hour = timeinfo.tm_hour;
        currentTime.minute = timeinfo.tm_min;
        currentTime.second = timeinfo.tm_sec;
        currentTime.weekday = timeinfo.tm_wday;
        currentTime.valid = true;
        xSemaphoreGive(timeMutex);
      }
      timeValid = true;
    }

    if (timeValid) {
      drawDisplay();
    } else {
      drawConnectingScreen();
    }

    vTaskDelay(pdMS_TO_TICKS(1000)); // Update every second
  }
}

// ════════════════════════════════════════════════════════════
//  DRAW — Main clock display
// ════════════════════════════════════════════════════════════
void drawDisplay() {
  char dateBuf[32];
  char timeBuf[16];
  char statusBuf[24];

  if (xSemaphoreTake(timeMutex, pdMS_TO_TICKS(100))) {
    // Format: "Fri 13 Feb 2026"
    snprintf(dateBuf, sizeof(dateBuf), "%s %02d %s %04d",
             DAY_NAMES[currentTime.weekday], currentTime.day,
             MONTH_NAMES[currentTime.month], currentTime.year);

    // Format: "09:33:12"
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d", currentTime.hour,
             currentTime.minute, currentTime.second);

    xSemaphoreGive(timeMutex);
  } else {
    return; // Skip this frame
  }

  // WiFi status
  if (wifiConnected) {
    snprintf(statusBuf, sizeof(statusBuf), "WiFi: Connected");
  } else {
    snprintf(statusBuf, sizeof(statusBuf), "WiFi: Reconnecting...");
  }

  u8g2.clearBuffer();

  // ── Top line: Date ──────────────────────────────────────
  u8g2.setFont(u8g2_font_helvR10_tr);
  int dateWidth = u8g2.getStrWidth(dateBuf);
  u8g2.drawStr((128 - dateWidth) / 2, 13, dateBuf);

  // Separator line below date
  u8g2.drawHLine(10, 17, 108);

  // ── Center: Large Time ─────────────────────────────────
  u8g2.setFont(u8g2_font_logisoso22_tn); // Large numeric font
  int timeWidth = u8g2.getStrWidth(timeBuf);
  u8g2.drawStr((128 - timeWidth) / 2, 46, timeBuf);

  // Separator line below time
  u8g2.drawHLine(10, 50, 108);

  // ── Bottom: Status bar ─────────────────────────────────
  u8g2.setFont(u8g2_font_helvR08_tr);
  int statusWidth = u8g2.getStrWidth(statusBuf);
  u8g2.drawStr((128 - statusWidth) / 2, 63, statusBuf);

  u8g2.sendBuffer();
}

// ════════════════════════════════════════════════════════════
//  DRAW — WiFiManager Config Portal screen
// ════════════════════════════════════════════════════════════
void drawPortalScreen() {
  static int dotCount = 0;
  dotCount = (dotCount + 1) % 4;

  char dots[5] = "";
  for (int i = 0; i < dotCount; i++) {
    strcat(dots, ".");
  }

  u8g2.clearBuffer();

  // Title
  u8g2.setFont(u8g2_font_helvB10_tr);
  u8g2.drawStr(14, 12, "WiFi Setup");

  // Separator
  u8g2.drawHLine(5, 16, 118);

  // Instructions
  u8g2.setFont(u8g2_font_helvR08_tr);
  u8g2.drawStr(4, 28, "1. Connect to WiFi:");

  // AP Name (bold)
  u8g2.setFont(u8g2_font_helvB08_tr);
  char apBuf[32];
  snprintf(apBuf, sizeof(apBuf), "   %s", AP_NAME);
  u8g2.drawStr(4, 39, apBuf);

  // IP Address
  u8g2.setFont(u8g2_font_helvR08_tr);
  u8g2.drawStr(4, 50, "2. Open: 192.168.4.1");

  // Waiting animation
  char waitBuf[24];
  snprintf(waitBuf, sizeof(waitBuf), "Waiting for config%s", dots);
  u8g2.drawStr(10, 63, waitBuf);

  u8g2.sendBuffer();
}

// ════════════════════════════════════════════════════════════
//  DRAW — Connecting/waiting screen
// ════════════════════════════════════════════════════════════
void drawConnectingScreen() {
  static int dotCount = 0;
  dotCount = (dotCount + 1) % 4;

  char dots[5] = "";
  for (int i = 0; i < dotCount; i++) {
    strcat(dots, ".");
  }

  u8g2.clearBuffer();

  u8g2.setFont(u8g2_font_helvB10_tr);
  u8g2.drawStr(20, 20, "OLED Clock");

  u8g2.setFont(u8g2_font_helvR08_tr);

  if (!wifiConnected) {
    char buf[32];
    snprintf(buf, sizeof(buf), "Connecting WiFi%s", dots);
    u8g2.drawStr(18, 40, buf);
    u8g2.drawStr(10, 56, "Please wait...");
  } else {
    char buf[32];
    snprintf(buf, sizeof(buf), "Syncing NTP%s", dots);
    u8g2.drawStr(24, 40, buf);
  }

  u8g2.sendBuffer();
}
