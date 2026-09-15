# 📜 REVERSE ENGINEERING & DỰ ÁN STANDEE VNPT F91: TỪ NGUYÊN BẢN ĐẾN THÀNH PHẨM

Tài liệu này ghi lại toàn bộ quy trình Kỹ thuật đảo ngược (Reverse Engineering) và Phát triển Firmware cho thiết bị **VNPT Payment Standee F91** (Sử dụng vi điều khiển **ESP32-S2** và màn hình **ST7789V 240x320 8-bit i80 Parallel LCD**).

---

## 📌 GIAI ĐOẠN 1: TRÍCH XUẤT & PHÂN TÍCH HỆ THỐNG GỐC

### 1. Trích xuất toàn bộ bộ nhớ Flash của nhà sản xuất
Sử dụng công cụ `esptool.py` trích xuất 4MB Flash nguyên bản từ chip ESP32-S2 xuất xưởng:
```bash
esptool.py --chip esp32s2 -p /dev/ttyACM0 -b 460800 read_flash 0x0 0x400000 full_factory_backup.bin
```
* **Kết quả**: Tạo ra file `full_factory_backup.bin` (4,194,304 bytes).
* **Mục đích**: Bảo toàn mã nguồn gốc của nhà máy và trích xuất phân vùng `app_factory.bin` tại địa chỉ `0x10000`.

### 2. Phân tích Disassembly mã máy (`irom.asm`)
Do ứng dụng gốc bị đóng gói không có sơ đồ mạch (Schematic), chúng ta sử dụng công cụ Disassembler (`xtensa-esp32s2-elf-objdump`) để chuyển đổi mã máy `app_factory.bin` sang dạng assembly `irom.asm`:
```bash
xtensa-esp32s2-elf-objdump -D -b binary -m xtensa app_factory.bin > irom.asm
```

### 3. Tìm thấy Sơ đồ chân Phần cứng (Hardware Pinout Map)
Qua việc quét các lệnh khởi tạo GPIO (`gpio_config`, `esp_lcd_new_i80_bus`), chúng ta đã giải mã thành công sơ đồ chân kết nối giữa ESP32-S2 và màn hình LCD ST7789V 8-bit i80:

| Tín hiệu LCD | Chân GPIO ESP32-S2 | Chức năng |
| :--- | :--- | :--- |
| **LCD_CS** | **GPIO 13** | Chip Select (Chọn chíp màn hình) |
| **LCD_DC** | **GPIO 12** | Data / Command (Phân biệt Dữ liệu / Lệnh) |
| **LCD_WR** | **GPIO 11** | Write Clock (Xung chốt dữ liệu PCLK 4MHz) |
| **LCD_RST** | **GPIO 18** | Reset cứng màn hình |
| **BK_LIGHT**| **GPIO 21** | Đèn nền màn hình (HIGH = Bật) |
| **POWER_42**| **GPIO 42** | Cấp nguồn 3.3V cho màn hình |
| **DATA[0..7]**| **GPIO [10, 9, 8, 7, 6, 5, 3, 2]** | Bus dữ liệu song song 8-bit |

---

## 📌 GIAI ĐOẠN 2: XÂY DỰNG FIRMWARE MỚI (ESP-IDF v5.3)

### 1. Khởi tạo Bus phần cứng Intel 8080 (i80)
Cấu hình giao tiếp `esp_lcd_i80_bus` chạy ở xung clock **4MHz**:
- Đảm bảo tín hiệu chốt dữ liệu 8-bit ổn định tuyệt đối, không nhiễu sọc hay xé hình.

