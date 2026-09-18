#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start Web Server for VNPT WiFi Config & Captive Portal
 */
esp_err_t web_server_start(void);

/**
 * @brief Stop Web Server
 */
void web_server_stop(void);

#ifdef __cplusplus
}
#endif

#endif // WEB_SERVER_H
