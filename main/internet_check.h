#ifndef INTERNET_CHECK_H
#define INTERNET_CHECK_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Chạy thử nghiệm Ping tới Google (8.8.8.8) hoặc Cloudflare (1.1.1.1)
 * 
 * @param target_host Địa chỉ IP dạng chuỗi ("8.8.8.8" hoặc "1.1.1.1")
 * @param timeout_ms Thời gian chờ phản hồi tối đa (ms)
 * @return true nếu nhận được ICMP Echo Reply (Internet OK)
 * @return false nếu Timeout hoặc không có kết nối
 */
bool internet_check_ping(const char *target_host, uint32_t timeout_ms);

/**
 * @brief Kiểm tra toàn diện kết nối Internet (Thử Google trước, nếu xịt thử Cloudflare)
 * 
 * @return true nếu Internet thực sự hoạt động
 */
bool internet_check_perform(void);

#endif // INTERNET_CHECK_H
