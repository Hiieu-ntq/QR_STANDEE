#ifndef MQTT_MANAGER_H
#define MQTT_MANAGER_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char broker_host[128];
    int port;
    char username[64];
    char password[64];
    char sub_topic[128];
    char pub_topic[128];
    bool use_tls;
} mqtt_config_t;

/**
 * @brief Initialize MQTT manager and load config from NVS
 */
void mqtt_manager_init(void);

/**
 * @brief Start MQTT client task / connection
 */
void mqtt_manager_start(void);

/**
 * @brief Stop MQTT client
 */
void mqtt_manager_stop(void);

/**
 * @brief Publish data string to specified topic (or default pub_topic if topic is NULL)
 */
bool mqtt_manager_publish(const char *topic, const char *data);

/**
 * @brief Get current MQTT configuration
 */
void mqtt_manager_get_config(mqtt_config_t *cfg);

/**
 * @brief Save new MQTT configuration to NVS and restart MQTT if active
 */
void mqtt_manager_set_config(const mqtt_config_t *cfg);

/**
 * @brief Check if MQTT is currently connected to broker
 */
bool mqtt_manager_is_connected(void);

/**
 * @brief Check if MQTT configuration has been saved in NVS
 */
bool mqtt_manager_has_saved_config(void);

#endif // MQTT_MANAGER_H
