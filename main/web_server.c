#include "web_server.h"
#include "wifi_manager.h"
#include "mqtt_manager.h"
#include <string.h>
#include <stdlib.h>
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "WEB_SERVER";
static httpd_handle_t s_server = NULL;

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

#define SAVE_REQ_MAX_LEN 1024

static esp_err_t root_get_handler(httpd_req_t *req) {
    size_t html_len = index_html_end - index_html_start;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, (const char *)index_html_start, html_len);
}

static esp_err_t scan_get_handler(httpd_req_t *req) {
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_APSTA) {
        esp_wifi_disconnect();
    }

    wifi_scan_config_t scan_config = {
        .show_hidden = false,
    };
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_start failed: %d (%s)", err, esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"scan_failed\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > 20) ap_count = 20;

    cJSON *root = cJSON_CreateArray();
    if (ap_count > 0) {
        wifi_ap_record_t *ap_records = calloc(ap_count, sizeof(wifi_ap_record_t));
        if (ap_records != NULL) {
            esp_wifi_scan_get_ap_records(&ap_count, ap_records);
            for (int i = 0; i < ap_count; i++) {
                if (strlen((const char *)ap_records[i].ssid) == 0) continue;
                cJSON *item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "ssid", (const char *)ap_records[i].ssid);
                cJSON_AddNumberToObject(item, "rssi", ap_records[i].rssi);
                cJSON_AddBoolToObject(item, "secure", ap_records[i].authmode != WIFI_AUTH_OPEN);
                cJSON_AddItemToArray(root, item);
            }
            free(ap_records);
        }
    }

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t status_get_handler(httpd_req_t *req) {
    char ip[16] = "0.0.0.0";
    wifi_manager_get_ip_str(ip, sizeof(ip));
    wifi_mgr_state_t st = wifi_manager_get_state();
    bool connected = (st == WIFI_MGR_CONNECTED_GOT_INTERNET || st == WIFI_MGR_CONNECTED_NO_INTERNET);

    wifi_static_ip_config_t static_cfg;
    wifi_manager_get_static_ip_config(&static_cfg);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "connected", connected);
    cJSON_AddStringToObject(root, "ip", ip);
    cJSON_AddStringToObject(root, "ap_ssid", "ESP32_WiFi_Setup");

    cJSON *st_obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(st_obj, "use_static", static_cfg.use_static_ip);
    cJSON_AddStringToObject(st_obj, "ip", static_cfg.ip);
    cJSON_AddStringToObject(st_obj, "netmask", static_cfg.netmask);
    cJSON_AddStringToObject(st_obj, "gateway", static_cfg.gateway);
    cJSON_AddStringToObject(st_obj, "dns", static_cfg.dns);
    cJSON_AddItemToObject(root, "static_cfg", st_obj);

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

typedef struct {
    char ssid[32];
    char password[64];
} pending_connect_t;

static void delayed_connect_task(void *arg) {
    pending_connect_t *info = (pending_connect_t *)arg;
    vTaskDelay(pdMS_TO_TICKS(200));
    wifi_manager_connect(info->ssid, info->password);
    free(info);
    vTaskDelete(NULL);
}

