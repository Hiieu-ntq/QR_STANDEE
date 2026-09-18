#include "internet_check.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "ping/ping_sock.h"

static const char *TAG = "INTERNET_CHECK";
static bool s_ping_success = false;

static void cmd_ping_on_ping_success(esp_ping_handle_t hdl, void *args) {
    uint8_t ttl;
    uint16_t seqno;
    uint32_t elapsed_time, recv_len;
    ip_addr_t target_addr;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TTL, &ttl, sizeof(ttl));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target_addr, sizeof(target_addr));
    esp_ping_get_profile(hdl, ESP_PING_PROF_SIZE, &recv_len, sizeof(recv_len));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed_time, sizeof(elapsed_time));
    
    ESP_LOGI(TAG, "Ping Reply from %s: bytes=%d time=%dms TTL=%d",
             inet_ntoa(target_addr.u_addr.ip4), (int)recv_len, (int)elapsed_time, (int)ttl);
    s_ping_success = true;
}

static void cmd_ping_on_ping_timeout(esp_ping_handle_t hdl, void *args) {
    uint16_t seqno;
    ip_addr_t target_addr;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target_addr, sizeof(target_addr));
    ESP_LOGW(TAG, "Ping Timeout from %s (seq=%d)", inet_ntoa(target_addr.u_addr.ip4), seqno);
}

static void cmd_ping_on_ping_end(esp_ping_handle_t hdl, void *args) {
    ip_addr_t target_addr;
    uint32_t transmitted, received;
    esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST, &transmitted, sizeof(transmitted));
    esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &received, sizeof(received));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target_addr, sizeof(target_addr));
    ESP_LOGI(TAG, "Ping Summary for %s: Sent=%d, Recv=%d", inet_ntoa(target_addr.u_addr.ip4), (int)transmitted, (int)received);
    esp_ping_delete_session(hdl);
}

bool internet_check_ping(const char *target_host, uint32_t timeout_ms) {
    s_ping_success = false;
    ip_addr_t target_addr;
    memset(&target_addr, 0, sizeof(target_addr));

    struct in_addr addr4;
    if (inet_aton(target_host, &addr4)) {
        target_addr.type = IPADDR_TYPE_V4;
        target_addr.u_addr.ip4.addr = addr4.s_addr;
    } else {
        ESP_LOGE(TAG, "Invalid IP string: %s", target_host);
        return false;
    }

    esp_ping_config_t ping_config = ESP_PING_DEFAULT_CONFIG();
    ping_config.target_addr = target_addr;
    ping_config.count = 2; // Gửi 2 gói tin ping
    ping_config.interval_ms = 500;
    ping_config.timeout_ms = timeout_ms;

    esp_ping_callbacks_t cbs = {
        .on_ping_success = cmd_ping_on_ping_success,
        .on_ping_timeout = cmd_ping_on_ping_timeout,
        .on_ping_end = cmd_ping_on_ping_end,
        .cb_args = NULL
    };

    esp_ping_handle_t ping_handle;
    if (esp_ping_new_session(&ping_config, &cbs, &ping_handle) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create ping session");
        return false;
    }

    ESP_LOGI(TAG, "Starting ICMP Ping to %s...", target_host);
    esp_ping_start(ping_handle);

    // Chờ tối đa (count * interval + timeout + 500ms)
    int wait_loops = (ping_config.count * ping_config.interval_ms + timeout_ms + 500) / 50;
    for (int i = 0; i < wait_loops; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (s_ping_success) {
            break;
        }
    }

    return s_ping_success;
}

bool internet_check_perform(void) {
    ESP_LOGI(TAG, "Checking Internet connectivity...");
    // 1. Thử ping Google DNS (8.8.8.8)
    if (internet_check_ping("8.8.8.8", 1500)) {
        ESP_LOGI(TAG, "Internet OK (Verified via Google 8.8.8.8)");
        return true;
    }

    // 2. Dự phòng: Thử ping Cloudflare DNS (1.1.1.1)
    ESP_LOGW(TAG, "Google DNS 8.8.8.8 unreached. Retrying with Cloudflare 1.1.1.1...");
    if (internet_check_ping("1.1.1.1", 1500)) {
        ESP_LOGI(TAG, "Internet OK (Verified via Cloudflare 1.1.1.1)");
        return true;
    }

    ESP_LOGE(TAG, "NO INTERNET ACCESS! (Both 8.8.8.8 and 1.1.1.1 failed)");
    return false;
}
