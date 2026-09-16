#!/usr/bin/env python3
import sys
import time
import os

try:
    import serial
except ImportError:
    print("Thu vien 'pyserial' chưa được cài đặt. Hãy chạy: pip install pyserial")
    sys.exit(1)

def send_image(port, bin_path):
    if not os.path.exists(bin_path):
        print(f"Loi: Khong tim thấy file '{bin_path}'")
        return

    with open(bin_path, "rb") as f:
        data = f.read()

    print(f"-> Đã tải {len(data)} bytes từ file {bin_path}")
    if len(data) != 153600:
        print(f"Canh bao: Dung lượng chuẩn là 153,600 bytes (240x320 RGB565). File này có {len(data)} bytes.")

    print(f"-> Đang mở cổng {port}...")
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = 115200
    ser.dtr = False
    ser.rts = False
    ser.open()

    # Cho ESP32 khôi phuc sau khi mo cong
    time.sleep(1.0)
    ser.read_all()

    print("-> Gửi lệnh AT+DTIME=0...")
    ser.write(b"AT+DTIME=0\r\n")

    # Đợi phản hồi OK từ ESP32
    resp = b""
    for _ in range(20):
        if ser.in_waiting:
            resp += ser.read(ser.in_waiting)
            if b"OK" in resp:
                break
        time.sleep(0.05)

    print(f"-> Phản hồi: {resp.decode(errors='ignore').strip()}")

    print(f"-> Đang truyền {len(data)} bytes dữ liệu ảnh thô...")
    t0 = time.time()
    ser.write(data)
    ser.flush()
    elapsed = time.time() - t0
    print(f"-> Gửi xong trong {elapsed:.3f} giây! Tốc độ: {len(data)/elapsed/1024:.1f} KB/s")

    time.sleep(0.5)
    if ser.in_waiting:
        print("-> Phản hồi từ thiết bị:\n", ser.read_all().decode(errors='ignore').strip())

    ser.close()
    print("-> Hoàn tất!")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Cách dùng: python3 send_img.py <cổng_serial> <file_anh.bin>")
        print("Ví dụ Linux:   python3 send_img.py /dev/ttyACM1 test_image.bin")
        print("Ví dụ Windows: python send_img.py COM3 test_image.bin")
        sys.exit(1)

    send_image(sys.argv[1], sys.argv[2])
