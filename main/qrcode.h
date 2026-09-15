#ifndef QRCODE_H
#define QRCODE_H

#include <stdint.h>
#include <stdbool.h>

#define ECC_LOW 0
#define ECC_MEDIUM 1
#define ECC_QUARTILE 2
#define ECC_HIGH 3

typedef struct {
    uint8_t version;
    uint8_t size;
    uint8_t ecc;
    uint8_t mode;
    uint8_t *modules;
} QRCode;

uint16_t qrcode_getBufferSize(uint8_t version);
int8_t qrcode_initText(QRCode *qrcode, uint8_t *modules, uint8_t version, uint8_t ecc, const char *data);
int8_t qrcode_initBytes(QRCode *qrcode, uint8_t *modules, uint8_t version, uint8_t ecc, uint8_t *data, uint16_t length);
bool qrcode_getModule(QRCode *qrcode, uint8_t x, uint8_t y);

#endif // QRCODE_H
