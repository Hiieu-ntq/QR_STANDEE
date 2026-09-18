#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_io_i80.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"

#include "esp_flash.h"
#include "vnpt_bg_data.h"
#include "vnpt_logo_center.h"
#include "qrcode.h"
#include "qrcodegen.h"
#include "wifi_manager.h"
#include "internet_check.h"

static const char *TAG = "F91_VNPT_FIRMWARE";

#define LCD_H_RES               240
#define LCD_V_RES               320
#define TILE_HEIGHT             32
#define FRAME_BUF_SIZE          (LCD_H_RES * TILE_HEIGHT * sizeof(uint16_t)) // 240 * 32 * 2 = 15,360 bytes (15.3 KB)
#define FULL_FRAME_SIZE         (LCD_H_RES * LCD_V_RES * sizeof(uint16_t))
#define FLASH_CUSTOM_BG_OFFSET  0x1DA000
#define FLASH_CUSTOM_BG_ERASE_SIZE 0x26000 // 38 sectors * 4096 = 155648 (sector aligned)

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
static uint16_t *frame_buffer = NULL; // 15.3 KB DMA-capable Tile Buffer
static uint16_t badge_buffer[68 * 24]; // Dedicated 3.2 KB buffer for countdown badge (stride 68)
static SemaphoreHandle_t lcd_trans_sem = NULL;

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
    STATE_IDLE_BG,             // Hien thi anh nen VNPT mac dinh
    STATE_IDLE_CUSTOM_BG,      // Hien thi anh nen tho custom (vinh vien)
    STATE_CUSTOM_BG_COUNTDOWN, // Hien thi anh nen tho custom co dem nguoc (vd 30s)
    STATE_QR_DISPLAY,          // Hien thi ma QR dem nguoc
    STATE_RAW_STREAM           // Dang nhan 153600 bytes qua Serial
} app_state_t;

static app_state_t current_state = STATE_IDLE_BG;
static int default_timeout_sec = 30;
static int current_countdown = 30;
static int custom_bg_countdown = 0;
static int raw_stream_display_timeout = 0;
static bool has_custom_bg = false;
static char current_qr_payload[512] = "https://vnpt.vn";

// Forward declaration
static void render_qr_payment_screen(const char *qr_data, int timeout_sec);

// RAW STREAM mode tracking (10 tiles x 15360 bytes = 153600 bytes)
static size_t stream_buf_offset = 0;
static int stream_tile_index = 0;
static bool stream_skip_whitespace = false;

// Synchronous LCD Draw Bitmap using DMA Done ISR Semaphore
static bool IRAM_ATTR lcd_on_trans_done(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx) {
    BaseType_t high_task_awoken = pdFALSE;
    if (lcd_trans_sem) {
        xSemaphoreGiveFromISR(lcd_trans_sem, &high_task_awoken);
    }
    return high_task_awoken == pdTRUE;
}

static void draw_bitmap_sync(int x_start, int y_start, int x_end, int y_end, const void *color_data) {
    esp_lcd_panel_draw_bitmap(panel_handle, x_start, y_start, x_end, y_end, color_data);
    if (lcd_trans_sem) {
        xSemaphoreTake(lcd_trans_sem, portMAX_DELAY);
    }
}

// Ensure Internal DMA RAM tile buffer is allocated (15.3 KB)
static bool ensure_frame_buffer(void) {
    if (!frame_buffer) {
        frame_buffer = heap_caps_malloc(FRAME_BUF_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (!frame_buffer) {
            ESP_LOGE(TAG, "Failed to allocate DMA tile buffer (%d bytes)!", (int)FRAME_BUF_SIZE);
            return false;
        }
        ESP_LOGI(TAG, "Successfully allocated 15.3KB DMA Tile Buffer!");
    }
    return true;
}

// Fill rectangle inside tile/region
static void fb_fill_rect(int x1, int y1, int width, int height, uint16_t color) {
    if (!ensure_frame_buffer()) return;
    int x2 = x1 + width;
    int y2 = y1 + height;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > LCD_H_RES) x2 = LCD_H_RES;
    if (y2 > TILE_HEIGHT) y2 = TILE_HEIGHT;

    for (int y = y1; y < y2; y++) {
        for (int x = x1; x < x2; x++) {
            frame_buffer[y * LCD_H_RES + x] = color;
        }
    }
}

// Fill rounded rectangle inside tile/region
static void fb_fill_rounded_rect(int x1, int y1, int width, int height, int radius, uint16_t color) {
    if (!ensure_frame_buffer()) return;
    int x2 = x1 + width - 1;
    int y2 = y1 + height - 1;

    for (int y = y1; y <= y2; y++) {
        for (int x = x1; x <= x2; x++) {
            bool draw = true;
            if (x < x1 + radius && y < y1 + radius) {
                int dx = x - (x1 + radius);
                int dy = y - (y1 + radius);
                if (dx * dx + dy * dy > radius * radius) draw = false;
            } else if (x > x2 - radius && y < y1 + radius) {
                int dx = x - (x2 - radius);
                int dy = y - (y1 + radius);
                if (dx * dx + dy * dy > radius * radius) draw = false;
            } else if (x < x1 + radius && y > y2 - radius) {
                int dx = x - (x1 + radius);
                int dy = y - (y2 - radius);
                if (dx * dx + dy * dy > radius * radius) draw = false;
            } else if (x > x2 - radius && y > y2 - radius) {
                int dx = x - (x2 - radius);
                int dy = y - (y2 - radius);
                if (dx * dx + dy * dy > radius * radius) draw = false;
            }

            if (draw && x >= 0 && x < LCD_H_RES && y >= 0 && y < TILE_HEIGHT) {
                frame_buffer[y * LCD_H_RES + x] = color;
            }
        }
    }
}