static esp_err_t save_post_handler(httpd_req_t *req) {
    if (req->content_len == 0 || req->content_len >= SAVE_REQ_MAX_LEN) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid data");
        return ESP_FAIL;
    }

    char buf[SAVE_REQ_MAX_LEN];
    size_t received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, buf + received, req->content_len - received);
        if (ret <= 0) return ESP_FAIL;
        received += ret;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *ssid_item = cJSON_GetObjectItem(root, "ssid");
    cJSON *pass_item = cJSON_GetObjectItem(root, "password");

    if (!cJSON_IsString(ssid_item) || strlen(ssid_item->valuestring) == 0) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ssid");
        return ESP_FAIL;
    }

    const char *ssid = ssid_item->valuestring;
    const char *password = cJSON_IsString(pass_item) ? pass_item->valuestring : "";

    // Parse Static IP options
    cJSON *use_static_item = cJSON_GetObjectItem(root, "use_static_ip");
    cJSON *static_ip_item = cJSON_GetObjectItem(root, "static_ip");
    cJSON *netmask_item = cJSON_GetObjectItem(root, "netmask");
    cJSON *gw_item = cJSON_GetObjectItem(root, "gateway");
    cJSON *dns_item = cJSON_GetObjectItem(root, "dns");

    wifi_static_ip_config_t cfg = {0};
    if (cJSON_IsBool(use_static_item)) {
        cfg.use_static_ip = cJSON_IsTrue(use_static_item);
    }
    if (cJSON_IsString(static_ip_item)) strncpy(cfg.ip, static_ip_item->valuestring, sizeof(cfg.ip) - 1);
    if (cJSON_IsString(netmask_item)) strncpy(cfg.netmask, netmask_item->valuestring, sizeof(cfg.netmask) - 1);
    if (cJSON_IsString(gw_item)) strncpy(cfg.gateway, gw_item->valuestring, sizeof(cfg.gateway) - 1);
    if (cJSON_IsString(dns_item)) strncpy(cfg.dns, dns_item->valuestring, sizeof(cfg.dns) - 1);

    wifi_manager_set_static_ip_config(&cfg);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":true,\"message\":\"Credentials & IP settings received. Connecting...\"}", HTTPD_RESP_USE_STRLEN);

    pending_connect_t *info = malloc(sizeof(pending_connect_t));
    if (info) {
        strncpy(info->ssid, ssid, sizeof(info->ssid) - 1);
        strncpy(info->password, password, sizeof(info->password) - 1);
        xTaskCreate(delayed_connect_task, "delayed_conn", 4096, info, 5, NULL);
    }

    cJSON_Delete(root);
    return ESP_OK;
}