### 2. Trích xuất & Tạo bộ đệm Hình nền VNPT Standby (`vnpt_bg_data.h`)
- Trích xuất ảnh phông nền mặc định VNPT Pure White (`#FFFFFF`) với Logo VNPT thương hiệu chuẩn từ màn hình gốc.
- Chuyển đổi sang mảng pixel 16-bit RGB565 ($240 \times 320 = 153,600$ bytes) lưu tại [main/vnpt_bg_data.h](file:///home/hiieu/Documents/Document_cachyos/vnpt_work/f91_display_controller/main/vnpt_bg_data.h).

### 3. Tích hợp Thuật toán QR ISO/IEC 18004 Nayuki C Engine
- Nhúng bộ thư viện C [qrcodegen.c](file:///home/hiieu/Documents/Document_cachyos/vnpt_work/f91_display_controller/main/qrcodegen.c) & [qrcodegen.h](file:///home/hiieu/Documents/Document_cachyos/vnpt_work/f91_display_controller/main/qrcodegen.h).
- Cho phép vi điều khiển tự động tạo mã QR động từ bất kỳ chuỗi URL hay VietQR nào được gửi sang qua Serial.

---

## 📌 GIAI ĐOẠN 3: GIẢI QUYẾT CÁC THÁCH THỨC KỸ THUẬT QUAN TRỌNG

### 🛠️ Thách thức 1: Khắc phục lỗi Crash sập nguồn (Stack Overflow)
* **Vấn đề**: Ban đầu khi nhận lệnh `AT+QR`, chip bị chớp 1 cái rồi sập nguồn khởi động lại.
* **Root Cause**: Bộ đệm sinh mã QR ($7.8\text{ KB}$) được khai báo dạng biến cục bộ trên Stack của Task (vốn chỉ có 4KB).
* **Giải pháp**: Chuyển toàn bộ bộ đệm QR sang cấp phát động trên **Heap SRAM (`heap_caps_malloc`)** qua hàm `ensure_qr_buffers()`.

### 🛠️ Thách thức 2: Chống nhấp nháy khi đếm ngược 30s (Flicker-Free Badge)
* **Vấn đề**: Nếu vẽ lại toàn bộ $240 \times 320$ pixels mỗi giây, màn hình sẽ bị chớp nháy liên tục.
* **Giải pháp**: Tạo bộ đệm DMA nhỏ $100 \times 26$ pixels (`badge_buf`). Mỗi giây chỉ cập nhật duy nhất vùng đếm ngược ở đáy qua DMA (`esp_lcd_panel_draw_bitmap()`). Màn hình đạt **0% nhấp nháy**.

### 🛠️ Thách thức 3: Sửa lỗi Lệch Màu Logo VNPT (Green $\rightarrow$ VNPT Blue)
* **Vấn đề**: Logo VNPT ở tâm bị biến thành màu xanh lục (Green).
* **Root Cause**: Bus 8-bit i80 yêu cầu thứ tự byte màu 16-bit RGB565 là High-Byte trước, Low-Byte sau (Xtensa Little-Endian).
* **Giải pháp**: Chuẩn hóa mã màu mảng [vnpt_logo_center.h](file:///home/hiieu/Documents/Document_cachyos/vnpt_work/f91_display_controller/main/vnpt_logo_center.h) theo công thức `MAKE_COLOR` (`0xD502` cho VNPT Blue `RGB 0, 90, 171`).

### 🛠️ Thách thức 4: Tương thích phần mềm SSCOM & Serial Non-Blocking
* **Giải pháp**: 
  - Cấu hình đọc Serial `fcntl(0, F_SETFL, O_NONBLOCK)` và `setvbuf(stdin, NULL, _IONBF, 0)`.
  - Thiết lập **100ms Idle Timeout** tự chốt lệnh ngay cả khi không gửi `\n`.
  - Tương thích $100\%$ phần mềm **SSCOM** trên Windows và lệnh Terminal trên Linux.

---

## 📌 GIAI ĐOẠN 4: NÂNG CẤP GIAO DIỆN THẨM MỸ & LƯU TRỮ

1. **Phóng to Mã QR**: Nâng kích thước ô chứa QR lên **$216 \times 216$ pixels** (tối đa chiều rộng màn hình), lề mỗi bên 12px giúp ứng dụng ngân hàng quét cực dễ.
2. **Bo cong 4 góc thẩm mỹ (Rounded Corners)**:
   - Bo cong 4 góc khung QR: Bán kính **12px**.
   - Bo cong 4 góc thẻ Logo VNPT trung tâm: Bán kính **8px**.
   - Bo cong 4 góc thẻ đếm ngược: Bán kính **8px**.
3. **Đẩy mã nguồn lên GitHub**: Quản lý phiên bản tại [https://github.com/Hiieu-ntq/QR_STANDEE](https://github.com/Hiieu-ntq/QR_STANDEE).