// Draw official default VNPT background image to LCD (Tile-by-Tile 15.3KB Synchronous DMA rendering)
static void draw_vnpt_background(void) {
    if (!ensure_frame_buffer()) return;

    for (int y = 0; y < LCD_V_RES; y += TILE_HEIGHT) {
        int tile_h = TILE_HEIGHT;
        if (y + tile_h > LCD_V_RES) tile_h = LCD_V_RES - y;

        int offset_pixels = y * LCD_H_RES;
        size_t bytes_to_copy = LCD_H_RES * tile_h * sizeof(uint16_t);
        memcpy(frame_buffer, vnpt_bg_pixels + offset_pixels, bytes_to_copy);

        draw_bitmap_sync(0, y, LCD_H_RES, y + tile_h, frame_buffer);
    }

    current_state = STATE_IDLE_BG;
    has_custom_bg = false;
    ESP_LOGI(TAG, "Displaying Official Pure White VNPT Standby Background Image (Tile-Rendered)...");
}

// Compact 5x8 font for ASCII 32 (' ') to 90 ('Z')
static const uint8_t font_5x8[59][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, // 32 ' '
    {0x00, 0x00, 0x5F, 0x00, 0x00}, // 33 '!'
    {0x00, 0x07, 0x00, 0x07, 0x00}, // 34 '"'
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, // 35 '#'
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, // 36 '$'
    {0x23, 0x13, 0x08, 0x64, 0x62}, // 37 '%'
    {0x36, 0x49, 0x55, 0x22, 0x50}, // 38 '&'
    {0x00, 0x05, 0x03, 0x00, 0x00}, // 39 '\''
    {0x00, 0x1C, 0x22, 0x41, 0x00}, // 40 '('
    {0x00, 0x41, 0x22, 0x1C, 0x00}, // 41 ')'
    {0x14, 0x08, 0x3E, 0x08, 0x14}, // 42 '*'
    {0x08, 0x08, 0x3E, 0x08, 0x08}, // 43 '+'
    {0x00, 0x50, 0x30, 0x00, 0x00}, // 44 ','
    {0x08, 0x08, 0x08, 0x08, 0x08}, // 45 '-'
    {0x00, 0x60, 0x60, 0x00, 0x00}, // 46 '.'
    {0x20, 0x10, 0x08, 0x04, 0x02}, // 47 '/'
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, // 48 '0'
    {0x00, 0x42, 0x7F, 0x40, 0x00}, // 49 '1'
    {0x42, 0x61, 0x51, 0x49, 0x46}, // 50 '2'
    {0x21, 0x41, 0x45, 0x4B, 0x31}, // 51 '3'
    {0x18, 0x14, 0x12, 0x7F, 0x10}, // 52 '4'
    {0x27, 0x45, 0x45, 0x45, 0x39}, // 53 '5'
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, // 54 '6'
    {0x01, 0x71, 0x09, 0x05, 0x03}, // 55 '7'
    {0x36, 0x49, 0x49, 0x49, 0x36}, // 56 '8'
    {0x06, 0x49, 0x49, 0x29, 0x1E}, // 57 '9'
    {0x00, 0x36, 0x36, 0x00, 0x00}, // 58 ':'
    {0x00, 0x56, 0x36, 0x00, 0x00}, // 59 ';'
    {0x08, 0x14, 0x22, 0x41, 0x00}, // 60 '<'
    {0x14, 0x14, 0x14, 0x14, 0x14}, // 61 '='
    {0x00, 0x41, 0x22, 0x14, 0x08}, // 62 '>'
    {0x02, 0x01, 0x51, 0x09, 0x06}, // 63 '?'
    {0x32, 0x49, 0x79, 0x41, 0x3E}, // 64 '@'
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, // 65 'A'
    {0x7F, 0x49, 0x49, 0x49, 0x36}, // 66 'B'
    {0x3E, 0x41, 0x41, 0x41, 0x22}, // 67 'C'
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, // 68 'D'
    {0x7F, 0x49, 0x49, 0x49, 0x41}, // 69 'E'
    {0x7F, 0x09, 0x09, 0x09, 0x01}, // 70 'F'
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, // 71 'G'
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, // 72 'H'
    {0x00, 0x41, 0x7F, 0x41, 0x00}, // 73 'I'
    {0x20, 0x40, 0x41, 0x3F, 0x01}, // 74 'J'
    {0x7F, 0x08, 0x14, 0x22, 0x41}, // 75 'K'
    {0x7F, 0x40, 0x40, 0x40, 0x40}, // 76 'L'
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, // 77 'M'
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, // 78 'N'
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, // 79 'O'
    {0x7F, 0x09, 0x09, 0x09, 0x06}, // 80 'P'
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, // 81 'Q'
    {0x7F, 0x09, 0x19, 0x29, 0x46}, // 82 'R'
    {0x46, 0x49, 0x49, 0x49, 0x31}, // 83 'S'
    {0x01, 0x01, 0x7F, 0x01, 0x01}, // 84 'T'
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, // 85 'U'
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, // 86 'V'
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, // 87 'W'
    {0x63, 0x14, 0x08, 0x14, 0x63}, // 88 'X'
    {0x07, 0x08, 0x70, 0x08, 0x07}, // 89 'Y'
    {0x61, 0x51, 0x49, 0x45, 0x43}  // 90 'Z'
};

