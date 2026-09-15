#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_io_i80.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"

#include "vnpt_bg_data.h"
#include "vnpt_logo_center.h"
#include "qrcode.h"
#include "qrcodegen.h"

static const char *TAG = "F91_VNPT_FIRMWARE";

#define LCD_H_RES        240
#define LCD_V_RES        320
#define FRAME_BUF_SIZE   (LCD_H_RES * LCD_V_RES * sizeof(uint16_t))

// Pinout mapping for ST7789V 8-bit i80 Parallel LCD
#define PIN_BK_LIGHT     21
#define PIN_POWER_42     42
#define PIN_GPIO41       41
#define PIN_GPIO40       40
#define PIN_GPIO14       14

#define PIN_LCD_DC       12
#define PIN_LCD_WR       11
#define PIN_LCD_CS       13
#define PIN_LCD_RST      18

static const int data_pins[8] = {10, 9, 8, 7, 6, 5, 3, 2};

// Global handles & buffers
static esp_lcd_panel_handle_t panel_handle = NULL;
static uint16_t *frame_buffer = NULL; // Internal DMA-capable RAM buffer

// Helper macro: Convert RGB to 16-bit i80 bus format (HighByte first, then LowByte)
#define MAKE_COLOR(r, g, b) ({ \
    uint8_t r5 = ((uint8_t)(r) >> 3) & 0x1F; \
    uint8_t g6 = ((uint8_t)(g) >> 2) & 0x3F; \
    uint8_t b5 = ((uint8_t)(b) >> 3) & 0x1F; \
    uint8_t hb = (r5 << 3) | (g6 >> 3); \
    uint8_t lb = ((g6 & 0x07) << 5) | b5; \
    (uint16_t)(((uint16_t)lb << 8) | hb); \
})

// Color Palette (Official VNPT Brand Colors)
#define COLOR_WHITE      0xFFFF
#define COLOR_BLACK      0x0000
#define COLOR_VNPT_BLUE  MAKE_COLOR(0, 90, 171)   // Official Vector VNPT Blue RGB (0, 90, 171)
#define COLOR_LIGHT_BG   MAKE_COLOR(240, 244, 248) // Clean light gray
#define COLOR_RED        MAKE_COLOR(235, 50, 50)

// Device States
typedef enum {
    STATE_IDLE_BG,
    STATE_QR_DISPLAY,
    STATE_RAW_STREAM
} app_state_t;

static app_state_t current_state = STATE_IDLE_BG;
static int default_timeout_sec = 30;
static int current_countdown = 30;
static char current_qr_payload[256] = "https://vnpt.vn";

// Streaming buffer for raw 150KB legacy mode
#define STREAM_CHUNK_LINES 10
#define STREAM_CHUNK_BYTES (LCD_H_RES * STREAM_CHUNK_LINES * 2)
static uint8_t stream_buf[STREAM_CHUNK_BYTES];
static size_t stream_buf_offset = 0;
static int stream_current_y = 0;

// Ensure Internal DMA RAM buffer is allocated
static bool ensure_frame_buffer(void) {
    if (!frame_buffer) {
        frame_buffer = heap_caps_malloc(FRAME_BUF_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (!frame_buffer) {
            ESP_LOGE(TAG, "Failed to allocate DMA internal RAM frame buffer!");
            return false;
        }
    }
    return true;
}

// Fill rectangle inside frame_buffer
static void fb_fill_rect(int x1, int y1, int width, int height, uint16_t color) {
    if (!ensure_frame_buffer()) return;
    int x2 = x1 + width;
    int y2 = y1 + height;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > LCD_H_RES) x2 = LCD_H_RES;
    if (y2 > LCD_V_RES) y2 = LCD_V_RES;

    for (int y = y1; y < y2; y++) {
        uint16_t *row = &frame_buffer[y * LCD_H_RES];
        for (int x = x1; x < x2; x++) {
            row[x] = color;
        }
    }
}

