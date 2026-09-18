#include "dns_server.h"
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "DNS_SERVER";
static int dns_socket = -1;
static TaskHandle_t dns_task_handle = NULL;

typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qd_count;
    uint16_t an_count;
    uint16_t ns_count;
    uint16_t ar_count;
} dns_header_t;

static void dns_server_task(void *pvParameters) {
    uint8_t rx_buffer[512];
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    dns_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (dns_socket < 0) {
        ESP_LOGE(TAG, "Failed to create DNS socket");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY)
    };

    if (bind(dns_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind DNS socket to port 53");
        close(dns_socket);
        dns_socket = -1;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Captive Portal DNS Server running on port 53");

    while (1) {
        int len = recvfrom(dns_socket, rx_buffer, sizeof(rx_buffer), 0,
                           (struct sockaddr *)&client_addr, &client_addr_len);
        if (len < (int)sizeof(dns_header_t)) {
            continue;
        }

        dns_header_t *header = (dns_header_t *)rx_buffer;
        // Standard query response, QR=1 (response), AA=1 (authoritative)
        header->flags = htons(0x8400);
        header->an_count = header->qd_count; // Answer count = Question count

        // Append A-record answer pointing to 192.168.4.1
        int reply_len = len;
        if (reply_len + 16 < (int)sizeof(rx_buffer)) {
            // Pointer to query name at offset 12 (0xC00C)
            rx_buffer[reply_len++] = 0xC0;
            rx_buffer[reply_len++] = 0x0C;
            // Type A (1)
            rx_buffer[reply_len++] = 0x00;
            rx_buffer[reply_len++] = 0x01;
            // Class IN (1)
            rx_buffer[reply_len++] = 0x00;
            rx_buffer[reply_len++] = 0x01;
            // TTL 60 seconds
            rx_buffer[reply_len++] = 0x00;
            rx_buffer[reply_len++] = 0x00;
            rx_buffer[reply_len++] = 0x00;
            rx_buffer[reply_len++] = 0x3C;
            // Data length (4 bytes for IPv4)
            rx_buffer[reply_len++] = 0x00;
            rx_buffer[reply_len++] = 0x04;
            // IP: 192.168.4.1
            rx_buffer[reply_len++] = 192;
            rx_buffer[reply_len++] = 168;
            rx_buffer[reply_len++] = 4;
            rx_buffer[reply_len++] = 1;

            sendto(dns_socket, rx_buffer, reply_len, 0,
                   (struct sockaddr *)&client_addr, client_addr_len);
        }
    }
}

esp_err_t dns_server_start(void) {
    if (dns_task_handle != NULL) return ESP_OK;
    xTaskCreate(dns_server_task, "dns_server_task", 3072, NULL, 5, &dns_task_handle);
    return ESP_OK;
}

void dns_server_stop(void) {
    if (dns_socket >= 0) {
        close(dns_socket);
        dns_socket = -1;
    }
    if (dns_task_handle != NULL) {
        vTaskDelete(dns_task_handle);
        dns_task_handle = NULL;
    }
    ESP_LOGI(TAG, "DNS Server stopped");
}
