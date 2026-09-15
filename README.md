# F91 VNPT Payment Standee Firmware (ESP32-S2)

Official Firmware for the **F91 VNPT Standee** device built with **ESP-IDF v5.3**. The device features an **ESP32-S2** microcontroller connected to a **240x320 ST7789V 8-bit Intel 8080 (i80) Parallel LCD Display**.

---

## Features

- **Official VNPT Standby Mode**: Pure white `#FFFFFF` background with brand VNPT Blue `#005BAA`.
- **Dynamic ISO/IEC 18004 QR Code Engine**: Generates VietQR / URL payment QR codes instantly from AT serial commands using Nayuki C QR Engine.
- **Rounded Aesthetics & Center Logo Overlay**: Maximized $216 \times 216$ QR display with 12px rounded corners and a 36x36 official VNPT Center Logo with an 8px rounded white card container.
- **Flicker-Free Countdown Badge**: Dedicated 100x26 DMA RAM buffer updates the 30-second countdown with **0% screen flickering**.
- **Non-Blocking USB CDC ACM AT Command Parser**: Parses AT commands (`AT+QR=`, `AT+DTIME=`, `AT+BG`, `AT+VER`) over USB CDC ACM with a 100ms idle timeout.
- **150KB Raw Image Streaming Mode**: Supports streaming raw 240x320 RGB565 images (`AT+DTIME=0`).

---

## Hardware Pinout (ST7789V 8-bit i80 Parallel Bus)

| Function | ESP32-S2 Pin | Notes |
| :--- | :--- | :--- |
| **LCD_CS** | GPIO 13 | Chip Select |
| **LCD_DC** | GPIO 12 | Data / Command |
| **LCD_WR** | GPIO 11 | Write Clock (PCLK 4MHz) |
| **LCD_RST** | GPIO 18 | Hardware Reset |
| **BK_LIGHT**| GPIO 21 | Backlight Control (HIGH = ON) |
| **POWER_42**| GPIO 42 | Display Power Enable |
| **DATA[0..7]**| GPIO [10, 9, 8, 7, 6, 5, 3, 2] | 8-bit Parallel Data Bus |

---

## Building & Flashing

### 1. Export ESP-IDF Environment
```bash
source $HOME/.espressif/v5.3.5/esp-idf/export.sh  # (or export.fish for Fish shell)
```

### 2. Build Firmware
```bash
idf.py build
```

### 3. Flash to ESP32-S2
```bash
sudo chmod 666 /dev/ttyACM0
idf.py -p /dev/ttyACM0 flash
```

---

## Serial AT Command Reference

### Send Payment QR Code
```bash
echo "AT+QR=https://vnpt.vn,30" | sudo tee /dev/ttyACM0
```

### Return to Standby Screen
```bash
echo "AT+BG" | sudo tee /dev/ttyACM0
```

### Stream 150KB Raw RGB565 Image
```bash
echo "AT+DTIME=0" | sudo tee /dev/ttyACM0
cat photo_240x320.rgb565 | sudo tee /dev/ttyACM0
```

---

## Project Structure

- `main/main.c`: Core application firmware logic, FreeRTOS tasks, AT parser, i80 DMA LCD renderer.
- `main/vnpt_bg_data.h`: Embedded 240x320 RGB565 VNPT Standby Background pixels.
- `main/vnpt_logo_center.h`: Embedded 36x36 RGB565 VNPT Center Logo pixels (i80 Little-Endian byte-swapped).
- `main/qrcodegen.c`, `main/qrcodegen.h`: ISO/IEC 18004 Nayuki QR generator engine.
- `sdkconfig`: ESP-IDF Kconfig build configuration.

---

## License
MIT License. Developed for VNPT Standee F91 Display Controller.