// Fill rounded rectangle inside frame_buffer
static void fb_fill_rounded_rect(int x1, int y1, int width, int height, int radius, uint16_t color) {
    if (!ensure_frame_buffer()) return;
    int x2 = x1 + width;
    int y2 = y1 + height;
    
    for (int y = y1; y < y2; y++) {
        if (y < 0 || y >= LCD_V_RES) continue;
        uint16_t *row = &frame_buffer[y * LCD_H_RES];
        for (int x = x1; x < x2; x++) {
            if (x < 0 || x >= LCD_H_RES) continue;

            int dx = 0, dy = 0;
            if (x < x1 + radius) dx = (x1 + radius) - x;
            else if (x >= x2 - radius) dx = x - (x2 - radius - 1);

            if (y < y1 + radius) dy = (y1 + radius) - y;
            else if (y >= y2 - radius) dy = y - (y2 - radius - 1);

            if (dx * dx + dy * dy <= radius * radius) {
                row[x] = color;
            }
        }
    }
}

// Display embedded 240x320 Pure White VNPT background image by copying from Flash to Internal DMA RAM
static void draw_vnpt_background(void) {
    ESP_LOGI(TAG, "🖼️ Displaying Official Pure White VNPT Standby Background Image...");
    if (!ensure_frame_buffer()) return;
    memcpy(frame_buffer, vnpt_bg_pixels, FRAME_BUF_SIZE);
    esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, LCD_H_RES, LCD_V_RES, frame_buffer);
    current_state = STATE_IDLE_BG;
}

// 8x12 ASCII font for digits
static const uint8_t font_8x12_digits[10][12] = {
    {0x3E, 0x63, 0x67, 0x6F, 0x7B, 0x73, 0x63, 0x63, 0x3E, 0x00, 0x00, 0x00}, // 0
    {0x0C, 0x1C, 0x3C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x3E, 0x00, 0x00, 0x00}, // 1
    {0x3E, 0x63, 0x03, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x7F, 0x00, 0x00, 0x00}, // 2
    {0x3E, 0x63, 0x03, 0x03, 0x1E, 0x03, 0x03, 0x63, 0x3E, 0x00, 0x00, 0x00}, // 3
    {0x06, 0x0E, 0x1E, 0x36, 0x66, 0x7F, 0x06, 0x06, 0x0F, 0x00, 0x00, 0x00}, // 4
    {0x7F, 0x60, 0x60, 0x7C, 0x03, 0x03, 0x03, 0x63, 0x3E, 0x00, 0x00, 0x00}, // 5
    {0x1C, 0x30, 0x60, 0x60, 0x7C, 0x63, 0x63, 0x63, 0x3E, 0x00, 0x00, 0x00}, // 6
    {0x7F, 0x63, 0x03, 0x06, 0x0C, 0x18, 0x18, 0x18, 0x18, 0x00, 0x00, 0x00}, // 7
    {0x3E, 0x63, 0x63, 0x63, 0x3E, 0x63, 0x63, 0x63, 0x3E, 0x00, 0x00, 0x00}, // 8
    {0x3E, 0x63, 0x63, 0x63, 0x3E, 0x03, 0x03, 0x06, 0x3C, 0x00, 0x00, 0x00}  // 9
};

static uint16_t *badge_buf = NULL;

