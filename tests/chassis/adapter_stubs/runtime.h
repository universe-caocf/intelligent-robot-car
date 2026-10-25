// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1;
inline const char* esp_err_to_name(esp_err_t) { return "injected driver failure"; }
using TaskHandle_t = void*;
using BaseType_t = int;
constexpr int pdPASS = 1, tskIDLE_PRIORITY = 0;
inline int64_t test_now = 0;
inline bool fail_task = false, fail_timer = false;
inline int fail_channel = -1, fail_duty_channel = -1, task_creations = 0;
inline bool fail_stop = false;
inline void (*task_entry)(void*) = nullptr;
inline void* task_arg = nullptr;
inline std::array<uint32_t, 4> duties{};
inline std::vector<std::pair<int, uint32_t>> writes;
inline std::map<std::string, int> nvs;
inline int nvs_writes = 0;
inline BaseType_t xTaskCreate(void (*entry)(void*), const char*, unsigned, void* arg, int, TaskHandle_t* handle) {
    ++task_creations;
    if (fail_task) return 0;
    task_entry = entry; task_arg = arg; *handle = arg; return pdPASS;
}
struct EndIteration {};
inline void vTaskDelay(int) { throw EndIteration{}; }
#define pdMS_TO_TICKS(ms) (ms)
inline int64_t esp_timer_get_time() { return test_now * 1000; }
inline void Tick(int64_t now) { test_now = now; try { task_entry(task_arg); } catch (const EndIteration&) {} }
using gpio_num_t = int;
constexpr int GPIO_MODE_OUTPUT = 1;
inline esp_err_t gpio_reset_pin(int) { return ESP_OK; }
inline esp_err_t gpio_set_direction(int, int) { return ESP_OK; }
inline esp_err_t gpio_set_level(int pin, int value) { duties.at(pin) = value; writes.emplace_back(pin, value); return ESP_OK; }
enum ledc_mode_t { LEDC_LOW_SPEED_MODE };
enum ledc_channel_t { LEDC_CHANNEL_0, LEDC_CHANNEL_1, LEDC_CHANNEL_2, LEDC_CHANNEL_3 };
constexpr int LEDC_TIMER_0 = 0, LEDC_TIMER_10_BIT = 10, LEDC_AUTO_CLK = 0;
struct ledc_timer_config_t { ledc_mode_t speed_mode; int duty_resolution, timer_num, freq_hz, clk_cfg; };
struct ledc_channel_config_t { int gpio_num; ledc_mode_t speed_mode; ledc_channel_t channel; int timer_sel; uint32_t duty; };
inline esp_err_t ledc_timer_config(const ledc_timer_config_t*) { return fail_timer ? ESP_FAIL : ESP_OK; }
inline esp_err_t ledc_channel_config(const ledc_channel_config_t* config) {
    return config->channel == fail_channel ? ESP_FAIL : ESP_OK;
}
inline esp_err_t ledc_set_duty(ledc_mode_t, ledc_channel_t channel, uint32_t value) {
    if ((value > 0 && channel == fail_duty_channel) || (value == 0 && fail_stop)) return ESP_FAIL;
    duties.at(channel) = value; writes.emplace_back(channel, value); return ESP_OK;
}
inline esp_err_t ledc_update_duty(ledc_mode_t, ledc_channel_t) { return ESP_OK; }
inline esp_err_t ledc_stop(ledc_mode_t, ledc_channel_t channel, int) { duties.at(channel) = 0; return ESP_OK; }
inline void ResetRuntime() {
    test_now = 0; fail_task = fail_timer = fail_stop = false;
    fail_channel = fail_duty_channel = -1; task_creations = 0;
    task_entry = nullptr; task_arg = nullptr; duties = {}; writes.clear(); nvs.clear(); nvs_writes = 0;
}