static void draw_string_5x8_tile(int start_x, int start_y, const char *str, uint16_t color, int tile_y_start, int tile_h) {
    if (!str) return;
    int cur_x = start_x;
    while (*str) {
        char c = *str++;
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c >= 32 && c <= 90) {
            const uint8_t *glyph = font_5x8[c - 32];
            for (int col = 0; col < 5; col++) {
                uint8_t line = glyph[col];
                int px = cur_x + col;
                if (px >= 0 && px < LCD_H_RES) {
                    for (int row = 0; row < 8; row++) {
                        if (line & (1 << row)) {
                            int py = start_y + row;
                            if (py >= tile_y_start && py < tile_y_start + tile_h) {
                                int local_y = py - tile_y_start;
                                frame_buffer[local_y * LCD_H_RES + px] = color;
                            }
                        }
                    }
                }
            }
        }
        cur_x += 6;
    }
}

static void draw_string_5x8(int start_x, int start_y, const char *str, uint16_t color) {
    draw_string_5x8_tile(start_x, start_y, str, color, 0, TILE_HEIGHT);
}

// Draw a status bar overlay on LCD to indicate WiFi / Connecting Status with English White Text
void draw_wifi_status_overlay(const char *text, uint16_t bg_color, uint16_t text_color) {
    if (!ensure_frame_buffer()) return;

    int bar_h = 24;
    int bar_y = LCD_V_RES - bar_h;

    // Draw background bar into Tile Buffer
    fb_fill_rect(0, 0, LCD_H_RES, bar_h, bg_color);

    // Draw a small status indicator dot
    int dot_x = 12;
    int dot_y = 12;
    for (int dy = -4; dy <= 4; dy++) {
        for (int dx = -4; dx <= 4; dx++) {
            if (dx*dx + dy*dy <= 16) {
                int px = dot_x + dx;
                int py = dot_y + dy;
                if (px >= 0 && px < LCD_H_RES && py >= 0 && py < bar_h) {
                    frame_buffer[py * LCD_H_RES + px] = COLOR_WHITE;
                }
            }
        }
    }

    // Draw English status text in WHITE color
    if (text && strlen(text) > 0) {
        int text_len = strlen(text);
        int text_w = text_len * 6 - 1;
        int text_x = 24 + (LCD_H_RES - 24 - text_w) / 2;
        if (text_x < 24) text_x = 24;
        int text_y = (bar_h - 8) / 2;

        draw_string_5x8(text_x, text_y, text, COLOR_WHITE);
    }

    // Flush only the bottom status bar region using 15.3KB Tile Buffer synchronously!
    draw_bitmap_sync(0, bar_y, LCD_H_RES, LCD_V_RES, frame_buffer);
}

// Hide / Erase status bar overlay on LCD and restore original screen content underneath
void hide_wifi_status_overlay(void) {
    if (!ensure_frame_buffer()) return;

    int bar_h = 24;
    int bar_y = LCD_V_RES - bar_h;

    if (current_state == STATE_IDLE_BG) {
        int offset_pixels = bar_y * LCD_H_RES;
        size_t bytes_to_copy = LCD_H_RES * bar_h * sizeof(uint16_t);
        memcpy(frame_buffer, vnpt_bg_pixels + offset_pixels, bytes_to_copy);
        draw_bitmap_sync(0, bar_y, LCD_H_RES, LCD_V_RES, frame_buffer);
    } else if (current_state == STATE_QR_DISPLAY) {
        render_qr_payment_screen(current_qr_payload, current_countdown);
    } else if (current_state == STATE_IDLE_CUSTOM_BG || current_state == STATE_CUSTOM_BG_COUNTDOWN) {
        uint32_t flash_addr = FLASH_CUSTOM_BG_OFFSET + bar_y * LCD_H_RES * sizeof(uint16_t);
        size_t bytes = LCD_H_RES * bar_h * sizeof(uint16_t);
        if (esp_flash_read(NULL, frame_buffer, flash_addr, bytes) == ESP_OK) {
            draw_bitmap_sync(0, bar_y, LCD_H_RES, LCD_V_RES, frame_buffer);
        }
    }
}

