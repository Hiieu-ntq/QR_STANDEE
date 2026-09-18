#ifndef DNS_SERVER_H
#define DNS_SERVER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start Captive Portal DNS Server on UDP port 53.
 * Intercepts all DNS requests and resolves them to SoftAP IP (192.168.4.1).
 */
esp_err_t dns_server_start(void);

/**
 * @brief Stop Captive Portal DNS Server.
 */
void dns_server_stop(void);

#ifdef __cplusplus
}
#endif

#endif // DNS_SERVER_H
