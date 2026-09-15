#include "qrcode.h"
#include <stdlib.h>
#include <string.h>

// Version info tables for sizes 1 to 10
static const uint16_t NUM_ERROR_CORRECTION_CODEWORDS[10][4] = {
    { 7, 10, 13, 17 }, { 10, 16, 22, 28 }, { 15, 26, 36, 44 }, { 20, 36, 52, 64 },
    { 26, 48, 72, 88 }, { 36, 64, 96, 112 }, { 40, 72, 108, 130 }, { 48, 88, 132, 156 },
    { 60, 110, 160, 192 }, { 72, 130, 192, 224 }
};

static const uint16_t NUM_RAW_DATA_MODULES[10] = {
    208, 359, 567, 807, 1079, 1383, 1568, 1936, 2336, 2768
};

static const uint8_t ALIGNMENT_PATTERN_POSITIONS[10][7] = {
    { 0 }, { 6, 18 }, { 6, 22 }, { 6, 26 }, { 6, 30 }, { 6, 34 },
    { 6, 22, 38 }, { 6, 24, 42 }, { 6, 26, 46 }, { 6, 28, 50 }
};

// Galios Field 256 math tables
static uint8_t gf_exp[256];
static uint8_t gf_log[256];

static void gf_init(void) {
    static bool initialized = false;
    if (initialized) return;
    uint8_t x = 1;
    for (int i = 0; i < 255; i++) {
        gf_exp[i] = x;
        gf_log[x] = i;
        x = (x << 1) ^ ((x & 0x80) ? 0x11D : 0);
    }
    gf_exp[255] = gf_exp[0];
    initialized = true;
}

static uint8_t gf_mul(uint8_t x, uint8_t y) {
    if (x == 0 || y == 0) return 0;
    return gf_exp[(gf_log[x] + gf_log[y]) % 255];
}

static void rs_generator_poly(uint8_t degree, uint8_t *poly) {
    poly[0] = 1;
    for (int i = 1; i <= degree; i++) poly[i] = 0;
    for (int i = 0; i < degree; i++) {
        uint8_t root = gf_exp[i];
        for (int j = degree; j > 0; j--) {
            poly[j] = poly[j - 1] ^ gf_mul(poly[j], root);
        }
        poly[0] = gf_mul(poly[0], root);
    }
}

static void rs_encode(const uint8_t *data, uint16_t len, uint8_t *ecc, uint8_t ecc_len) {
    gf_init();
    uint8_t gen[256];
    rs_generator_poly(ecc_len, gen);
    memset(ecc, 0, ecc_len);
    for (int i = 0; i < len; i++) {
        uint8_t factor = data[i] ^ ecc[0];
        memmove(ecc, ecc + 1, ecc_len - 1);
        ecc[ecc_len - 1] = 0;
        for (int j = 0; j < ecc_len; j++) {
            ecc[j] ^= gf_mul(gen[ecc_len - 1 - j], factor);
        }
    }
}

uint16_t qrcode_getBufferSize(uint8_t version) {
    uint8_t size = 17 + version * 4;
    return (size * size + 7) / 8;
}

static void set_module(QRCode *qrcode, uint8_t x, uint8_t y, bool value) {
    uint16_t index = y * qrcode->size + x;
    if (value) {
        qrcode->modules[index / 8] |= (1 << (index % 8));
    } else {
        qrcode->modules[index / 8] &= ~(1 << (index % 8));
    }
}

bool qrcode_getModule(QRCode *qrcode, uint8_t x, uint8_t y) {
    if (x >= qrcode->size || y >= qrcode->size) return false;
    uint16_t index = y * qrcode->size + x;
    return (qrcode->modules[index / 8] >> (index % 8)) & 1;
}

static void draw_finder(QRCode *q, uint8_t x, uint8_t y) {
    for (int dy = -1; dy <= 7; dy++) {
        for (int dx = -1; dx <= 7; dx++) {
            int px = x + dx;
            int py = y + dy;
            if (px < 0 || px >= q->size || py < 0 || py >= q->size) continue;
            bool dark = (dx >= 0 && dx <= 6 && (dy == 0 || dy == 6)) ||
                        (dy >= 0 && dy <= 6 && (dx == 0 || dx == 6)) ||
                        (dx >= 2 && dx <= 4 && dy >= 2 && dy <= 4);
            set_module(q, px, py, dark);
        }
    }
}

static void draw_alignment(QRCode *q, uint8_t x, uint8_t y) {
    for (int dy = -2; dy <= 2; dy++) {
        for (int dx = -2; dx <= 2; dx++) {
            bool dark = (abs(dx) == 2 || abs(dy) == 2 || (dx == 0 && dy == 0));
            set_module(q, x + dx, y + dy, dark);
        }
    }
}

