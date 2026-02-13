/*
 * ESP32-C3 Super Mini — OLED 128×64 Online Clock
 * Thailand Time (UTC+7) via NTP
 *
 * OLED I2C Wiring:
 *   SDA -> GPIO 2
 *   SCL -> GPIO 3
 *
 * Uses FreeRTOS tasks:
 *   - taskNTP:     syncs time from NTP server periodically
 *   - taskDisplay: updates the OLED display every second
 */

#include <Arduino.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <Wire.h>
#include <time.h>

// ─── WiFi Configuration ──────────────────────────────────────
const char *WIFI_SSID = "EN_Title";
const char *WIFI_PASS = "11111112";

// ─── NTP Configuration ──────────────────────────────────────
const char *NTP_SERVER_1 = "pool.ntp.org";
const char *NTP_SERVER_2 = "time.nist.gov";
const char *NTP_SERVER_3 = "time.google.com";
const long GMT_OFFSET = 7 * 3600; // UTC+7 Thailand
const int DST_OFFSET = 0;         // No DST in Thailand

// ─── NTP Sync Interval ──────────────────────────────────────
const unsigned long NTP_SYNC_INTERVAL_MS = 3600000; // Re-sync every 1 hour

// ─── OLED Display (SSD1306 128×64, I2C on GPIO 5 & 6) ──────
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

// ─── Day & Month Names ─────────────────────────────────────
const char *DAY_NAMES[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char *MONTH_NAMES[] = {"",    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                             "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

// ─── Function Prototypes ────────────────────────────────────
void taskWiFi(void *param);
void taskNTP(void *param);
void taskDisplay(void *param);
void connectWiFi();
void drawDisplay();
void drawConnectingScreen();

// ════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);

  // Wait for USB CDC to be ready (ESP32-C3 uses native USB)
  unsigned long startWait = millis();
  while (!Serial && (millis() - startWait < 3000)) {
    delay(10);
  }
  delay(500);

  Serial.println("\n[BOOT] ESP32-C3 OLED Clock Starting...");

  // Initialize I2C on GPIO 5 (SDA) and GPIO 6 (SCL)
  Wire.begin(2, 3);

  // Initialize OLED
  u8g2.begin();
  u8g2.setContrast(200);

  // Show boot screen
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_helvB10_tr);
  u8g2.drawStr(20, 28, "OLED Clock");
  u8g2.setFont(u8g2_font_helvR08_tr);
  u8g2.drawStr(22, 46, "Connecting WiFi...");
  u8g2.sendBuffer();

  // Create mutex
  timeMutex = xSemaphoreCreateMutex();

  // Configure NTP (configTime stores settings internally)
  configTime(GMT_OFFSET, DST_OFFSET, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);

  // Create FreeRTOS tasks
  xTaskCreate(taskWiFi, "WiFi", 4096, NULL, 2, NULL);
  xTaskCreate(taskNTP, "NTP", 4096, NULL, 1, NULL);
  xTaskCreate(taskDisplay, "Display", 4096, NULL, 1, NULL);
}

void loop() {
  // Not used — all work is in FreeRTOS tasks
  vTaskDelay(portMAX_DELAY);
}

// ════════════════════════════════════════════════════════════
//  WiFi TASK — manages WiFi connection and reconnection
// ════════════════════════════════════════════════════════════
void taskWiFi(void *param) {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  // ★ Fix for ESP32-C3 Super Mini: reduce TX power
  // The small PCB antenna can't handle full power (20dBm),
  // causing signal distortion and connection failures.
  WiFi.setTxPower(WIFI_POWER_8_5dBm);

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      wifiConnected = false;

      // Fully disconnect and reset before retrying
      WiFi.disconnect(true);
      vTaskDelay(pdMS_TO_TICKS(1000));

      Serial.println("[WiFi] Connecting...");
      WiFi.begin(WIFI_SSID, WIFI_PASS);

      // Wait up to 20 seconds for connection
      int attempts = 0;
      while (WiFi.status() != WL_CONNECTED && attempts < 40) {
        vTaskDelay(pdMS_TO_TICKS(500));
        Serial.print(".");
        attempts++;
      }

      if (WiFi.status() == WL_CONNECTED) {
        wifiConnected = true;
        Serial.printf("\n[WiFi] Connected! IP: %s\n",
                      WiFi.localIP().toString().c_str());
      } else {
        Serial.println("\n[WiFi] Connection failed, retrying in 10s...");
        WiFi.disconnect(true);
        vTaskDelay(pdMS_TO_TICKS(10000));
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
  // Draw a thin separator line
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
  } else {
    char buf[32];
    snprintf(buf, sizeof(buf), "Syncing NTP%s", dots);
    u8g2.drawStr(24, 40, buf);
  }

  u8g2.drawStr(10, 58, "SSID: .@LICEC-Student");

  u8g2.sendBuffer();
}
