#include "wifi_manager.h"
#include "internet_check.h"
#include "web_server.h"
#include "dns_server.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/inet.h"

static const char *TAG = "WIFI_MANAGER";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_event_group;
static wifi_mgr_state_t s_current_state = WIFI_MGR_DISCONNECTED;
static char s_current_ip[16] = "0.0.0.0";
static int s_retry_num = 0;
#define MAXIMUM_RETRY  5

static wifi_static_ip_config_t s_static_ip_config = {
    .use_static_ip = false,
    .ip = "192.168.1.100",
    .netmask = "255.255.255.0",
    .gateway = "192.168.1.1",
    .dns = "8.8.8.8"
};

extern void draw_wifi_status_overlay(const char *text, uint16_t bg_color, uint16_t text_color);
extern void hide_wifi_status_overlay(void);

static void auto_hide_overlay_task(void *pvParameters) {
    vTaskDelay(pdMS_TO_TICKS(4000));
    if (s_current_state == WIFI_MGR_CONNECTED_GOT_INTERNET) {
        hide_wifi_status_overlay();
        dns_server_stop();
        web_server_stop();
        esp_wifi_set_mode(WIFI_MODE_STA);
        ESP_LOGI(TAG, "Internet verified -> Stopped SoftAP ESP32_WiFi_Setup & switched to pure STA mode");
    }
    vTaskDelete(NULL);
}

static void apply_ip_configuration(void) {
    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta_netif) return;

    if (s_static_ip_config.use_static_ip) {
        esp_netif_dhcpc_stop(sta_netif);

        esp_netif_ip_info_t ip_info;
        memset(&ip_info, 0, sizeof(ip_info));
        ip_info.ip.addr = inet_addr(s_static_ip_config.ip);
        ip_info.netmask.addr = inet_addr(s_static_ip_config.netmask);
        ip_info.gw.addr = inet_addr(s_static_ip_config.gateway);
        esp_netif_set_ip_info(sta_netif, &ip_info);

        esp_netif_dns_info_t dns_info;
        dns_info.ip.u_addr.ip4.addr = inet_addr(s_static_ip_config.dns);
        dns_info.ip.type = ESP_IPADDR_TYPE_V4;
        esp_netif_set_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns_info);

        ESP_LOGI(TAG, "Static IP configured: %s, GW: %s, DNS: %s", s_static_ip_config.ip, s_static_ip_config.gateway, s_static_ip_config.dns);
    } else {
        esp_netif_dhcpc_start(sta_netif);
        ESP_LOGI(TAG, "Using DHCP Client for IP configuration.");
    }
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        apply_ip_configuration();
        esp_wifi_connect();
        s_current_state = WIFI_MGR_CONNECTING;
        ESP_LOGI(TAG, "Connecting to AP...");
        draw_wifi_status_overlay("CONNECTING...", 0x001F, 0xFFFF); // Xanh dương / Chữ trắng
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_current_state == WIFI_MGR_SOFTAP_MODE) {
            ESP_LOGI(TAG, "In SoftAP mode, ignoring STA disconnect event.");
            return;
        }
        if (s_retry_num < MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retry connect to AP (%d/%d)...", s_retry_num, MAXIMUM_RETRY);
            draw_wifi_status_overlay("RETRYING...", 0xF800, 0xFFFF); // Đỏ / Chữ trắng
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            s_current_state = WIFI_MGR_DISCONNECTED;
            ESP_LOGW(TAG, "Failed to connect to AP");
            draw_wifi_status_overlay("DISCONNECTED", 0xF800, 0xFFFF);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        snprintf(s_current_ip, sizeof(s_current_ip), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Got Local IP Address: %s", s_current_ip);
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        draw_wifi_status_overlay("CHECKING INTERNET...", 0x07E0, 0xFFFF); // Xanh lá / Chữ trắng

        // Đã có IP Local -> Tiến hành kiểm tra xem thực sự có INTERNET hay không (Ping Google/Cloudflare)
        if (internet_check_perform()) {
            s_current_state = WIFI_MGR_CONNECTED_GOT_INTERNET;
            ESP_LOGI(TAG, ">>> WIFI & INTERNET FULLY CONNECTED & VERIFIED <<<");
            draw_wifi_status_overlay("INTERNET OK", 0x07E0, 0xFFFF); // Xanh lá / Chữ trắng
            xTaskCreate(auto_hide_overlay_task, "auto_hide_overlay", 2048, NULL, 5, NULL);
        } else {
            s_current_state = WIFI_MGR_CONNECTED_NO_INTERNET;
            ESP_LOGW(TAG, ">>> LOCAL IP OK, BUT NO INTERNET ACCESS <<<");
            draw_wifi_status_overlay("NO INTERNET", 0xF800, 0xFFFF); // Đỏ / Chữ trắng
        }
    }
}

