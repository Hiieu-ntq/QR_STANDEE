#include "mqtt_manager.h"
#include "json_parser.h"
#include "wifi_manager.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "esp_crt_bundle.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "MQTT_MANAGER";

extern void process_at_command(const char *cmd);
extern void draw_wifi_status_overlay(const char *text, uint16_t bg_color, uint16_t text_color);
extern void hide_wifi_status_overlay(void);

extern void render_qr_payment_screen(const char *qr_data, int timeout_sec);

static esp_mqtt_client_handle_t s_client = NULL;
static bool s_is_connected = false;
static bool s_is_started = false;

static mqtt_config_t s_mqtt_config = {
    .broker_host = "4a1375dbae154624a6759636e2559329.s1.eu.hivemq.cloud",
    .port = 8883,
    .username = "esp32s3_demomqtts",
    .password = "12345678",
    .sub_topic = "vnpt/f91/cmd",
    .pub_topic = "vnpt/f91/status",
    .use_tls = true
};

bool mqtt_manager_has_saved_config(void) {
    nvs_handle_t handle;
    char host[128] = {0};
    size_t len = sizeof(host);
    if (nvs_open("mqtt_store", NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_str(handle, "host", host, &len);
        nvs_close(handle);
    }
    return (strlen(host) > 0);
}

static void load_mqtt_config_from_nvs(void) {
    nvs_handle_t handle;
    if (nvs_open("mqtt_store", NVS_READONLY, &handle) == ESP_OK) {
        size_t len = sizeof(s_mqtt_config.broker_host);
        nvs_get_str(handle, "host", s_mqtt_config.broker_host, &len);

        int32_t p = s_mqtt_config.port;
        if (nvs_get_i32(handle, "port", &p) == ESP_OK) {
            s_mqtt_config.port = p;
        }

        len = sizeof(s_mqtt_config.username);
        nvs_get_str(handle, "user", s_mqtt_config.username, &len);

        len = sizeof(s_mqtt_config.password);
        nvs_get_str(handle, "pass", s_mqtt_config.password, &len);

        len = sizeof(s_mqtt_config.sub_topic);
        nvs_get_str(handle, "sub_topic", s_mqtt_config.sub_topic, &len);

        len = sizeof(s_mqtt_config.pub_topic);
        nvs_get_str(handle, "pub_topic", s_mqtt_config.pub_topic, &len);

        uint8_t tls = 1;
        if (nvs_get_u8(handle, "use_tls", &tls) == ESP_OK) {
            s_mqtt_config.use_tls = (tls != 0);
        }

        nvs_close(handle);
        ESP_LOGI(TAG, "MQTT Config loaded from NVS: Host=%s, Port=%d, TLS=%d, SubTopic=%s",
                 s_mqtt_config.broker_host, s_mqtt_config.port, s_mqtt_config.use_tls, s_mqtt_config.sub_topic);
    } else {
        ESP_LOGI(TAG, "No MQTT Config in NVS -> Using default HiveMQ Cloud settings (%s:%d)",
                 s_mqtt_config.broker_host, s_mqtt_config.port);
    }
}

static void save_mqtt_config_to_nvs(void) {
    nvs_handle_t handle;
    if (nvs_open("mqtt_store", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_str(handle, "host", s_mqtt_config.broker_host);
        nvs_set_i32(handle, "port", s_mqtt_config.port);
        nvs_set_str(handle, "user", s_mqtt_config.username);
        nvs_set_str(handle, "pass", s_mqtt_config.password);
        nvs_set_str(handle, "sub_topic", s_mqtt_config.sub_topic);
        nvs_set_str(handle, "pub_topic", s_mqtt_config.pub_topic);
        nvs_set_u8(handle, "use_tls", s_mqtt_config.use_tls ? 1 : 0);
        nvs_commit(handle);
        nvs_close(handle);
        ESP_LOGI(TAG, "MQTT Config saved to NVS.");
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            s_is_connected = true;
            ESP_LOGI(TAG, ">>> MQTT CONNECTED TO BROKER (%s:%d) <<<", s_mqtt_config.broker_host, s_mqtt_config.port);

            // Display MQTT CONNECTED notification on LCD overlay screen
            draw_wifi_status_overlay("MQTT CONNECTED", 0x07E0, 0xFFFF);

            if (strlen(s_mqtt_config.sub_topic) > 0) {
                int msg_id = esp_mqtt_client_subscribe(s_client, s_mqtt_config.sub_topic, 1);
                ESP_LOGI(TAG, "Subscribed to topic [%s], msg_id=%d", s_mqtt_config.sub_topic, msg_id);
            }

            char status_json[300];
            json_parser_create_status(status_json, sizeof(status_json));
            mqtt_manager_publish(s_mqtt_config.pub_topic, status_json);

            // Check if both WiFi and MQTT credentials are saved, and close Captive Portal
            wifi_manager_check_and_close_captive_portal();
            break;

        case MQTT_EVENT_DISCONNECTED:
            s_is_connected = false;
            ESP_LOGW(TAG, "MQTT DISCONNECTED from broker");
            break;

        case MQTT_EVENT_DATA: {
            ESP_LOGI(TAG, "MQTT_EVENT_DATA received from topic %.*s (len=%d)",
                     event->topic_len, event->topic, event->data_len);

            char data_buf[1024] = {0};
            size_t copy_len = (event->data_len < sizeof(data_buf) - 1) ? event->data_len : (sizeof(data_buf) - 1);
            memcpy(data_buf, event->data, copy_len);
            data_buf[copy_len] = '\0';

            char *p = data_buf;
            while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;

            if (*p == '{' || *p == '[') {
                ESP_LOGI(TAG, "Parsing incoming MQTT JSON payload: %s", p);
                if (!json_parser_process(p)) {
                    ESP_LOGW(TAG, "JSON parse unhandled, fallback rendering payload as QR code: %s", p);
                    render_qr_payment_screen(p, 30);
                }
            } else if (strncasecmp(p, "AT+", 3) == 0) {
                ESP_LOGI(TAG, "Executing incoming MQTT AT Command: %s", p);
                process_at_command(p);
            } else if (strlen(p) > 0) {
                ESP_LOGI(TAG, "Rendering raw string payload as QR code: %s", p);
                render_qr_payment_screen(p, 30);
            }
            break;
        }

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT_EVENT_ERROR occurred");
            break;

        default:
            break;
    }
}

void mqtt_manager_init(void) {
    load_mqtt_config_from_nvs();
}

void mqtt_manager_start(void) {
    if (s_is_started) {
        ESP_LOGI(TAG, "MQTT client already started.");
        return;
    }

    if (strlen(s_mqtt_config.broker_host) == 0) {
        ESP_LOGW(TAG, "No broker host configured, skipping MQTT start.");
        return;
    }

    char uri_buf[256];
    const char *scheme = s_mqtt_config.use_tls ? "mqtts" : "mqtt";
    snprintf(uri_buf, sizeof(uri_buf), "%s://%s:%d", scheme, s_mqtt_config.broker_host, s_mqtt_config.port);

    ESP_LOGI(TAG, "Starting MQTT client targeting URI: %s", uri_buf);

    esp_mqtt_client_config_t mqtt_cfg;
    memset(&mqtt_cfg, 0, sizeof(mqtt_cfg));

    mqtt_cfg.broker.address.uri = uri_buf;
    if (s_mqtt_config.use_tls) {
        mqtt_cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
        mqtt_cfg.broker.verification.skip_cert_common_name_check = false;
        ESP_LOGI(TAG, "Configuring TLS with Certificate Bundle (esp_crt_bundle_attach=%p, SNI enabled)", esp_crt_bundle_attach);
    } else {
        ESP_LOGI(TAG, "Configuring plain MQTT (non-TLS)");
    }

    mqtt_cfg.credentials.client_id = "f91_standee_device";
    if (strlen(s_mqtt_config.username) > 0) {
        mqtt_cfg.credentials.username = s_mqtt_config.username;
    }
    if (strlen(s_mqtt_config.password) > 0) {
        mqtt_cfg.credentials.authentication.password = s_mqtt_config.password;
    }
    mqtt_cfg.task.stack_size = 8192;

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "Failed to initialize esp_mqtt_client!");
        return;
    }

    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_err_t err = esp_mqtt_client_start(s_client);
    if (err == ESP_OK) {
        s_is_started = true;
        ESP_LOGI(TAG, "esp_mqtt_client_start() succeeded.");
    } else {
        ESP_LOGE(TAG, "esp_mqtt_client_start() failed: %s", esp_err_to_name(err));
    }
}