static void draw_countdown_badge(int seconds) {
    int badge_w = 100;
    int badge_h = 26;
    int badge_x = (LCD_H_RES - badge_w) / 2; // 70
    int badge_y = 282;

    if (!badge_buf) {
        badge_buf = heap_caps_malloc(badge_w * badge_h * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    }
    if (!badge_buf) return;

    // 1. Fill dedicated DMA buffer with VNPT Blue
    for (int i = 0; i < badge_w * badge_h; i++) {
        badge_buf[i] = COLOR_VNPT_BLUE;
    }

    // Round badge corners
    int radius = 8;
    for (int y = 0; y < badge_h; y++) {
        for (int x = 0; x < badge_w; x++) {
            int dx = 0, dy = 0;
            if (x < radius) dx = radius - x;
            else if (x >= badge_w - radius) dx = x - (badge_w - radius - 1);

            if (y < radius) dy = radius - y;
            else if (y >= badge_h - radius) dy = y - (badge_h - radius - 1);

            if (dx * dx + dy * dy > radius * radius) {
                badge_buf[y * badge_w + x] = COLOR_LIGHT_BG;
            }
        }
    }

    // 2. Render countdown digits into badge_buf
    int tens = (seconds / 10) % 10;
    int ones = seconds % 10;

    for (int d = 0; d < 2; d++) {
        int digit = (d == 0) ? tens : ones;
        int start_x = (d == 0) ? 40 : 52;
        int start_y = 7;
        const uint8_t *glyph = font_8x12_digits[digit];
        for (int row = 0; row < 12; row++) {
            uint8_t line = glyph[row];
            int py = start_y + row;
            for (int col = 0; col < 8; col++) {
                int px = start_x + col;
                if (px < badge_w && py < badge_h) {
                    if (line & (0x80 >> col)) {
                        badge_buf[py * badge_w + px] = COLOR_WHITE;
                    }
                }
            }
        }
    }

    // 3. Update ONLY the 100x26 badge region over DMA (Flicker-Free, No Full Screen Refresh!)
    esp_lcd_panel_draw_bitmap(panel_handle, badge_x, badge_y, badge_x + badge_w, badge_y + badge_h, badge_buf);
}

static uint8_t *qr_code_buf = NULL;
static uint8_t *qr_temp_buf = NULL;

static bool ensure_qr_buffers(void) {
    if (!qr_code_buf) {
        qr_code_buf = heap_caps_malloc(qrcodegen_BUFFER_LEN_MAX, MALLOC_CAP_8BIT);
    }
    if (!qr_temp_buf) {
        qr_temp_buf = heap_caps_malloc(qrcodegen_BUFFER_LEN_MAX, MALLOC_CAP_8BIT);
    }
    return (qr_code_buf != NULL && qr_temp_buf != NULL);
}

// Generate & Render QR Code screen into frame_buffer and flush to LCD using Nayuki QR Engine
static void render_qr_payment_screen(const char *qr_data, int timeout_sec) {
    ESP_LOGI(TAG, "📲 Rendering Payment QR Screen. Payload: %s, Timeout: %ds", qr_data, timeout_sec);

    if (!ensure_frame_buffer() || !ensure_qr_buffers()) return;

    // 1. Fill Background with bright light gray
    fb_fill_rect(0, 0, LCD_H_RES, LCD_V_RES, COLOR_LIGHT_BG);

    // 2. Draw Top Banner (VNPT Blue)
    fb_fill_rect(0, 0, LCD_H_RES, 45, COLOR_VNPT_BLUE);

    // 3. Generate QR Code Matrix using Nayuki's ISO/IEC 18004 standard qrcodegen library
    uint8_t *qrcode = qr_code_buf;
    uint8_t *tempBuffer = qr_temp_buf;

    bool ok = qrcodegen_encodeText(qr_data, tempBuffer, qrcode, qrcodegen_Ecc_QUARTILE,
                                   qrcodegen_VERSION_MIN, 20,
                                   qrcodegen_Mask_AUTO, true);
    if (!ok) {
        ok = qrcodegen_encodeText(qr_data, tempBuffer, qrcode, qrcodegen_Ecc_MEDIUM,
                                   qrcodegen_VERSION_MIN, 20,
                                   qrcodegen_Mask_AUTO, true);
    }
    if (!ok) {
        ok = qrcodegen_encodeText(qr_data, tempBuffer, qrcode, qrcodegen_Ecc_LOW,
                                   qrcodegen_VERSION_MIN, 20,
                                   qrcodegen_Mask_AUTO, true);
    }

    if (!ok) {
        ESP_LOGE(TAG, "Failed to encode QR payload!");
        return;
    }

    int size = qrcodegen_getSize(qrcode);
    ESP_LOGI(TAG, "✅ QR generated successfully via Nayuki engine! Size: %dx%d", size, size);

    // 4. Calculate QR display bounds (MAXIMIZED 216x216 pixels)
    int qr_box_size = 216;
    int qr_box_x = (LCD_H_RES - qr_box_size) / 2; // 12px margins
    int qr_box_y = 54;

    // Pure white rounded card background for QR with 12px radius
    fb_fill_rounded_rect(qr_box_x, qr_box_y, qr_box_size, qr_box_size, 12, COLOR_WHITE);

    int quiet_zone = 1;
    int total_modules = size + quiet_zone * 2;
    int scale = qr_box_size / total_modules;
    if (scale < 1) scale = 1;

    int actual_qr_width = total_modules * scale;
    int start_offset_x = qr_box_x + (qr_box_size - actual_qr_width) / 2 + quiet_zone * scale;
    int start_offset_y = qr_box_y + (qr_box_size - actual_qr_width) / 2 + quiet_zone * scale;

    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            bool is_black = qrcodegen_getModule(qrcode, x, y);
            uint16_t color = is_black ? COLOR_BLACK : COLOR_WHITE;
            int px = start_offset_x + x * scale;
            int py = start_offset_y + y * scale;
            fb_fill_rect(px, py, scale, scale, color);
        }
    }

    // 5. Overlay Official Center Logo (36x36 with 8px rounded white card)
    int logo_x = (LCD_H_RES - VNPT_LOGO_WIDTH) / 2;
    int logo_y = qr_box_y + (qr_box_size - VNPT_LOGO_HEIGHT) / 2;

    int logo_bg_w = VNPT_LOGO_WIDTH + 8; // 44px
    int logo_bg_h = VNPT_LOGO_HEIGHT + 8; // 44px
    int logo_bg_x = logo_x - 4;
    int logo_bg_y = logo_y - 4;

    // White rounded card background for center logo (8px radius)
    fb_fill_rounded_rect(logo_bg_x, logo_bg_y, logo_bg_w, logo_bg_h, 8, COLOR_WHITE);

    // Copy official logo pixels to frame_buffer
    for (int ly = 0; ly < VNPT_LOGO_HEIGHT; ly++) {
        for (int lx = 0; lx < VNPT_LOGO_WIDTH; lx++) {
            uint16_t pixel = vnpt_logo_center_pixels[ly * VNPT_LOGO_WIDTH + lx];
            if (pixel != COLOR_WHITE) {
                frame_buffer[(logo_y + ly) * LCD_H_RES + (logo_x + lx)] = pixel;
            }
        }
    }

    // 6. Draw initial countdown badge inside frame_buffer
    current_countdown = timeout_sec;
    draw_countdown_badge(current_countdown);

    // 7. Flush entire frame_buffer to LCD in ONE single DMA transaction!
    esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, LCD_H_RES, LCD_V_RES, frame_buffer);

    current_state = STATE_QR_DISPLAY;
}

