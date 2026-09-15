# 🏗️ KIẾN TRÚC MÃ NGUỒN VÀ GIẢI THÍCH CHI TIẾT CÁC HÀM (`main.c`)

Tài liệu này giải thích chi tiết cấu trúc thư mục, chức năng, mục đích, nhiệm vụ của từng hàm trong file [main/main.c](file:///home/hiieu/Documents/Document_cachyos/vnpt_work/f91_display_controller/main/main.c), cũng như phân loại rõ ràng hàm nào là **Tự viết (Custom)** và hàm nào **Dùng thư viện (Library/ESP-IDF)**.

---

## 📁 1. CẤU TRÚC DỰ ÁN & VAI TRÒ CÁC TỆP TIN

```text
f91_display_controller/
├── CMakeLists.txt              # Cấu hình biên dịch CMake gốc của ESP-IDF
├── sdkconfig                   # Cấu hình Kconfig hệ thống (Clock, Memory, USB CDC)
├── sdkconfig.defaults          # Cấu hình mặc định cho chip ESP32-S2
├── README.md                   # Hướng dẫn tổng quan & Pinout phần cứng
├── REVERSE_ENGINEERING_WALKTHROUGH.md # Nhật ký quy trình đảo ngược & phát triển
├── CODE_ARCHITECTURE_AND_FUNCTIONS.md # (File này) Giải thích kiến trúc mã nguồn
└── main/
    ├── CMakeLists.txt          # Cấu hình CMake cho component main
    ├── main.c                  # MÃ NGUỒN CỐT LÕI (Điều khiển LCD, USB, QR Engine)
    ├── qrcodegen.c / .h        # Thư viện thuật toán QR ISO/IEC 18004 (Project Nayuki)
    ├── qrcode.c / .h           # Wrapper trợ lý truy xuất ma trận QR
    ├── vnpt_bg_data.h          # Mảng dữ liệu RGB565 Hình nền Standby VNPT (240x320)
    └── vnpt_logo_center.h      # Mảng dữ liệu RGB565 Logo VNPT tâm mã QR (36x36)
```

---

## 🛠️ 2. PHÂN LOẠI & GIẢI THÍCH CHI TIẾT CÁC HÀM TRONG `main.c`

### A. ✏️ DẠNG 1: CÁC HÀM TỰ VIẾT (CUSTOM FUNCTIONS)

Các hàm này do chúng ta tự thiết kế và tối ưu hóa chuyên biệt cho thiết bị Standee VNPT F91:

#### 1. `ensure_frame_buffer(void)`
* **Loại hàm**: 自制 (Tự viết).
* **Mục đích & Nhiệm vụ**: Cấp phát bộ đệm khung hình toàn màn hình ($240 \times 320 \times 2 = 153.6\text{ KB}$) trong vùng RAM nội hỗ trợ DMA (`MALLOC_CAP_DMA`).
* **Tại sao cần**: Đảm bảo bộ đệm luôn sẵn sàng trước khi vẽ, chống lỗi đè vùng nhớ.

#### 2. `ensure_qr_buffers(void)`
* **Loại hàm**: 自制 (Tự viết).
* **Mục đích & Nhiệm vụ**: Cấp phát 2 mảng đệm tạm ($3918\text{ bytes} \times 2$) cho thuật toán QR trên **Heap SRAM** (`MALLOC_CAP_8BIT`).
* **Tại sao cần**: **Chìa khóa giải quyết dứt điểm lỗi Stack Overflow (Crash/Nhấp nháy)** ban đầu do không chiếm dụng bộ nhớ Stack của Task.

#### 3. `fb_fill_rect(int x1, int y1, int width, int height, uint16_t color)`
* **Loại hàm**: 自制 (Tự viết).
* **Mục đích & Nhiệm vụ**: Tô một hình chữ nhật phẳng có màu `color` vào bộ đệm `frame_buffer`.
* **Ứng dụng**: Tô nền màn hình, vẽ các module đen/trắng của mã QR, vẽ thanh banner VNPT Blue.

#### 4. `fb_fill_rounded_rect(int x1, int y1, int width, int height, int radius, uint16_t color)`
* **Loại hàm**: 自制 (Tự viết).
* **Mục đích & Nhiệm vụ**: Tô một hình chữ nhật **bo cong 4 góc** với bán kính `radius` pixel.
* **Ứng dụng**: Tạo viền bo tròn thẩm mỹ **12px** cho khung Mã QR và **8px** cho thẻ Logo VNPT trung tâm.

#### 5. `draw_vnpt_background(void)`
* **Loại hàm**: 自制 (Tự viết).
* **Mục đích & Nhiệm vụ**: Sao chép mảng hình nền VNPT Standby từ Flash (`vnpt_bg_pixels`) vào DMA RAM và đẩy ra màn hình.
* **Ứng dụng**: Hiển thị hình nền chờ VNPT khi khởi động hoặc khi hết 30s đếm ngược.

#### 6. `fb_draw_digit(int start_x, int start_y, int digit, uint16_t fg_color, uint16_t bg_color)`
* **Loại hàm**: 自制 (Tự viết).
* **Mục đích & Nhiệm vụ**: Vẽ một chữ số (0-9) kích thước $8 \times 12$ pixels từ mảng font bitmap `font_8x12_digits`.

#### 7. `draw_countdown_badge(int seconds)`
* **Loại hàm**: 自制 (Tự viết).
* **Mục đích & Nhiệm vụ**: Tạo và đẩy duy nhất bộ đệm đếm ngược $100 \times 26$ pixels (`badge_buf`) ở góc dưới qua DMA.
* **Ứng dụng**: Đạt hiệu ứng **0% nhấp nháy màn hình (Flicker-Free)** khi đếm ngược 30s.

#### 8. `render_qr_payment_screen(const char *qr_data, int timeout_sec)`
* **Loại hàm**: 自制 (Tự viết).
* **Mục đích & Nhiệm vụ**: Hàm cốt lõi tạo màn hình thanh toán VNPT:
  1. Tô nền xám nhẹ & vẽ Banner xanh VNPT.
  2. Phóng to khung QR lên **$216 \times 216$ pixels** bo góc **12px**.
  3. Lồng Logo VNPT chuẩn 36x36 ở tâm với thẻ viền bo góc **8px**.
  4. Bắn toàn bộ khung hình ra LCD qua 1 giao dịch DMA duy nhất.

#### 9. `process_at_command(const char *cmd)`
* **Loại hàm**: 自制 (Tự viết).
* **Mục đích & Nhiệm vụ**: Phân tích cú pháp chuỗi lệnh AT nhận được qua USB:
  - Không phân biệt chữ hoa/chữ thường (`AT+QR=`, `at+qr=`).
  - Tự động bóc tách dấu ngoặc kép `""` và tham số thời gian chờ.
  - Phân nhánh các lệnh: `AT+QR=`, `AT+DTIME=`, `AT+BG`, `AT+VER`.

#### 10. `usb_cdc_task(void *pvParameters)`
* **Loại hàm**: 自制 (Tự viết - Task FreeRTOS).
* **Mục đích & Nhiệm vụ**: Task đọc dữ liệu từ cổng USB CDC Serial:
  - Cấu hình đọc non-blocking (`fcntl O_NONBLOCK`).
  - Tự động chốt lệnh sau **100ms Idle Timeout** nếu không gửi `\n`.
  - Xử lý nhận dữ liệu ảnh thô 150KB (`STATE_RAW_STREAM`).

#### 11. `timer_task(void *pvParameters)`
* **Loại hàm**: 自制 (Tự viết - Task FreeRTOS).
* **Mục đích & Nhiệm vụ**: Task đếm ngược 1 giây:
  - Giảm thời gian đếm ngược từng giây và gọi `draw_countdown_badge()`.
  - Tự động gọi `draw_vnpt_background()` khi hết giờ (`seconds == 0`).
  - Nếu `timeout == 0`: Giữ hiển thị mã QR vĩnh viễn không tự tắt.

#### 12. `app_main(void)`
* **Loại hàm**: 自制 (Điểm khởi chạy ứng dụng).
* **Mục đích & Nhiệm vụ**: Hàm khởi tạo chính của vi điều khiển:
  1. Cấu hình unbuffered I/O cho stdio.
  2. Bật nguồn màn hình (GPIO 42, 21).
  3. Khởi tạo Bus i80 4MHz và LCD ST7789V.
  4. Hiển thị hình nền Standby VNPT & Tạo 2 Task FreeRTOS.

---

### B. 📚 DẠNG 2: CÁC HÀM DÙNG THƯ VIỆN (LIBRARY / ESP-IDF / C STANDARD)

Các hàm này được gọi từ các thư viện chính hãng của Espressif, ISO/IEC Nayuki QR Engine và thư viện C tiêu chuẩn:

| Tên Hàm | Thư Viện Nguồn | Mục Đích & Nhiệm Vụ |
| :--- | :--- | :--- |
| `esp_lcd_new_i80_bus()` | ESP-IDF (`esp_lcd_io_i80.h`) | Khởi tạo ngoại vi Bus song song Intel 8080 8-bit phần cứng |
| `esp_lcd_new_panel_st7789()` | ESP-IDF (`esp_lcd_panel_vendor.h`) | Khởi tạo Driver điều khiển chip màn hình ST7789V |
| `esp_lcd_panel_reset()` | ESP-IDF (`esp_lcd_panel_ops.h`) | Reset cứng màn hình qua chân GPIO 18 |
| `esp_lcd_panel_init()` | ESP-IDF (`esp_lcd_panel_ops.h`) | Nạp các thanh ghi cấu hình khởi động màn hình |
| `esp_lcd_panel_draw_bitmap()` | ESP-IDF (`esp_lcd_panel_ops.h`) | **Bắn dữ liệu pixel từ RAM ra màn hình qua bus DMA phần cứng** |
| `qrcodegen_encodeText()` | Nayuki QR Engine (`qrcodegen.h`) | Tính toán và tạo ma trận mã QR chuẩn quốc tế ISO/IEC 18004 |
| `qrcodegen_getSize()` | Nayuki QR Engine (`qrcodegen.h`) | Lấy kích thước ma trận QR (số module $N \times N$) |
| `qrcodegen_getModule()` | Nayuki QR Engine (`qrcodegen.h`) | Đọc trạng thái Đen (true) hay Trắng (false) của pixel QR |
| `heap_caps_malloc()` | ESP-IDF Heap (`esp_heap_caps.h`) | Cấp phát bộ nhớ RAM nội hỗ trợ DMA / SRAM |
| `xTaskCreate()` | FreeRTOS (`freertos/task.h`) | Tạo và đăng ký một Task độc lập chạy trên FreeRTOS |
| `vTaskDelay()` | FreeRTOS (`freertos/task.h`) | Tạm ngưng Task trong khoảng thời gian chỉ định (ms) |
| `fcntl()` | POSIX System (`fcntl.h`) | Cấu hình cổng cờ reading non-blocking (`O_NONBLOCK`) trên file descriptor 0 |
| `setvbuf()` | C Standard (`stdio.h`) | Tắt bộ đệm dòng (`_IONBF`) giúp đọc byte Serial ngay lập tức |
| `read()` | POSIX I/O (`unistd.h`) | Đọc dữ liệu thô từng byte từ cổng USB CDC Serial |