void mqtt_manager_stop(void) {
    if (!s_is_started || !s_client) return;
    ESP_LOGI(TAG, "Stopping MQTT client...");
    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client = NULL;
    s_is_started = false;
    s_is_connected = false;
}

bool mqtt_manager_publish(const char *topic, const char *data) {
    if (!s_is_connected || !s_client || !data) {
        ESP_LOGW(TAG, "Cannot publish: MQTT client not connected.");
        return false;
    }
    const char *target_topic = (topic && strlen(topic) > 0) ? topic : s_mqtt_config.pub_topic;
    int msg_id = esp_mqtt_client_publish(s_client, target_topic, data, 0, 1, 0);
    ESP_LOGI(TAG, "Published msg (id=%d) to topic [%s]: %s", msg_id, target_topic, data);
    return (msg_id >= 0);
}

void mqtt_manager_get_config(mqtt_config_t *cfg) {
    if (cfg) {
        memcpy(cfg, &s_mqtt_config, sizeof(mqtt_config_t));
    }
}

void mqtt_manager_set_config(const mqtt_config_t *cfg) {
    if (!cfg) return;
    memcpy(&s_mqtt_config, cfg, sizeof(mqtt_config_t));
    save_mqtt_config_to_nvs();

    if (s_is_started) {
        mqtt_manager_stop();
        mqtt_manager_start();
    }
}

bool mqtt_manager_is_connected(void) {
    return s_is_connected;
}