static void draw_format_info(QRCode *q, uint8_t ecc) {
    // Format Info bits for Mask 0 ((x+y)%2==0) for ECC 0..3
    static const uint16_t FORMAT_INFO[4] = { 0x77C4, 0x5412, 0x355F, 0x1689 };
    uint16_t format = FORMAT_INFO[ecc & 3];

    int format_bits_x[15] = { 8, 8, 8, 8, 8, 8, 8, 8, 7, 5, 4, 3, 2, 1, 0 };
    int format_bits_y[15] = { 0, 1, 2, 3, 4, 5, 7, 8, 8, 8, 8, 8, 8, 8, 8 };

    for (int i = 0; i < 15; i++) {
        bool dark = (format >> (14 - i)) & 1;
        set_module(q, format_bits_x[i], format_bits_y[i], dark);
    }

    for (int i = 0; i < 7; i++) {
        bool dark = (format >> (14 - i)) & 1;
        set_module(q, 8, q->size - 1 - i, dark);
    }
    for (int i = 0; i < 8; i++) {
        bool dark = (format >> i) & 1;
        set_module(q, q->size - 1 - i, 8, dark);
    }
}

int8_t qrcode_initBytes(QRCode *qrcode, uint8_t *modules, uint8_t version, uint8_t ecc, uint8_t *data, uint16_t length) {
    if (version < 1 || version > 10) return -1;
    qrcode->version = version;
    qrcode->size = 17 + version * 4;
    qrcode->ecc = ecc;
    qrcode->modules = modules;
    memset(modules, 0, qrcode_getBufferSize(version));

    // Draw Finders
    draw_finder(qrcode, 0, 0);
    draw_finder(qrcode, qrcode->size - 7, 0);
    draw_finder(qrcode, 0, qrcode->size - 7);

    // Timing patterns
    for (int i = 8; i < qrcode->size - 8; i++) {
        set_module(qrcode, i, 6, i % 2 == 0);
        set_module(qrcode, 6, i, i % 2 == 0);
    }

    // Alignments
    if (version >= 2) {
        int pos_count = 0;
        while (ALIGNMENT_PATTERN_POSITIONS[version - 1][pos_count] != 0) pos_count++;
        for (int i = 0; i < pos_count; i++) {
            for (int j = 0; j < pos_count; j++) {
                int ax = ALIGNMENT_PATTERN_POSITIONS[version - 1][i];
                int ay = ALIGNMENT_PATTERN_POSITIONS[version - 1][j];
                if ((ax == 6 && ay == 6) || (ax == 6 && ay == qrcode->size - 7) || (ax == qrcode->size - 7 && ay == 6)) continue;
                draw_alignment(qrcode, ax, ay);
            }
        }
    }

    // Dark module
    set_module(qrcode, 8, 4 * version + 9, true);

    // Format Info
    draw_format_info(qrcode, ecc);

    // Data packing + ECC
    uint16_t num_raw = NUM_RAW_DATA_MODULES[version - 1] / 8;
    uint16_t ecc_bytes = NUM_ERROR_CORRECTION_CODEWORDS[version - 1][ecc];
    uint16_t data_bytes = num_raw - ecc_bytes;

    uint8_t buffer[300];
    memset(buffer, 0, sizeof(buffer));

    // Byte mode indicator 0100 + 8-bit count
    buffer[0] = 0x40 | (length >> 4);
    buffer[1] = (length << 4) & 0xF0;

    uint16_t bit_idx = 12;
    for (int i = 0; i < length && i < data_bytes - 2; i++) {
        for (int b = 7; b >= 0; b--) {
            if ((data[i] >> b) & 1) {
                buffer[bit_idx / 8] |= (1 << (7 - (bit_idx % 8)));
            }
            bit_idx++;
        }
    }

    // ECC calculation
    uint8_t ecc_buf[256];
    rs_encode(buffer, data_bytes, ecc_buf, ecc_bytes);

    memcpy(buffer + data_bytes, ecc_buf, ecc_bytes);

    // Interleave/Place modules in QR matrix
    int byte_pos = 0;
    int bit_pos = 7;
    int x = qrcode->size - 1;
    int dir = -1;

    while (x > 0) {
        if (x == 6) x--;
        for (int y_idx = 0; y_idx < qrcode->size; y_idx++) {
            int y = (dir == -1) ? (qrcode->size - 1 - y_idx) : y_idx;
            for (int col = 0; col < 2; col++) {
                int px = x - col;
                // Check if already reserved module
                bool reserved = false;
                if ((px < 9 && (y < 9 || y >= qrcode->size - 8)) || (px >= qrcode->size - 8 && y < 9)) reserved = true;
                if (px == 6 || y == 6) reserved = true;
                if (!reserved) {
                    bool val = false;
                    if (byte_pos < num_raw) {
                        val = (buffer[byte_pos] >> bit_pos) & 1;
                        bit_pos--;
                        if (bit_pos < 0) {
                            bit_pos = 7;
                            byte_pos++;
                        }
                    }
                    val ^= ((px + y) % 2 == 0); // Apply Mask Pattern 0
                    set_module(qrcode, px, y, val);
                }
            }
        }
        dir = -dir;
        x -= 2;
    }

    return 0;
}

int8_t qrcode_initText(QRCode *qrcode, uint8_t *modules, uint8_t version, uint8_t ecc, const char *data) {
    return qrcode_initBytes(qrcode, modules, version, ecc, (uint8_t *)data, strlen(data));
}
