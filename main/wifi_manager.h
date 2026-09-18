#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>
#include <stddef.h>

/**
 * @brief Trạng thái WiFi hiện tại
 */
typedef enum {
    WIFI_MGR_DISCONNECTED,
    WIFI_MGR_CONNECTING,
    WIFI_MGR_CONNECTED_NO_INTERNET,
    WIFI_MGR_CONNECTED_GOT_INTERNET,
    WIFI_MGR_SOFTAP_MODE
} wifi_mgr_state_t;

/**
 * @brief Cấu hình IP tĩnh
 */
typedef struct {
    bool use_static_ip;
    char ip[16];
    char netmask[16];
    char gateway[16];
    char dns[16];
} wifi_static_ip_config_t;

/**
 * @brief Khởi tạo WiFi Manager (Station & SoftAP) & NVS Storage
 */
void wifi_manager_init(void);

/**
 * @brief Kết nối tới một mạng WiFi mới và lưu NVS
 */
bool wifi_manager_connect(const char *ssid, const char *password);

/**
 * @brief Khởi động lại mô-đun WiFi & Thử kết nối lại
 */
void wifi_manager_restart(void);

/**
 * @brief Xóa thông tin WiFi đã lưu, bật lại SoftAP & Captive Portal để quét WiFi mới
 */
void wifi_manager_reset_to_ap_mode(void);

/**
 * @brief Lấy trạng thái Mạng hiện tại
 */
wifi_mgr_state_t wifi_manager_get_state(void);

/**
 * @brief Đọc thông tin IP hiện tại
 */
void wifi_manager_get_ip_str(char *ip_buf, size_t buf_len);

/**
 * @brief Đọc và Lưu Cấu hình IP Tĩnh
 */
void wifi_manager_get_static_ip_config(wifi_static_ip_config_t *cfg);
void wifi_manager_set_static_ip_config(const wifi_static_ip_config_t *cfg);

#endif // WIFI_MANAGER_H