// Timer Task for Countdown & Auto-Reset to VNPT Standby Background
static void timer_task(void *pvParameters) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (current_state == STATE_QR_DISPLAY) {
            current_countdown--;
            if (current_countdown > 0) {
                draw_countdown_badge(current_countdown);
            } else {
                ESP_LOGI(TAG, "⏰ Timeout reached. Returning to Official VNPT Standby Background...");
                draw_vnpt_background();
            }
        }
    }
}

// AT Command Parser
static void process_at_command(const char *cmd) {
    ESP_LOGI(TAG, "📩 Received AT Command: [%s]", cmd);

    // Create uppercase version for case-insensitive matching
    char upper_cmd[300];
    size_t len = strlen(cmd);
    if (len >= sizeof(upper_cmd)) len = sizeof(upper_cmd) - 1;
    for (size_t i = 0; i < len; i++) {
        upper_cmd[i] = (cmd[i] >= 'a' && cmd[i] <= 'z') ? (cmd[i] - 32) : cmd[i];
    }
    upper_cmd[len] = '\0';

    const char *qr_ptr = strstr(upper_cmd, "AT+QR=");
    const char *dtime_ptr = strstr(upper_cmd, "AT+DTIME=");
    const char *bg_ptr = strstr(upper_cmd, "AT+BG");
    const char *clear_ptr = strstr(upper_cmd, "AT+CLEAR");
    const char *ver_ptr = strstr(upper_cmd, "AT+VER");

    if (qr_ptr) {
        char payload[256] = {0};
        int timeout = default_timeout_sec;
        const char *orig_param = cmd + ((qr_ptr - upper_cmd) + 6);

        const char *comma = strrchr(orig_param, ',');
        if (comma && comma > orig_param) {
            size_t payload_len = comma - orig_param;
            if (payload_len < sizeof(payload)) {
                strncpy(payload, orig_param, payload_len);
                payload[payload_len] = '\0';
                timeout = atoi(comma + 1);
                if (timeout <= 0) timeout = 30;
            }
        } else {
            strncpy(payload, orig_param, sizeof(payload) - 1);
        }

        // Strip surrounding quotes if present
        size_t plen = strlen(payload);
        if (plen >= 2 && payload[0] == '"' && payload[plen - 1] == '"') {
            payload[plen - 1] = '\0';
            memmove(payload, payload + 1, plen - 1);
        }

        // Strip trailing whitespace / carriage return
        plen = strlen(payload);
        while (plen > 0 && (payload[plen - 1] == ' ' || payload[plen - 1] == '\r' || payload[plen - 1] == '\n')) {
            payload[--plen] = '\0';
        }

        ESP_LOGI(TAG, "🎯 Render QR Payload: [%s], Timeout: %ds", payload, timeout);
        strncpy(current_qr_payload, payload, sizeof(current_qr_payload) - 1);
        render_qr_payment_screen(current_qr_payload, timeout);
        printf("+QR: OK\r\n");
        fflush(stdout);
    }
    else if (dtime_ptr) {
        int t = atoi(dtime_ptr + 9);
        if (t == 0) {
            current_state = STATE_RAW_STREAM;
            stream_buf_offset = 0;
            stream_current_y = 0;
            ESP_LOGI(TAG, "📥 Switched to RAW_STREAM mode...");
        } else {
            default_timeout_sec = t;
        }
        printf("OK\r\n");
        fflush(stdout);
    }
    else if (bg_ptr || clear_ptr) {
        draw_vnpt_background();
        printf("OK\r\n");
        fflush(stdout);
    }
    else if (ver_ptr) {
        printf("+VER: F91_VNPT_STANDEE_V1.0\r\nOK\r\n");
        fflush(stdout);
    }
}