// Captive Portal probe redirects for Android / iOS / Windows
static esp_err_t captive_redirect_handler(httpd_req_t *req) {
    wifi_mgr_state_t state = wifi_manager_get_state();
    if (state == WIFI_MGR_CONNECTED_GOT_INTERNET || state == WIFI_MGR_CONNECTED_NO_INTERNET) {
        if (strstr(req->uri, "generate_204")) {
            httpd_resp_set_status(req, "204 No Content");
            httpd_resp_send(req, NULL, 0);
            return ESP_OK;
        }
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t http_404_redirect_handler(httpd_req_t *req, httpd_err_code_t err) {
    wifi_mgr_state_t state = wifi_manager_get_state();
    if (state == WIFI_MGR_CONNECTED_GOT_INTERNET || state == WIFI_MGR_CONNECTED_NO_INTERNET) {
        if (strstr(req->uri, "generate_204")) {
            httpd_resp_set_status(req, "204 No Content");
            httpd_resp_send(req, NULL, 0);
            return ESP_OK;
        }
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t mqtt_get_handler(httpd_req_t *req) {
    mqtt_config_t cfg;
    mqtt_manager_get_config(&cfg);
    bool conn = mqtt_manager_is_connected();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "connected", conn);
    cJSON_AddStringToObject(root, "host", cfg.broker_host);
    cJSON_AddNumberToObject(root, "port", cfg.port);
    cJSON_AddStringToObject(root, "user", cfg.username);
    cJSON_AddStringToObject(root, "sub_topic", cfg.sub_topic);
    cJSON_AddStringToObject(root, "pub_topic", cfg.pub_topic);
    cJSON_AddBoolToObject(root, "use_tls", cfg.use_tls);

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t mqtt_post_handler(httpd_req_t *req) {
    if (req->content_len == 0 || req->content_len >= SAVE_REQ_MAX_LEN) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid data");
        return ESP_FAIL;
    }

    char buf[SAVE_REQ_MAX_LEN];
    size_t received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, buf + received, req->content_len - received);
        if (ret <= 0) return ESP_FAIL;
        received += ret;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    mqtt_config_t cfg;
    mqtt_manager_get_config(&cfg);

    cJSON *host_item = cJSON_GetObjectItem(root, "host");
    cJSON *port_item = cJSON_GetObjectItem(root, "port");
    cJSON *user_item = cJSON_GetObjectItem(root, "user");
    cJSON *pass_item = cJSON_GetObjectItem(root, "pass");
    cJSON *sub_item = cJSON_GetObjectItem(root, "sub_topic");
    cJSON *pub_item = cJSON_GetObjectItem(root, "pub_topic");
    cJSON *tls_item = cJSON_GetObjectItem(root, "use_tls");

    if (cJSON_IsString(host_item) && strlen(host_item->valuestring) > 0) {
        strncpy(cfg.broker_host, host_item->valuestring, sizeof(cfg.broker_host) - 1);
    }
    if (cJSON_IsNumber(port_item)) cfg.port = port_item->valueint;
    if (cJSON_IsString(user_item)) strncpy(cfg.username, user_item->valuestring, sizeof(cfg.username) - 1);
    if (cJSON_IsString(pass_item)) strncpy(cfg.password, pass_item->valuestring, sizeof(cfg.password) - 1);
    if (cJSON_IsString(sub_item)) strncpy(cfg.sub_topic, sub_item->valuestring, sizeof(cfg.sub_topic) - 1);
    if (cJSON_IsString(pub_item)) strncpy(cfg.pub_topic, pub_item->valuestring, sizeof(cfg.pub_topic) - 1);
    if (cJSON_IsBool(tls_item)) cfg.use_tls = cJSON_IsTrue(tls_item);

    mqtt_manager_set_config(&cfg);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":true,\"message\":\"MQTT configuration updated\"}", HTTPD_RESP_USE_STRLEN);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t web_server_start(void) {
    if (s_server != NULL) return ESP_OK;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 16;
    config.stack_size = 8192;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) return err;

    static const httpd_uri_t uri_root = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler };
    static const httpd_uri_t uri_scan = { .uri = "/api/scan", .method = HTTP_GET, .handler = scan_get_handler };
    static const httpd_uri_t uri_status = { .uri = "/api/status", .method = HTTP_GET, .handler = status_get_handler };
    static const httpd_uri_t uri_save = { .uri = "/api/save", .method = HTTP_POST, .handler = save_post_handler };
    static const httpd_uri_t uri_mqtt_get = { .uri = "/api/mqtt", .method = HTTP_GET, .handler = mqtt_get_handler };
    static const httpd_uri_t uri_mqtt_post = { .uri = "/api/mqtt", .method = HTTP_POST, .handler = mqtt_post_handler };

    static const httpd_uri_t uri_gen204 = { .uri = "/generate_204", .method = HTTP_GET, .handler = captive_redirect_handler };
    static const httpd_uri_t uri_hotspot = { .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = captive_redirect_handler };
    static const httpd_uri_t uri_ncsi = { .uri = "/ncsi.txt", .method = HTTP_GET, .handler = captive_redirect_handler };
    static const httpd_uri_t uri_connecttest = { .uri = "/connecttest.txt", .method = HTTP_GET, .handler = captive_redirect_handler };

    httpd_register_uri_handler(s_server, &uri_root);
    httpd_register_uri_handler(s_server, &uri_scan);
    httpd_register_uri_handler(s_server, &uri_status);
    httpd_register_uri_handler(s_server, &uri_save);
    httpd_register_uri_handler(s_server, &uri_mqtt_get);
    httpd_register_uri_handler(s_server, &uri_mqtt_post);
    httpd_register_uri_handler(s_server, &uri_gen204);
    httpd_register_uri_handler(s_server, &uri_hotspot);
    httpd_register_uri_handler(s_server, &uri_ncsi);
    httpd_register_uri_handler(s_server, &uri_connecttest);
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, http_404_redirect_handler);

    ESP_LOGI(TAG, "Web Config Server started on http://192.168.4.1/");
    return ESP_OK;
}

void web_server_stop(void) {
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}