// 8x12 Font Digits Bitmap ('0'-'9')
static const uint8_t font_8x12_digits[10][12] = {
    {0x3C,0x66,0x66,0x6E,0x76,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}, // 0
    {0x18,0x38,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x7E,0x00}, // 1
    {0x3C,0x66,0x06,0x06,0x0C,0x18,0x30,0x60,0x60,0x66,0x7E,0x00}, // 2
    {0x3C,0x66,0x06,0x06,0x1C,0x06,0x06,0x06,0x06,0x66,0x3C,0x00}, // 3
    {0x0C,0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // 4
    {0x7E,0x60,0x60,0x60,0x7C,0x06,0x06,0x06,0x06,0x66,0x3C,0x00}, // 5
    {0x3C,0x66,0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}, // 6
    {0x7E,0x66,0x06,0x0C,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, // 7
    {0x3C,0x66,0x66,0x66,0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}, // 8
    {0x3C,0x66,0x66,0x66,0x3E,0x06,0x06,0x06,0x06,0x66,0x3C,0x00}  // 9
};

// Draw countdown badge using dedicated 68x24 buffer (stride 68) to prevent row-stride mismatch
static void draw_countdown_badge(int seconds) {
    int badge_w = 68;
    int badge_h = 24;
    int badge_x = (LCD_H_RES - badge_w) / 2;
    int badge_y = 282;

    // Fill badge_buffer with rounded rectangle (RED)
    for (int y = 0; y < badge_h; y++) {
        for (int x = 0; x < badge_w; x++) {
            bool draw = true;
            int r = 6;
            if (x < r && y < r) {
                int dx = x - r; int dy = y - r;
                if (dx*dx + dy*dy > r*r) draw = false;
            } else if (x >= badge_w - r && y < r) {
                int dx = x - (badge_w - 1 - r); int dy = y - r;
                if (dx*dx + dy*dy > r*r) draw = false;
            } else if (x < r && y >= badge_h - r) {
                int dx = x - r; int dy = y - (badge_h - 1 - r);
                if (dx*dx + dy*dy > r*r) draw = false;
            } else if (x >= badge_w - r && y >= badge_h - r) {
                int dx = x - (badge_w - 1 - r); int dy = y - (badge_h - 1 - r);
                if (dx*dx + dy*dy > r*r) draw = false;
            }
            badge_buffer[y * badge_w + x] = draw ? COLOR_RED : COLOR_LIGHT_BG;
        }
    }

    int tens = (seconds / 10) % 10;
    int ones = seconds % 10;

    for (int d = 0; d < 2; d++) {
        int digit = (d == 0) ? tens : ones;
        int start_x = (d == 0) ? 18 : 36;
        int start_y = 6;
        const uint8_t *glyph = font_8x12_digits[digit];
        for (int row = 0; row < 12; row++) {
            uint8_t line = glyph[row];
            int py = start_y + row;
            for (int col = 0; col < 8; col++) {
                int px = start_x + col;
                if (px < badge_w && py < badge_h) {
                    if (line & (0x80 >> col)) {
                        badge_buffer[py * badge_w + px] = COLOR_WHITE;
                    }
                }
            }
        }
    }

    // Flush only the badge region (68x24) synchronously over DMA!
    draw_bitmap_sync(badge_x, badge_y, badge_x + badge_w, badge_y + badge_h, badge_buffer);
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

// Generate & Render QR Code screen tile-by-tile to LCD using 15.3KB Tile Buffer
static void render_qr_payment_screen(const char *qr_data, int timeout_sec) {
    ESP_LOGI(TAG, "Rendering Payment QR Screen. Payload: %s, Timeout: %ds", qr_data, timeout_sec);

    if (!ensure_frame_buffer() || !ensure_qr_buffers()) return;

    // 1. Generate QR Code Matrix using Nayuki's ISO/IEC 18004 standard qrcodegen library
    uint8_t *qrcode = qr_code_buf;
    uint8_t *tempBuffer = qr_temp_buf;

    bool ok = qrcodegen_encodeText(qr_data, tempBuffer, qrcode, qrcodegen_Ecc_QUARTILE,
                                   qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                                   qrcodegen_Mask_AUTO, true);
    if (!ok) {
        ok = qrcodegen_encodeText(qr_data, tempBuffer, qrcode, qrcodegen_Ecc_MEDIUM,
                                   qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                                   qrcodegen_Mask_AUTO, true);
    }
    if (!ok) {
        ok = qrcodegen_encodeText(qr_data, tempBuffer, qrcode, qrcodegen_Ecc_LOW,
                                   qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                                   qrcodegen_Mask_AUTO, true);
    }

    if (!ok) {
        ESP_LOGE(TAG, "Failed to encode QR payload: [%s]", qr_data);
        return;
    }

    int size = qrcodegen_getSize(qrcode);
    ESP_LOGI(TAG, "QR generated successfully! Size: %dx%d", size, size);

    // QR & Logo geometry
    int qr_box_size = 216;
    int qr_box_x = (LCD_H_RES - qr_box_size) / 2; // 12px margins
    int qr_box_y = 54;
    int quiet_zone = 1;
    int total_modules = size + quiet_zone * 2;
    int scale = qr_box_size / total_modules;
    if (scale < 1) scale = 1;

    int actual_qr_width = total_modules * scale;
    int start_offset_x = qr_box_x + (qr_box_size - actual_qr_width) / 2 + quiet_zone * scale;
    int start_offset_y = qr_box_y + (qr_box_size - actual_qr_width) / 2 + quiet_zone * scale;

    int logo_x = (LCD_H_RES - VNPT_LOGO_WIDTH) / 2;
    int logo_y = qr_box_y + (qr_box_size - VNPT_LOGO_HEIGHT) / 2;
    int logo_bg_w = VNPT_LOGO_WIDTH + 8;
    int logo_bg_h = VNPT_LOGO_HEIGHT + 8;
    int logo_bg_x = logo_x - 4;
    int logo_bg_y = logo_y - 4;

    int badge_w = 68;
    int badge_h = 24;
    int badge_x = (LCD_H_RES - badge_w) / 2;
    int badge_y = 282;

    current_countdown = timeout_sec;
    int tens = (current_countdown / 10) % 10;
    int ones = current_countdown % 10;

    // Render Tile by Tile (10 tiles total for 320px height)
    for (int tile_y_start = 0; tile_y_start < LCD_V_RES; tile_y_start += TILE_HEIGHT) {
        int tile_h = TILE_HEIGHT;
        if (tile_y_start + tile_h > LCD_V_RES) tile_h = LCD_V_RES - tile_y_start;

        // 1. Fill tile background with COLOR_LIGHT_BG
        for (int i = 0; i < LCD_H_RES * tile_h; i++) {
            frame_buffer[i] = COLOR_LIGHT_BG;
        }

        // 2. Draw Top Banner if inside this tile
        for (int py = tile_y_start; py < tile_y_start + tile_h; py++) {
            if (py < 45) {
                int local_y = py - tile_y_start;
                for (int px = 0; px < LCD_H_RES; px++) {
                    frame_buffer[local_y * LCD_H_RES + px] = COLOR_VNPT_BLUE;
                }
            }
        }
        const char *header_text = "QUET MA DE THANH TOAN";
        int text_w = strlen(header_text) * 6 - 1;
        int text_x = (LCD_H_RES - text_w) / 2;
        int text_y = (45 - 8) / 2;
        draw_string_5x8_tile(text_x, text_y, header_text, COLOR_WHITE, tile_y_start, tile_h);

        // 3. Draw QR Card background (White rounded rect)
        for (int py = tile_y_start; py < tile_y_start + tile_h; py++) {
            if (py >= qr_box_y && py < qr_box_y + qr_box_size) {
                int local_y = py - tile_y_start;
                for (int px = qr_box_x; px < qr_box_x + qr_box_size; px++) {
                    bool draw = true;
                    int r = 12;
                    int x1 = qr_box_x;
                    int y1 = qr_box_y;
                    int x2 = qr_box_x + qr_box_size - 1;
                    int y2 = qr_box_y + qr_box_size - 1;
                    if (px < x1 + r && py < y1 + r) {
                        int dx = px - (x1 + r); int dy = py - (y1 + r);
                        if (dx*dx + dy*dy > r*r) draw = false;
                    } else if (px > x2 - r && py < y1 + r) {
                        int dx = px - (x2 - r); int dy = py - (y1 + r);
                        if (dx*dx + dy*dy > r*r) draw = false;
                    } else if (px < x1 + r && py > y2 - r) {
                        int dx = px - (x1 + r); int dy = py - (y2 - r);
                        if (dx*dx + dy*dy > r*r) draw = false;
                    } else if (px > x2 - r && py > y2 - r) {
                        int dx = px - (x2 - r); int dy = py - (y2 - r);
                        if (dx*dx + dy*dy > r*r) draw = false;
                    }
                    if (draw) {
                        frame_buffer[local_y * LCD_H_RES + px] = COLOR_WHITE;
                    }
                }
            }
        }

        // 4. Draw QR Modules inside this tile
        for (int qy = 0; qy < size; qy++) {
            int py = start_offset_y + qy * scale;
            if (py + scale > tile_y_start && py < tile_y_start + tile_h) {
                for (int qx = 0; qx < size; qx++) {
                    bool is_black = qrcodegen_getModule(qrcode, qx, qy);
                    uint16_t color = is_black ? COLOR_BLACK : COLOR_WHITE;
                    int px = start_offset_x + qx * scale;

                    for (int sub_y = py; sub_y < py + scale; sub_y++) {
                        if (sub_y >= tile_y_start && sub_y < tile_y_start + tile_h) {
                            int local_y = sub_y - tile_y_start;
                            for (int sub_x = px; sub_x < px + scale; sub_x++) {
                                if (sub_x >= 0 && sub_x < LCD_H_RES) {
                                    frame_buffer[local_y * LCD_H_RES + sub_x] = color;
                                }
                            }
                        }
                    }
                }
            }
        }

        // 5. Draw Center Logo Card (White rounded rectangle with 6px corner radius) & Pixels
        for (int py = tile_y_start; py < tile_y_start + tile_h; py++) {
            if (py >= logo_bg_y && py < logo_bg_y + logo_bg_h) {
                int local_y = py - tile_y_start;
                for (int px = logo_bg_x; px < logo_bg_x + logo_bg_w; px++) {
                    bool draw = true;
                    int r = 6;
                    int x1 = logo_bg_x;
                    int y1 = logo_bg_y;
                    int x2 = logo_bg_x + logo_bg_w - 1;
                    int y2 = logo_bg_y + logo_bg_h - 1;
                    if (px < x1 + r && py < y1 + r) {
                        int dx = px - (x1 + r); int dy = py - (y1 + r);
                        if (dx*dx + dy*dy > r*r) draw = false;
                    } else if (px > x2 - r && py < y1 + r) {
                        int dx = px - (x2 - r); int dy = py - (y1 + r);
                        if (dx*dx + dy*dy > r*r) draw = false;
                    } else if (px < x1 + r && py > y2 - r) {
                        int dx = px - (x1 + r); int dy = py - (y2 - r);
                        if (dx*dx + dy*dy > r*r) draw = false;
                    } else if (px > x2 - r && py > y2 - r) {
                        int dx = px - (x2 - r); int dy = py - (y2 - r);
                        if (dx*dx + dy*dy > r*r) draw = false;
                    }
                    if (draw) {
                        frame_buffer[local_y * LCD_H_RES + px] = COLOR_WHITE;
                    }
                }
            }
        }
        for (int ly = 0; ly < VNPT_LOGO_HEIGHT; ly++) {
            int py = logo_y + ly;
            if (py >= tile_y_start && py < tile_y_start + tile_h) {
                int local_y = py - tile_y_start;
                for (int lx = 0; lx < VNPT_LOGO_WIDTH; lx++) {
                    uint16_t pixel = vnpt_logo_center_pixels[ly * VNPT_LOGO_WIDTH + lx];
                    if (pixel != COLOR_WHITE) {
                        frame_buffer[local_y * LCD_H_RES + (logo_x + lx)] = pixel;
                    }
                }
            }
        }

        // 6. Draw Countdown Badge in bottom tile
        for (int py = tile_y_start; py < tile_y_start + tile_h; py++) {
            if (py >= badge_y && py < badge_y + badge_h) {
                int local_y = py - tile_y_start;
                for (int px = badge_x; px < badge_x + badge_w; px++) {
                    frame_buffer[local_y * LCD_H_RES + px] = COLOR_RED;
                }
            }
        }
        for (int d = 0; d < 2; d++) {
            int digit = (d == 0) ? tens : ones;
            int start_x = badge_x + ((d == 0) ? 18 : 36);
            int start_y = badge_y + 6;
            const uint8_t *glyph = font_8x12_digits[digit];
            for (int row = 0; row < 12; row++) {
                int py = start_y + row;
                if (py >= tile_y_start && py < tile_y_start + tile_h) {
                    int local_y = py - tile_y_start;
                    uint8_t line = glyph[row];
                    for (int col = 0; col < 8; col++) {
                        int px = start_x + col;
                        if (px < LCD_H_RES && (line & (0x80 >> col))) {
                            frame_buffer[local_y * LCD_H_RES + px] = COLOR_WHITE;
                        }
                    }
                }
            }
        }

        // Flush Tile to LCD synchronously
        draw_bitmap_sync(0, tile_y_start, LCD_H_RES, tile_y_start + tile_h, frame_buffer);
    }

    current_state = STATE_QR_DISPLAY;
}

// Timer Task for Countdown & Auto-Reset to Standby Background
static void timer_task(void *pvParameters) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (current_state == STATE_QR_DISPLAY) {
            // Only count down if timeout is set (> 0). If timeout == 0, stay displayed permanently!
            if (current_countdown > 0) {
                current_countdown--;
                if (current_countdown > 0) {
                    draw_countdown_badge(current_countdown);
                } else {
                    if (has_custom_bg) {
                        ESP_LOGI(TAG, "QR Timeout reached (0s). Restoring Custom Raw Background from Flash...");
                        for (int y = 0; y < LCD_V_RES; y += TILE_HEIGHT) {
                            int tile_h = TILE_HEIGHT;
                            if (y + tile_h > LCD_V_RES) tile_h = LCD_V_RES - y;
                            uint32_t flash_addr = FLASH_CUSTOM_BG_OFFSET + y * LCD_H_RES * sizeof(uint16_t);
                            size_t bytes = LCD_H_RES * tile_h * sizeof(uint16_t);
                            esp_err_t rerr = esp_flash_read(NULL, frame_buffer, flash_addr, bytes);
                            if (rerr == ESP_OK) {
                                draw_bitmap_sync(0, y, LCD_H_RES, y + tile_h, frame_buffer);
                            }
                        }
                        current_state = STATE_IDLE_CUSTOM_BG;
                    } else {
                        ESP_LOGI(TAG, "QR Timeout reached. Returning to Official VNPT Standby Background...");
                        draw_vnpt_background();
                    }
                }
            }
        } else if (current_state == STATE_CUSTOM_BG_COUNTDOWN) {
            if (custom_bg_countdown > 0) {
                custom_bg_countdown--;
                if (custom_bg_countdown <= 0) {
                    ESP_LOGI(TAG, "Custom Raw Background timeout (%ds) reached. Returning to Official VNPT Background...", raw_stream_display_timeout);
                    draw_vnpt_background();
                }
            }
        }
    }
}

// AT Command Parser
static void process_at_command(const char *cmd) {
    ESP_LOGI(TAG, "Received AT Command: [%s]", cmd);

    // Create uppercase version for case-insensitive matching
    char upper_cmd[512];
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
    const char *wifi_ptr = strstr(upper_cmd, "AT+WIFI=");
    const char *wifistatus_ptr = strstr(upper_cmd, "AT+WIFISTATUS");
    const char *ping_ptr = strstr(upper_cmd, "AT+PING");

    if (qr_ptr) {
        const char *orig_param = cmd + ((qr_ptr - upper_cmd) + 6);
        char raw[512] = {0};
        strncpy(raw, orig_param, sizeof(raw) - 1);

        size_t rlen = strlen(raw);
        while (rlen > 0 && (raw[rlen - 1] == '\r' || raw[rlen - 1] == '\n' || raw[rlen - 1] == ' ' || raw[rlen - 1] == '\t')) {
            raw[--rlen] = '\0';
        }

        char *p = raw;
        while (*p == ' ' || *p == '\t') p++;

        char payload[512] = {0};
        int timeout = default_timeout_sec;

        if (*p == '"') {
            p++;
            char *close_quote = strrchr(p, '"');
            if (close_quote) {
                size_t plen = close_quote - p;
                if (plen >= sizeof(payload)) plen = sizeof(payload) - 1;
                strncpy(payload, p, plen);
                payload[plen] = '\0';

                char *comma = strchr(close_quote + 1, ',');
                if (comma) {
                    timeout = atoi(comma + 1);
                }
            } else {
                strncpy(payload, p, sizeof(payload) - 1);
            }
        } else {
            char *comma = strrchr(p, ',');
            if (comma && comma > p && (comma[1] >= '0' && comma[1] <= '9')) {
                size_t plen = comma - p;
                if (plen >= sizeof(payload)) plen = sizeof(payload) - 1;
                strncpy(payload, p, plen);
                payload[plen] = '\0';
                timeout = atoi(comma + 1);
            } else {
                strncpy(payload, p, sizeof(payload) - 1);
            }
        }

        size_t final_len = strlen(payload);
        if (final_len > 0) {
            ESP_LOGI(TAG, "Render QR Payload: [%s], Timeout: %ds", payload, timeout);
            strncpy(current_qr_payload, payload, sizeof(current_qr_payload) - 1);
            render_qr_payment_screen(current_qr_payload, timeout);
            printf("+QR: OK\r\n");
        } else {
            ESP_LOGW(TAG, "Empty QR payload received!");
            printf("+QR: ERROR (Empty Payload)\r\n");
        }
        fflush(stdout);
    }
    else if (dtime_ptr) {
        int t = atoi(dtime_ptr + 9);
        raw_stream_display_timeout = t;
        current_state = STATE_RAW_STREAM;
        stream_buf_offset = 0;
        stream_tile_index = 0;
        stream_skip_whitespace = true;
        ESP_LOGI(TAG, "RAW_STREAM: san sang nhan 153600 bytes (Display timeout: %ds)...", raw_stream_display_timeout);
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
    else if (wifi_ptr) {
        const char *param = cmd + ((wifi_ptr - upper_cmd) + 8);
        char ssid[32] = {0};
        char pass[64] = {0};

        if (sscanf(param, "\"%31[^\"]\",\"%63[^\"]\"", ssid, pass) == 2 ||
            sscanf(param, "%31[^,],%63s", ssid, pass) == 2) {
            printf("+WIFI: CONNECTING...\r\n");
            fflush(stdout);
            if (wifi_manager_connect(ssid, pass)) {
                char ip_str[16] = {0};
                wifi_manager_get_ip_str(ip_str, sizeof(ip_str));
                wifi_mgr_state_t st = wifi_manager_get_state();
                if (st == WIFI_MGR_CONNECTED_GOT_INTERNET) {
                    printf("+WIFI: CONNECTED, IP=%s, INTERNET=OK\r\nOK\r\n", ip_str);
                } else {
                    printf("+WIFI: CONNECTED, IP=%s, INTERNET=FAIL\r\nOK\r\n", ip_str);
                }
            } else {
                printf("+WIFI: ERROR (CONNECT_FAILED)\r\n");
            }
        } else if (sscanf(param, "\"%31[^\"]\"", ssid) == 1 || sscanf(param, "%31s", ssid) == 1) {
            printf("+WIFI: CONNECTING...\r\n");
            fflush(stdout);
            if (wifi_manager_connect(ssid, "")) {
                char ip_str[16] = {0};
                wifi_manager_get_ip_str(ip_str, sizeof(ip_str));
                printf("+WIFI: CONNECTED, IP=%s\r\nOK\r\n", ip_str);
            } else {
                printf("+WIFI: ERROR (CONNECT_FAILED)\r\n");
            }
        } else {
            printf("+WIFI: ERROR (INVALID_PARAMS)\r\n");
        }
        fflush(stdout);
    }
    else if (wifistatus_ptr) {
        wifi_mgr_state_t st = wifi_manager_get_state();
        char ip_str[16] = "0.0.0.0";
        wifi_manager_get_ip_str(ip_str, sizeof(ip_str));
        const char *st_str = "DISCONNECTED";
        if (st == WIFI_MGR_CONNECTING) st_str = "CONNECTING";
        else if (st == WIFI_MGR_CONNECTED_NO_INTERNET) st_str = "CONNECTED_NO_INTERNET";
        else if (st == WIFI_MGR_CONNECTED_GOT_INTERNET) st_str = "CONNECTED_GOT_INTERNET";
        else if (st == WIFI_MGR_SOFTAP_MODE) st_str = "SOFTAP_MODE";

        printf("+WIFISTATUS: %s, IP=%s\r\nOK\r\n", st_str, ip_str);
        fflush(stdout);
    }
    else if (ping_ptr) {
        printf("+PING: CHECKING INTERNET...\r\n");
        fflush(stdout);
        if (internet_check_perform()) {
            printf("+PING: INTERNET OK (Google/Cloudflare reached)\r\nOK\r\n");
        } else {
            printf("+PING: INTERNET FAIL\r\nERROR\r\n");
        }
        fflush(stdout);
    }
    else if (strstr(upper_cmd, "AT+STATICIP=")) {
        const char *param = cmd + 12;
        char ip[16] = {0}, netmask[16] = {0}, gw[16] = {0}, dns[16] = {0};
        if (sscanf(param, "1,\"%15[^\"]\",\"%15[^\"]\",\"%15[^\"]\",\"%15[^\"]\"", ip, netmask, gw, dns) == 4 ||
            sscanf(param, "1,%15[^,],%15[^,],%15[^,],%15s", ip, netmask, gw, dns) == 4) {
            wifi_static_ip_config_t cfg = { .use_static_ip = true };
            strncpy(cfg.ip, ip, sizeof(cfg.ip) - 1);
            strncpy(cfg.netmask, netmask, sizeof(cfg.netmask) - 1);
            strncpy(cfg.gateway, gw, sizeof(cfg.gateway) - 1);
            strncpy(cfg.dns, dns, sizeof(cfg.dns) - 1);
            wifi_manager_set_static_ip_config(&cfg);
            printf("+STATICIP: ENABLED, IP=%s, GW=%s, DNS=%s\r\nOK\r\n", ip, gw, dns);
        } else {
            wifi_static_ip_config_t cfg = { .use_static_ip = false };
            wifi_manager_set_static_ip_config(&cfg);
            printf("+STATICIP: DISABLED (DHCP ENABLED)\r\nOK\r\n");
        }
        fflush(stdout);
    }
    else if (strstr(upper_cmd, "AT+WIFIRESTART") || strstr(upper_cmd, "AT+WIFIRST")) {
        printf("+WIFIRESTART: RESTARTING...\r\n");
        fflush(stdout);
        wifi_manager_restart();
        printf("OK\r\n");
        fflush(stdout);
    }
    else if (strstr(upper_cmd, "AT+WIFIRESET") || strstr(upper_cmd, "AT+APMODE")) {
        printf("+WIFIRESET: ERASING NVS & OPENING SOFTAP \"ESP32_WiFi_Setup\"...\r\n");
        fflush(stdout);
        wifi_manager_reset_to_ap_mode();
        printf("OK\r\n");
        fflush(stdout);
    }
    else if (strstr(upper_cmd, "AT+RST") || strstr(upper_cmd, "AT+RESTART")) {
        printf("+RST: REBOOTING HARDWARE...\r\nOK\r\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    }
}

// USB Serial Reader Task
static void usb_cdc_task(void *pvParameters) {
    char rx_line[512];
    size_t rx_idx = 0;
    uint8_t rx_buf[512];
    int stream_idle_counter = 0;

    fcntl(0, F_SETFL, O_NONBLOCK);

    while (1) {
        if (current_state == STATE_RAW_STREAM) {
            if (!ensure_frame_buffer()) {
                ESP_LOGE(TAG, "frame_buffer null! Huy stream.");
                current_state = STATE_IDLE_BG;
                continue;
            }

            int chunk_bytes = read(0, rx_buf, sizeof(rx_buf));
            if (chunk_bytes > 0) {
                stream_idle_counter = 0;
                uint8_t *fb_bytes = (uint8_t*)frame_buffer;

                for (int i = 0; i < chunk_bytes; i++) {
                    uint8_t b = rx_buf[i];
                    if (stream_skip_whitespace) {
                        if (b == '\n' || b == '\r') {
                            continue;
                        }
                        stream_skip_whitespace = false;
                    }
                    if (stream_buf_offset < FRAME_BUF_SIZE) {
                        // Swap bytes on-the-fly for Little-Endian RGB565 files to i80 bus:
                        if ((stream_buf_offset & 1) == 0) {
                            fb_bytes[stream_buf_offset + 1] = b;
                        } else {
                            fb_bytes[stream_buf_offset - 1] = b;
                        }
                        stream_buf_offset++;
                    }

                    if (stream_buf_offset >= FRAME_BUF_SIZE) {
                        // Received 1 tile (15360 bytes / 32 lines)
                        int y_start = stream_tile_index * TILE_HEIGHT;
                        int y_end = y_start + TILE_HEIGHT;
                        if (y_end > LCD_V_RES) y_end = LCD_V_RES;

                        draw_bitmap_sync(0, y_start, LCD_H_RES, y_end, frame_buffer);

                        // Save tile to Flash at FLASH_CUSTOM_BG_OFFSET + tile_offset
                        uint32_t flash_addr = FLASH_CUSTOM_BG_OFFSET + (stream_tile_index * FRAME_BUF_SIZE);
                        if (stream_tile_index == 0) {
                            esp_flash_erase_region(NULL, FLASH_CUSTOM_BG_OFFSET, FLASH_CUSTOM_BG_ERASE_SIZE);
                        }
                        esp_flash_write(NULL, frame_buffer, flash_addr, FRAME_BUF_SIZE);

                        stream_tile_index++;
                        stream_buf_offset = 0;

                        if (stream_tile_index >= (LCD_V_RES / TILE_HEIGHT)) {
                            ESP_LOGI(TAG, "Nhan du 153600 bytes! Render & Flash hoan tat.");
                            has_custom_bg = true;

                            if (raw_stream_display_timeout > 0) {
                                custom_bg_countdown = raw_stream_display_timeout;
                                current_state = STATE_CUSTOM_BG_COUNTDOWN;
                            } else {
                                current_state = STATE_IDLE_CUSTOM_BG;
                            }
                            printf("+DTIME: OK\r\n");
                            fflush(stdout);
                        }
                    }
                }
            } else {
                stream_idle_counter++;
                int max_idle = (stream_buf_offset == 0 && stream_tile_index == 0) ? 12000 : 1500;
                if (stream_idle_counter > max_idle) {
                    if (stream_tile_index > 0) {
                        ESP_LOGI(TAG, "Stream partial completion (%d tiles).", stream_tile_index);
                        printf("+DTIME: OK (Partial)\r\n");
                        fflush(stdout);
                    } else if (stream_buf_offset > 0) {
                        ESP_LOGW(TAG, "RAW_STREAM incomplete. Timeout reset.");
                        printf("+DTIME: ERROR (Timeout)\r\n");
                        fflush(stdout);
                    }
                    stream_buf_offset = 0;
                    stream_tile_index = 0;
                    current_state = STATE_IDLE_BG;
                    stream_idle_counter = 0;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        } else {
            uint8_t ch;
            int bytes = read(0, &ch, 1);
            if (bytes > 0) {
                if (ch == '\r' || ch == '\n') {
                    if (rx_idx > 0) {
                        rx_line[rx_idx] = '\0';
                        process_at_command(rx_line);
                        rx_idx = 0;
                    }
                } else if (ch >= 32 && ch != 127 && rx_idx < sizeof(rx_line) - 1) {
                    rx_line[rx_idx++] = ch;
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
    }
}

void app_main(void) {
    // Unbuffered I/O
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "Starting F91 VNPT Standby & Payment QR Firmware...");

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

    vTaskDelay(pdMS_TO_TICKS(100));

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

    // 3. Initialize Panel IO (CS=13, pclk=4MHz) & Register DMA Done Callback
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_i80_config_t io_config = {
        .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = 4 * 1000 * 1000,
        .trans_queue_depth = 2,
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

    lcd_trans_sem = xSemaphoreCreateBinary();
    esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = lcd_on_trans_done,
    };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io_handle, &cbs, NULL));

    // 4. Initialize ST7789 Panel
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_endian = LCD_RGB_ENDIAN_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));

    esp_lcd_panel_reset(panel_handle);
    esp_lcd_panel_init(panel_handle);
    esp_lcd_panel_invert_color(panel_handle, false);
    esp_lcd_panel_disp_on_off(panel_handle, true);

    // Turn backlight ON
    gpio_set_level(PIN_BK_LIGHT, 1);

    // Allocate frame_buffer ONCE at boot time before WiFi/other allocations
    if (!frame_buffer) {
        frame_buffer = heap_caps_malloc(FRAME_BUF_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (frame_buffer) {
            ESP_LOGI(TAG, "Successfully allocated DMA frame_buffer (%d bytes)", (int)FRAME_BUF_SIZE);
        } else {
            ESP_LOGE(TAG, "CRITICAL: Failed to allocate DMA frame_buffer!");
        }
    }

    // 5. Render default VNPT Standby Background Image
    draw_vnpt_background();

    // 6. Initialize WiFi Manager & NVS WiFi credentials
    wifi_manager_init();

    // 7. Create Tasks
    xTaskCreate(usb_cdc_task, "usb_cdc_task", 8192, NULL, 5, NULL);
    xTaskCreate(timer_task, "timer_task", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "Free Heap: %u bytes, Min Free Heap: %u bytes", (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size());
}