// USB Serial Reader Task
static void usb_cdc_task(void *pvParameters) {
    char rx_line[300];
    size_t rx_idx = 0;
    uint8_t ch;
    int idle_counter = 0;

    // Set non-blocking mode on stdin (file descriptor 0)
    fcntl(0, F_SETFL, O_NONBLOCK);

    while (1) {
        int bytes = read(0, &ch, 1);
        if (bytes > 0) {
            idle_counter = 0;
            if (current_state == STATE_RAW_STREAM) {
                stream_buf[stream_buf_offset++] = ch;
                if (stream_buf_offset >= STREAM_CHUNK_BYTES) {
                    int next_y = stream_current_y + STREAM_CHUNK_LINES;
                    if (next_y <= LCD_V_RES) {
                        esp_lcd_panel_draw_bitmap(panel_handle, 0, stream_current_y, LCD_H_RES, next_y, (uint16_t*)stream_buf);
                        stream_current_y = next_y;
                    }
                    stream_buf_offset = 0;

                    if (stream_current_y >= LCD_V_RES) {
                        ESP_LOGI(TAG, "🎉 Full raw frame rendered!");
                        stream_current_y = 0;
                        current_state = STATE_IDLE_BG;
                    }
                }
            } else {
                if (ch == '\r' || ch == '\n') {
                    if (rx_idx > 0) {
                        rx_line[rx_idx] = '\0';
                        process_at_command(rx_line);
                        rx_idx = 0;
                    }
                } else if (ch >= 32 && ch != 127 && rx_idx < sizeof(rx_line) - 1) {
                    rx_line[rx_idx++] = ch;
                }
            }
        } else {
            if (rx_idx > 0 && current_state != STATE_RAW_STREAM) {
                idle_counter++;
                if (idle_counter > 20) { // 100ms idle timeout auto-triggers AT parser
                    rx_line[rx_idx] = '\0';
                    process_at_command(rx_line);
                    rx_idx = 0;
                    idle_counter = 0;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

void app_main(void) {
    // Unbuffered I/O
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "🚀 Starting F91 VNPT Standby & Payment QR Firmware...");

    // 1. Initialize Control GPIOs
    gpio_reset_pin(PIN_POWER_42);
    gpio_set_direction(PIN_POWER_42, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_POWER_42, 1);

    gpio_reset_pin(PIN_GPIO41);
    gpio_set_direction(PIN_GPIO41, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_GPIO41, 0);

    gpio_reset_pin(PIN_GPIO40);
    gpio_set_direction(PIN_GPIO40, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_GPIO40, 0);

    gpio_reset_pin(PIN_GPIO14);
    gpio_set_direction(PIN_GPIO14, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_GPIO14, 1);

    gpio_reset_pin(PIN_BK_LIGHT);
    gpio_set_direction(PIN_BK_LIGHT, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_BK_LIGHT, 0);

    vTaskDelay(pdMS_TO_TICKS(200));

    // 2. Initialize i80 Bus (4MHz for ultra-stable latching)
    esp_lcd_i80_bus_handle_t i80_bus = NULL;
    esp_lcd_i80_bus_config_t bus_config = {
        .dc_gpio_num = PIN_LCD_DC,
        .wr_gpio_num = PIN_LCD_WR,
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .bus_width = 8,
        .max_transfer_bytes = LCD_H_RES * LCD_V_RES * sizeof(uint16_t),
    };
    for (int i = 0; i < 8; i++) {
        bus_config.data_gpio_nums[i] = data_pins[i];
    }
    ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus_config, &i80_bus));

    // 3. Initialize Panel IO (CS=13, pclk=4MHz)
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_i80_config_t io_config = {
        .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = 4 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .dc_levels = {
            .dc_idle_level  = 0,
            .dc_cmd_level   = 0,
            .dc_dummy_level = 0,
            .dc_data_level  = 1,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i80(i80_bus, &io_config, &io_handle));

    // 4. Initialize ST7789 Panel
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_endian = LCD_RGB_ENDIAN_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));

    esp_lcd_panel_reset(panel_handle);
    esp_lcd_panel_init(panel_handle);
    esp_lcd_panel_invert_color(panel_handle, false); // Hardware invert_color = false for correct white background
    esp_lcd_panel_disp_on_off(panel_handle, true);

    // Turn backlight ON
    gpio_set_level(PIN_BK_LIGHT, 1);

    // 5. Render default VNPT Standby Background Image
    draw_vnpt_background();

    // 6. Create Tasks
    xTaskCreate(usb_cdc_task, "usb_cdc_task", 8192, NULL, 5, NULL);
    xTaskCreate(timer_task, "timer_task", 4096, NULL, 5, NULL);

    // 7. Auto Demo: Show default VNPT Payment QR code after bootup
    vTaskDelay(pdMS_TO_TICKS(1500));
    if (current_state == STATE_IDLE_BG) {
        ESP_LOGI(TAG, "✨ Bootup Auto Demo: Generating VNPT Payment QR...");
        render_qr_payment_screen("https://vnpt.vn", 30);
    }
}