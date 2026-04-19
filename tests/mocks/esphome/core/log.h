#pragma once
#include <cstdio>

#ifdef ENABLE_TEST_LOGGING
#define ESP_LOGD(tag, fmt, ...) printf("[D][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("[W][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) printf("[I][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) printf("[E][%s] " fmt "\n", tag, ##__VA_ARGS__)
#else
#define ESP_LOGD(tag, ...) (void)0
#define ESP_LOGW(tag, ...) (void)0
#define ESP_LOGI(tag, ...) (void)0
#define ESP_LOGE(tag, ...) (void)0
#endif
