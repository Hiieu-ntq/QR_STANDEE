# F91 VNPT Standee Firmware v2.3 (ESP32-S2 + ST7789 8-bit Parallel)

Firmware điều khiển hiển thị và giao tiếp cho thiết bị **F91 VNPT Standee / Loa Thanh Toán Thông Minh** trên nền tảng vi điều khiển **ESP32-S2** và màn hình **ST7789** chuẩn giao tiếp song song 8-bit Intel 8080 ($240 \times 320$ pixel).

---

##  Tính Năng Chính

* **Màn hình chờ thương hiệu VNPT HD**: Nền trắng tinh khiết chuẩn $240 \times 320$ pixel với logo VNPT sắc nét, không viền rác.
* **Bộ sinh mã QR động Nayuki ISO/IEC 18004**: Tự động tạo mã QR chất lượng cao (ECC Quartile/Medium) theo nội dung thanh toán gửi từ máy tính/POS, tích hợp logo VNPT ($36 \times 36$) ở tâm mã QR có **bo tròn 4 góc ($R=6\text{px}$)**.
* **Thanh Hướng Dẫn Banner Xanh**: Dải màu xanh VNPT trên cùng hiển thị dòng chữ trắng hướng dẫn **`QUET MA DE THANH TOAN`**.
* **Huy hiệu đếm ngược thời gian (Countdown Badge)**: Đếm lùi thời gian thực (30s, 29s, ..., 1s), tự động đóng mã QR và quay về màn hình chờ khi hết hạn. Được đồng bộ bằng FreeRTOS Mutex, không bị kẹt hay giật lag.
* **Tích hợp Web Config & Captive Portal Tự Động**: 
  * Phát Wi-Fi SoftAP tên `ESP32_WiFi_Setup` (Mật khẩu: `12345678`).
  * Tự động nảy trang Web Cấu hình VNPT (`http://192.168.4.1`) trên điện thoại iOS / Android.
  * Tự động đóng cửa sổ Pop-up Captive Portal ngay sau khi kết nối thành công.
  * Tự động tắt phát sóng SoftAP chuyển về chế độ Pure STA khi đã có Internet để tiết kiệm điện và chống nhiễu.
* **Hỗ trợ Cấu Hình IP Tĩnh (Static IP)**: Cấu hình qua Web UI hoặc lệnh AT `AT+STATICIP=`.
* **Hỗ trợ nạp luồng ảnh thô 150KB Zero-Copy (`AT+DTIME`)**: Nhận trực tiếp ảnh $240 \times 320$ RGB565 qua USB CDC và vẽ trực tiếp vào LCD, không tốn RAM hệ thống.
* **Giao diện dòng lệnh AT Command chuẩn hoá**: Tự động Echo ký tự khi gõ/paste (hỗ trợ phím Backspace, Terminal VS Code, SSCOM, PuTTY, v.v.).

---

## Bảng Tập Lệnh Serial AT Commands Chi Tiết

Mọi lệnh AT được gửi qua cổng USB Serial CDC (Baudrate: `115200`, kết thúc bằng `\r\n` hoặc `\n`).

