# 🔧 ESP32 Super Mini — Arduino Sketches

รวมโค้ดตัวอย่างสำหรับบอร์ด **ESP32-C3 Super Mini** และ **ESP32-C5** พร้อม WiFiManager, FreeRTOS และจอ OLED

## 📋 รายการ Sketches

### 🟢 ESP32-C3 Super Mini

> ⚠️ **ESP32-C3 WiFi Fix:** ต้องเรียก `WiFi.mode(WIFI_STA)` **ก่อน** `WiFi.setTxPower()` เสมอ
> เพราะเสาอากาศ PCB ตัวเล็กรับ power เต็ม 20dBm ไม่ไหว ต้องลดเหลือ 8.5dBm

| โฟลเดอร์ | คำอธิบาย |
|---|---|
| `ESP32C3-WiFiManager/` | WiFiManager + Ping 1.1.1.1 (แบบง่าย ไม่มี FreeRTOS) |
| `ESP32C3-FreeRTOS-WiFiManager/` | 🧩 **Template** — FreeRTOS + WiFiManager |
| `ESP32C3-OLED-WiFiManager-FreeRTOS/` | 🧩 **Template** — OLED + FreeRTOS + WiFiManager |
| `ESP32C3-WiFiManager-FreeRTOS/` | FreeRTOS + WiFiManager + Ping |
| `ESP32C3-OLED-128x64-Online-Clock-Wifimanager/` | 🕐 นาฬิกา OLED + WiFiManager |

### 🔵 ESP32-C5

| โฟลเดอร์ | คำอธิบาย |
|---|---|
| `ESP32C5-FreeRTOS-WiFiManager/` | 🧩 **Template** — FreeRTOS + WiFiManager |
| `ESP32C5-OLED-WiFiManager-FreeRTOS/` | 🧩 **Template** — OLED + FreeRTOS + WiFiManager |
| `ESP32C5-OLED-128x64-Online-Clock/` | 🕐 นาฬิกา OLED (WiFi hardcode) |
| `ESP32C5-OLED-128x64-Online-Clock-Wifimanager/` | 🕐 นาฬิกา OLED + WiFiManager |

## 🛠 วิธีใช้งาน

### 1) เลือกโปรเจกต์

แต่ละโฟลเดอร์คือโปรเจกต์แยกกัน ใช้ได้ทั้ง Arduino IDE หรือ PlatformIO

### 2) ตั้งค่าใน `platformio.ini` (ถ้าใช้ PlatformIO)

- เลือก `board` ให้ตรงรุ่น (C3 หรือ C5)
- เลือก `framework = arduino`
- ตั้งค่า `monitor_speed` ให้ตรงกับ `Serial.begin(...)`
- ถ้ามี OLED ให้เปิดไลบรารี U8g2 ตามเดิมในไฟล์

### 3) แก้การกำหนดขา I2C (OLED)

ค่าเริ่มต้นที่ใช้ในตัวอย่าง:

- ESP32-C3 Super Mini: SDA = 5, SCL = 6
- ESP32-C5: SDA = 2, SCL = 3

หากเปลี่ยนบอร์ดหรือสาย ให้แก้ค่าใน `main.cpp`

### ต้องติดตั้ง Library

| Library | ผู้พัฒนา | ใช้ใน |
|---|---|---|
| [WiFiManager](https://github.com/tzapu/WiFiManager) | tzapu | ทุกโปรเจกต์ที่มี WiFiManager |
| [U8g2](https://github.com/olikraus/u8g2) | olikraus | ทุกโปรเจกต์ที่มี OLED |
| [ESP32Ping](https://github.com/marian-craciunescu/ESP32Ping) | marian-craciunescu | โปรเจกต์ Ping |

ติดตั้งผ่าน **Arduino IDE → Sketch → Include Library → Manage Libraries...**

ถ้าใช้ PlatformIO ให้เพิ่มใน `lib_deps` ของ `platformio.ini`

### การเชื่อมต่อ WiFi ผ่าน WiFiManager

1. เปิดเครื่อง — ESP32 จะลองเชื่อมต่อ WiFi ที่บันทึกไว้
2. ถ้าไม่ได้ → สร้าง **Access Point** (ชื่อขึ้นอยู่กับแต่ละ Sketch)
3. ใช้มือถือ/คอมเชื่อมต่อ AP นั้น
4. เปิดเบราว์เซอร์ไปที่ **192.168.4.1**
5. เลือก WiFi + ใส่รหัสผ่าน → บันทึก → ESP32 จะเชื่อมต่ออัตโนมัติ

### การต่อ OLED Display (I2C)

| บอร์ด | SDA | SCL |
|---|---|---|
| ESP32-C3 Super Mini | GPIO **5** | GPIO **6** |
| ESP32-C5 | GPIO **2** | GPIO **3** |

## ★ ESP32-C3 WiFi Fix

```cpp
// ★ ESP32-C3 Super Mini WiFi Fix:
// ต้องตั้ง WiFi.mode() ก่อน จึงจะ setTxPower ได้
WiFi.mode(WIFI_STA);
WiFi.setTxPower(WIFI_POWER_8_5dBm);

// หลัง autoConnect หรือ reconnect ต้อง re-apply
WiFi.setTxPower(WIFI_POWER_8_5dBm);
```

**สาเหตุ:** เสาอากาศ PCB ขนาดเล็กของ ESP32-C3 Super Mini รับ TX power เต็ม (20dBm) ไม่ไหว ทำให้สัญญาณบิดเบี้ยว → เชื่อมต่อไม่ได้ ต้องลดเหลือ 8.5dBm

## ⚙️ การตั้งค่าที่พบบ่อย

### เปลี่ยนชื่อ Access Point ของ WiFiManager

ค้นหาใน `main.cpp` ของแต่ละโปรเจกต์ แล้วแก้ชื่อใน `autoConnect("...")`

### ตั้งค่าเวลา (Online Clock)

โปรเจกต์นาฬิกาใช้อินเทอร์เน็ตเวลา (NTP) อยู่แล้ว หากต้องการปรับโซนเวลาให้แก้ค่า offset ที่กำหนดในโค้ด

### เปลี่ยนขา/ความสว่าง OLED

แก้พารามิเตอร์ในส่วนเริ่มต้นจอของ U8g2 ตามรุ่นและขาที่ต้องการ

## ✅ เช็กลิสต์ก่อนอัปโหลด

- เลือกบอร์ดให้ตรงรุ่น (C3/C5)
- เลือกพอร์ตถูกต้อง
- ตั้ง `monitor_speed` ให้ตรงกับ `Serial.begin(...)`
- ติดตั้งไลบรารีครบ
- ถ้าเป็น ESP32-C3 ให้ใช้ WiFi Fix ตามด้านบน

## 📝 License

MIT