void wifi_manager_init(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    // Read saved WiFi credentials and Static IP from NVS
    nvs_handle_t my_handle;
    char saved_ssid[32] = {0};
    char saved_pass[64] = {0};
    size_t ssid_len = sizeof(saved_ssid);
    size_t pass_len = sizeof(saved_pass);

    if (nvs_open("wifi_store", NVS_READONLY, &my_handle) == ESP_OK) {
        nvs_get_str(my_handle, "ssid", saved_ssid, &ssid_len);
        nvs_get_str(my_handle, "pass", saved_pass, &pass_len);

        uint8_t u_static = 0;
        if (nvs_get_u8(my_handle, "use_static", &u_static) == ESP_OK) {
            s_static_ip_config.use_static_ip = (u_static != 0);
        }
        size_t len = sizeof(s_static_ip_config.ip);
        nvs_get_str(my_handle, "ip_addr", s_static_ip_config.ip, &len);
        len = sizeof(s_static_ip_config.netmask);
        nvs_get_str(my_handle, "netmask", s_static_ip_config.netmask, &len);
        len = sizeof(s_static_ip_config.gateway);
        nvs_get_str(my_handle, "gateway", s_static_ip_config.gateway, &len);
        len = sizeof(s_static_ip_config.dns);
        nvs_get_str(my_handle, "dns_addr", s_static_ip_config.dns, &len);

        nvs_close(my_handle);
    }

    if (strlen(saved_ssid) > 0) {
        ESP_LOGI(TAG, "Found saved WiFi SSID in NVS: %s -> Booting in pure STA mode", saved_ssid);
        wifi_config_t sta_config = {0};
        strncpy((char*)sta_config.sta.ssid, saved_ssid, sizeof(sta_config.sta.ssid));
        strncpy((char*)sta_config.sta.password, saved_pass, sizeof(sta_config.sta.password));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
        ESP_ERROR_CHECK(esp_wifi_start());
    } else {
        ESP_LOGI(TAG, "No saved WiFi credentials found in NVS -> Starting SoftAP Captive Portal");
        s_current_state = WIFI_MGR_SOFTAP_MODE;

        wifi_config_t ap_config = {
            .ap = {
                .ssid = "ESP32_WiFi_Setup",
                .ssid_len = strlen("ESP32_WiFi_Setup"),
                .password = "12345678",
                .max_connection = 4,
                .authmode = WIFI_AUTH_WPA2_PSK
            }
        };

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
        ESP_ERROR_CHECK(esp_wifi_start());

        dns_server_start();
        web_server_start();

        draw_wifi_status_overlay("SETUP: ESP32_WiFi_Setup", 0x001F, 0xFFFF);
    }
}

bool wifi_manager_connect(const char *ssid, const char *password) {
    if (!ssid || strlen(ssid) == 0) return false;

    ESP_LOGI(TAG, "Saving and connecting to new WiFi: %s", ssid);

    nvs_handle_t my_handle;
    if (nvs_open("wifi_store", NVS_READWRITE, &my_handle) == ESP_OK) {
        nvs_set_str(my_handle, "ssid", ssid);
        nvs_set_str(my_handle, "pass", password ? password : "");
        nvs_commit(my_handle);
        nvs_close(my_handle);
    }

    wifi_config_t wifi_config = {0};
    strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    if (password) {
        strncpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
    }

    s_retry_num = 0;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    apply_ip_configuration();
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(15000));

    return (bits & WIFI_CONNECTED_BIT) != 0;
}

void wifi_manager_restart(void) {
    ESP_LOGI(TAG, "Restarting WiFi driver and reconnecting...");
    esp_wifi_disconnect();
    s_retry_num = 0;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    apply_ip_configuration();
    esp_wifi_connect();
}

void wifi_manager_reset_to_ap_mode(void) {
    ESP_LOGI(TAG, "Resetting WiFi credentials in NVS and opening SoftAP Captive Portal...");

    // 1. Erase all saved WiFi credentials & Static IP in Flash NVS
    nvs_handle_t my_handle;
    if (nvs_open("wifi_store", NVS_READWRITE, &my_handle) == ESP_OK) {
        nvs_erase_all(my_handle);
        nvs_commit(my_handle);
        nvs_close(my_handle);
    }

    // 2. Set state to SoftAP mode and stop any STA retry loops
    s_current_state = WIFI_MGR_SOFTAP_MODE;
    s_retry_num = MAXIMUM_RETRY;

    // 3. Clear STA configuration in RAM driver to prevent auto-reconnect
    wifi_config_t sta_clean_config = {0};
    esp_wifi_set_config(WIFI_IF_STA, &sta_clean_config);
    esp_wifi_disconnect();
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    // 4. Configure SoftAP Hotspot
    wifi_config_t ap_config = {
        .ap = {
            .ssid = "ESP32_WiFi_Setup",
            .ssid_len = strlen("ESP32_WiFi_Setup"),
            .password = "12345678",
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK
        }
    };
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    esp_wifi_start();

    // 5. Start Captive Portal DNS & Web Config Server
    dns_server_start();
    web_server_start();

    // 6. Display Setup Overlay on LCD
    draw_wifi_status_overlay("SETUP: ESP32_WiFi_Setup", 0x001F, 0xFFFF);
}

wifi_mgr_state_t wifi_manager_get_state(void) {
    return s_current_state;
}

void wifi_manager_get_ip_str(char *ip_buf, size_t buf_len) {
    if (ip_buf && buf_len > 0) {
        strncpy(ip_buf, s_current_ip, buf_len);
    }
}

void wifi_manager_get_static_ip_config(wifi_static_ip_config_t *cfg) {
    if (cfg) {
        *cfg = s_static_ip_config;
    }
}

void wifi_manager_set_static_ip_config(const wifi_static_ip_config_t *cfg) {
    if (cfg) {
        s_static_ip_config = *cfg;
        nvs_handle_t my_handle;
        if (nvs_open("wifi_store", NVS_READWRITE, &my_handle) == ESP_OK) {
            nvs_set_u8(my_handle, "use_static", s_static_ip_config.use_static_ip ? 1 : 0);
            nvs_set_str(my_handle, "ip_addr", s_static_ip_config.ip);
            nvs_set_str(my_handle, "netmask", s_static_ip_config.netmask);
            nvs_set_str(my_handle, "gateway", s_static_ip_config.gateway);
            nvs_set_str(my_handle, "dns_addr", s_static_ip_config.dns);
            nvs_commit(my_handle);
            nvs_close(my_handle);
        }
    }
}
