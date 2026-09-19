#ifndef JSON_PARSER_H
#define JSON_PARSER_H

#include <stdbool.h>
#include <stddef.h>

/**
 * @brief Parse and execute a JSON command string.
 * @param json_str Null-terminated JSON string.
 * @return true if command parsed successfully, false otherwise.
 */
bool json_parser_process(const char *json_str);

/**
 * @brief Create a status JSON payload string for reporting over MQTT or HTTP.
 * @param buf Output buffer to store JSON string.
 * @param buf_len Size of buffer.
 */
void json_parser_create_status(char *buf, size_t buf_len);

#endif // JSON_PARSER_H