| STT | Câu lệnh AT | Cú pháp & Ví dụ | Mô tả chi tiết | Phản hồi từ thiết bị |
| :---: | :--- | :--- | :--- | :--- |
| **1** | `AT` | `AT` | Kiểm tra kết nối Serial giữa máy tính/POS và thiết bị. | `OK` |
| **2** | `AT+VER` | `AT+VER` | Lấy thông tin phiên bản firmware đang chạy. | `+VER: F91_VNPT_STANDEE_V2.3`<br>`OK` |
| **3** | `ATE1` | `ATE1` | Bật chế độ Serial Echo (Mặc định). Ký tự khi gõ hoặc paste vào terminal sẽ hiển thị thời gian thực. Hỗ trợ phím Backspace. | `OK` |
| **4** | `ATE0` | `ATE0` | Tắt chế độ Serial Echo. Dành cho các phần mềm POS/máy tự động giao tiếp không muốn nhận lại chuỗi gửi. | `OK` |
| **5** | `AT+QR` | `AT+QR=https://vnpt.vn,30`<br>`AT+QR="https://youtube.com",45` | Sinh và hiển thị mã QR thanh toán.<br>- Tham số 1: Nội dung mã QR.<br>- Tham số 2: Thời gian đếm ngược (giây). | `+QR: OK`<br>*(hoặc `+QR: ERROR` nếu chuỗi rỗng)* |
| **6** | `AT+QR_DISPLAY` | `AT+QR_DISPLAY=30,https://vnpt.vn` | Lệnh hiển thị mã QR theo chuẩn gốc F91 Hemipay (tương thích ngược hoàn toàn).<br>- Tham số 1: Thời gian đếm ngược (giây).<br>- Tham số 2: Nội dung mã QR. | `+QR_DISPLAY: OK` |
| **7** | `AT+BG`<br>`AT+CLEAR` | `AT+BG`<br>`AT+CLEAR`<br>`AT+DISPLAY_CLEAR` | Quay về màn hình chờ VNPT ngay lập tức. Xóa sạch mọi mã QR và hủy bỏ hoàn toàn tác vụ đếm ngược. | `OK` |
| **8** | `AT+DTIME=0` | `AT+DTIME=0` | Chuyển sang chế độ nhận luồng ảnh thô 153.600 bytes ($240 \times 320$ RGB565) hiển thị vĩnh viễn trên LCD. | `OK`<br>*(Sau khi nhận đủ 150KB: `+DTIME: OK`)* |
| **9** | `AT+DTIME=<sec>` | `AT+DTIME=30` | Nhận luồng ảnh thô 153.600 bytes kèm thời gian đếm lùi `<sec>` giây trước khi tự động về màn hình chờ. | `OK`<br>*(Sau khi nhận đủ 150KB: `+DTIME: OK`)* |
| **10** | `AT+APMODE`<br>`AT+WIFIRESET` | `AT+APMODE`<br>`AT+WIFIRESET` | Xóa sạch cấu hình Wi-Fi trong Flash NVS & RAM driver, bật lại SoftAP `ESP32_WiFi_Setup` và mở Captive Portal cấu hình trên điện thoại. | `+WIFIRESET: ERASING NVS & OPENING SOFTAP...`<br>`OK` |
| **11** | `AT+WIFISTATUS` | `AT+WIFISTATUS` | Xem trạng thái Wi-Fi & Internet hiện tại, địa chỉ IP được cấp, SSID và kết quả Ping Internet. | `+WIFISTATUS: CONNECTED, IP: 192.168.1.50, INTERNET: ONLINE`<br>`OK` |
| **12** | `AT+STATICIP` | `AT+STATICIP=1,"192.168.1.100","255.255.255.0","192.168.1.1","8.8.8.8"`<br>`AT+STATICIP=0` | Cấu hình IP Tĩnh (Static IP) hoặc quay về DHCP động.<br>- Tham số 1: `1` (bật) / `0` (tắt DHCP).<br>- Tham số 2..5: IP, Netmask, Gateway, DNS. | `+STATICIP: ENABLED (192.168.1.100)`<br>`OK` |
| **13** | `AT+WIFIRESTART` | `AT+WIFIRESTART` | Ngắt và kết nối lại mạng Wi-Fi đã lưu trong Flash NVS. | `+WIFIRESTART: RESTARTING...`<br>`OK` |
| **14** | `AT+RST` | `AT+RST`<br>`AT+RESTART` | Khởi động lại vi điều khiển ESP32-S2. | `+RST: REBOOTING HARDWARE...`<br>`OK` |

---

##  Sơ Đồ Chân Phần Cứng (Hardware Pinout)

Thiết bị sử dụng chip ESP32-S2 điều khiển màn hình ST7789 240x320 qua bus vi xử lý 8-bit Intel 8080:

