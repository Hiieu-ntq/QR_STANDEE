#include "json_parser.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_system.h"
#include "wifi_manager.h"
#include <string.h>
#include <stdio.h>
#include <strings.h>

static const char *TAG = "JSON_PARSER";

// External display handler implemented in main.c
extern void process_at_command(const char *cmd);

extern void render_qr_payment_screen(const char *qr_data, int timeout_sec);

bool json_parser_process(const char *json_str) {
    if (!json_str || strlen(json_str) == 0) {
        ESP_LOGE(TAG, "Empty JSON input");
        return false;
    }

    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        ESP_LOGE(TAG, "cJSON_Parse failed for input: %s", json_str);
        return false;
    }

    // Case-insensitive search for command key
    cJSON *cmd_item = cJSON_GetObjectItem(root, "cmd");
    if (!cmd_item || !cJSON_IsString(cmd_item)) {
        cmd_item = cJSON_GetObjectItem(root, "action");
    }
    if (!cmd_item || !cJSON_IsString(cmd_item)) {
        cmd_item = cJSON_GetObjectItem(root, "command");
    }
    if (!cmd_item || !cJSON_IsString(cmd_item)) {
        cmd_item = cJSON_GetObjectItem(root, "type");
    }

    cJSON *payload_item = cJSON_GetObjectItem(root, "payload");
    if (!payload_item || !cJSON_IsString(payload_item)) {
        payload_item = cJSON_GetObjectItem(root, "qr");
    }
    if (!payload_item || !cJSON_IsString(payload_item)) {
        payload_item = cJSON_GetObjectItem(root, "qr_data");
    }
    if (!payload_item || !cJSON_IsString(payload_item)) {
        payload_item = cJSON_GetObjectItem(root, "data");
    }
    if (!payload_item || !cJSON_IsString(payload_item)) {
        payload_item = cJSON_GetObjectItem(root, "url");
    }
    if (!payload_item || !cJSON_IsString(payload_item)) {
        payload_item = cJSON_GetObjectItem(root, "code");
    }

    cJSON *timeout_item = cJSON_GetObjectItem(root, "timeout");
    if (!timeout_item) {
        timeout_item = cJSON_GetObjectItem(root, "dtime");
    }

    int timeout = 30;
    if (timeout_item && cJSON_IsNumber(timeout_item)) {
        timeout = timeout_item->valueint;
    }

    bool success = false;

    if (cmd_item && cJSON_IsString(cmd_item) && cmd_item->valuestring) {
        const char *cmd = cmd_item->valuestring;
        ESP_LOGI(TAG, "Processing JSON command: [%s]", cmd);

        if (strcasecmp(cmd, "qr") == 0 || strcasecmp(cmd, "DISPLAY_QR") == 0 || strcasecmp(cmd, "show_qr") == 0) {
            if (payload_item && payload_item->valuestring && strlen(payload_item->valuestring) > 0) {
                ESP_LOGI(TAG, "Directly rendering QR Code from JSON payload (timeout=%ds)...", timeout);
                render_qr_payment_screen(payload_item->valuestring, timeout);
                success = true;
            } else {
                ESP_LOGW(TAG, "QR JSON command missing payload string");
            }
        }
        else if (strcasecmp(cmd, "clear") == 0 || strcasecmp(cmd, "bg") == 0) {
            process_at_command("AT+BG");
            success = true;
        }
        else if (strcasecmp(cmd, "reboot") == 0 || strcasecmp(cmd, "rst") == 0) {
            process_at_command("AT+RST");
            success = true;
        }
        else if (strcasecmp(cmd, "status") == 0 || strcasecmp(cmd, "ping") == 0) {
            process_at_command("AT+PING");
            success = true;
        }
    }

    // Direct payload check if no explicit 'cmd' string found (e.g. {"qr": "https://..."} or {"url": "https://..."})
    if (!success && payload_item && payload_item->valuestring && strlen(payload_item->valuestring) > 0) {
        ESP_LOGI(TAG, "Found direct QR payload in JSON: [%s], rendering (timeout=%ds)...", payload_item->valuestring, timeout);
        render_qr_payment_screen(payload_item->valuestring, timeout);
        success = true;
    }

    cJSON_Delete(root);
    return success;
}

void json_parser_create_status(char *buf, size_t buf_len) {
    if (!buf || buf_len == 0) return;

    char ip_str[16] = "0.0.0.0";
    wifi_manager_get_ip_str(ip_str, sizeof(ip_str));

    wifi_mgr_state_t st = wifi_manager_get_state();
    const char *st_str = "DISCONNECTED";
    if (st == WIFI_MGR_CONNECTING) st_str = "CONNECTING";
    else if (st == WIFI_MGR_CONNECTED_NO_INTERNET) st_str = "CONNECTED_NO_INTERNET";
    else if (st == WIFI_MGR_CONNECTED_GOT_INTERNET) st_str = "CONNECTED_GOT_INTERNET";
    else if (st == WIFI_MGR_SOFTAP_MODE) st_str = "SOFTAP_MODE";

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device", "F91_VNPT_STANDEE");
    cJSON_AddStringToObject(root, "firmware", "v1.0");
    cJSON_AddStringToObject(root, "wifi_status", st_str);
    cJSON_AddStringToObject(root, "ip", ip_str);
    cJSON_AddNumberToObject(root, "free_heap", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "min_heap", (double)esp_get_minimum_free_heap_size());

    char *json_out = cJSON_PrintUnformatted(root);
    if (json_out) {
        snprintf(buf, buf_len, "%s", json_out);
        cJSON_free(json_out);
    } else {
        snprintf(buf, buf_len, "{}");
    }
    cJSON_Delete(root);
}