### 1. Đường Dữ Liệu 8-bit (Data Bus D0 - D7)
| ST7789 Data Pin | ESP32-S2 GPIO |
| :---: | :---: |
| D0 | GPIO 33 |
| D1 | GPIO 34 |
| D2 | GPIO 35 |
| D3 | GPIO 36 |
| D4 | GPIO 37 |
| D5 | GPIO 38 |
| D6 | GPIO 9 |
| D7 | GPIO 10 |

### 2. Đường Điều Khiển Bus (Control Pins)
| Chân tín hiệu | ESP32-S2 GPIO | Trạng thái tích cực / Chức năng |
| :--- | :---: | :--- |
| **CS** (Chip Select) | GPIO 11 | Mức thấp (0) khi truyền lệnh/dữ liệu |
| **DC / RS** (Data/Command) | GPIO 13 | Mức thấp (0): Command / Mức cao (1): Data |
| **WR** (Write Enable) | GPIO 12 | Kích sườn xung lên ($0 \rightarrow 1$) để chốt byte |
| **RST** (Reset) | GPIO 21 | Reset cứng phần cứng ST7789 |
| **BK_LIGHT** (Backlight) | GPIO 18 | Mức cao (1): Bật đèn nền LED màn hình |

### 3. Nguồn & Kích Hoạt Phần Cứng
| Chức năng | ESP32-S2 GPIO | Mức logic thiết lập |
| :--- | :---: | :--- |
| Power Control 1 | GPIO 42 | 1 (Cấp nguồn VDD LCD) |
| Power Control 2 | GPIO 14 | 1 (Enable) |
| Auxiliary Pins | GPIO 40, GPIO 41 | 0 (Ground reference) |

---

##  Hướng Dẫn Biên Dịch & Nạp Firmware Trên Windows

### 1. Môi Trường ESP-IDF Trên Windows
Mở **ESP-IDF PowerShell** hoặc **ESP-IDF Command Prompt** (được tạo tự động sau khi cài ESP-IDF trên Windows).

Nếu dùng PowerShell thông thường, nạp môi trường bằng lệnh:
```powershell
. C:\Espressif\frameworks\esp-idf-v5.3.5\export.ps1
```

### 2. Biên Dịch Dự Án (Build)
Chuyển vào thư mục dự án và chạy:
```powershell
cd C:\path\to\f91_display_controller
idf.py build
```

### 3. Nạp Firmware Vào Thiết Bị (Flash)

#### Cách 1: Nạp tự động qua idf.py (Windows)
```powershell
idf.py -p COM3 flash
```
*(Thay `COM3` bằng cổng COM thực tế của F91 Standee trong Device Manager)*

#### Cách 2: Đưa mạch vào chế độ ROM Bootloader (Nếu nạp bị lỗi)
1. Nhấn giữ nút **BOOT** trên mạch.
2. Nhấn nhả nút **RESET**.
3. Thả nút **BOOT**.
4. Nạp firmware bằng `esptool` trên PowerShell:
```powershell
python -m esptool --chip esp32s2 -p COM3 -b 460800 write_flash 0x1000 build/bootloader/bootloader.bin 0x8000 build/partition_table/partition-table.bin 0x10000 build/f91_display_controller.bin
```

---

##  Hướng Dẫn Gửi Lệnh AT Trên Windows

### Cách 1: Dùng PowerShell (Windows Terminal)
Mở PowerShell và gửi lệnh AT trực tiếp tới cổng COM (Ví dụ: `COM3`):

```powershell
# Kiểm tra danh sách cổng COM hiện có
[System.IO.Ports.SerialPort]::getportnames()

# Gửi lệnh hiện QR Code 30 giây
$port = New-Object System.IO.Ports.SerialPort COM3, 115200, None, 8, One
$port.Open()
$port.WriteLine("AT+QR=https://vnpt.vn,30`r`n")
$port.Close()

# Gửi lệnh Reset Wi-Fi mở Web Config
$port.Open()
$port.WriteLine("AT+APMODE`r`n")
$port.Close()
```

### Cách 2: Dùng Extension Serial Monitor Trong VS Code
1. Cài đặt tiện ích **Serial Monitor** hoặc **ESP-IDF Terminal** trong VS Code.
2. Nhấp vào biểu tượng Serial Monitor dưới thanh trạng thái.
3. Chọn cổng `COMx`, Baud rate `115200`, Line Ending `CRLF (\r\n)`.
4. Bấm **Start Monitoring** và nhập các lệnh `AT+QR=...`, `AT+APMODE` vào ô chat.

### Cách 3: Dùng Phần Mềm SSCOM / PuTTY / TeraTerm (Windows)
1. Tải và mở **SSCOM** hoặc **TeraTerm**.
2. Chọn Port: `COMx`, Baud rate: `115200`, DataBits: `8`, StopBits: `1`, Parity: `None`.
3. Tích chọn ô **Send With CRLF** (`\r\n`).
4. Gõ lệnh `AT+QR=https://vnpt.vn,30` và bấm **Send**.

---

##  Hướng Dẫn Nạp Ảnh Thô (Raw RGB565 Image Upload)

Chế độ ảnh thô cho phép hiển thị hình ảnh poster quảng cáo tùy biến trực tiếp lên màn hình $240 \times 320$.

### Cách 1: Nạp bằng Script Python (Tự Động Resize & Chuyển Đổi RGB565)
Công cụ tích hợp sẵn cho phép chọn file ảnh bất kỳ (`.jpg`, `.png`, `.bmp`), tự động căn chỉnh về $240 \times 320$ và bắn qua cổng COM trên Windows:

```powershell
# Cài đặt thư viện pillow và pyserial (nếu chưa có)
pip install pillow pyserial

# Hiển thị poster vĩnh viễn trên COM3
python send_image.py poster.jpg -p COM3

# Hiển thị poster có đếm ngược 30 giây rồi về màn hình chờ
python send_image.py poster.png -p COM3 -t 30
```

### Cách 2: Tạo file `.bin` bằng phần mềm Image2Lcd và gửi qua SSCOM
1. Mở phần mềm **Image2Lcd**:
   - Open file ảnh bất kỳ.
   - Output file type: `*.bin`
   - BitsPixel: `16bit TrueColor (RGB565)`.
   - Max Width & Height: `240 x 320`.
   - Scan mode: `Horizon scan / Top to bottom`.
   - **Bỏ chọn ô "Include Head Data"** (để dung lượng file đúng chính xác $153.600$ bytes).
   - Bấm **Save** để lưu file `image.bin`.
2. Truyền file qua SSCOM trên Windows:
   - **Bước 1**: Gửi lệnh text `AT+DTIME=0` (hoặc `AT+DTIME=30`). Thiết bị trả về `OK`.
   - **Bước 2**: Sử dụng chức năng **Send File** trên SSCOM chọn file `image.bin` vừa tạo và bấm gửi.
   - Thiết bị sẽ vẽ trực tiếp từng pixel lên màn hình và trả về `+DTIME: OK`.

---

## Hướng Dẫn Cấu Hình Wi-Fi Qua Web Config (Captive Portal)

1. **Gửi lệnh kích hoạt Web Config**: Gửi lệnh `AT+APMODE` hoặc `AT+WIFIRESET` từ máy tính/POS.
2. **Kết nối Wi-Fi trên Điện thoại / Máy tính**:
   - Tên Wi-Fi (SSID): `ESP32_WiFi_Setup`
   - Mật khẩu: `12345678`
3. **Trang Cấu Hình**:
   - Điện thoại sẽ tự động bật cửa sổ Pop-up **VNPT Web Config**.
   - Nếu không tự nảy, mở trình duyệt truy cập: `http://192.168.4.1`
4. **Thao tác**: Chọn Wi-Fi, nhập mật khẩu (có thể tích chọn cấu hình IP Tĩnh) và bấm **Lưu & Kết Nối**. Cửa sổ cài đặt trên điện thoại sẽ tự động đóng sau khi hoàn tất.

---

##  Giấy Phép (License)
Phát triển bởi VNPT Standee Payment Firmware Team © 2026. Tất cả các quyền được bảo hộ.
