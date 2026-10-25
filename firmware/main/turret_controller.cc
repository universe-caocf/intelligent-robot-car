// SPDX-License-Identifier: GPL-3.0-or-later
#include "turret_controller.h"
#include <sdkconfig.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <cJSON.h>
#include <algorithm>

namespace car {
static constexpr const char* TAG = "TurretControl";
static int64_t NowMs() { return esp_timer_get_time() / 1000; }

TurretHardwareConfig TurretHardwareConfig::FromBuildConfig() {
    TurretHardwareConfig config;
#if CONFIG_CAR_TURRET_ENABLED
    config.enabled = true;
    config.flywheel_gpio = CONFIG_CAR_TURRET_FLYWHEEL_GPIO;
    config.push_gpio = CONFIG_CAR_TURRET_PUSH_GPIO;
    config.confirm_gpio = CONFIG_CAR_TURRET_CONFIRM_GPIO;
    config.tilt_gpio = CONFIG_CAR_TURRET_TILT_GPIO;
    config.rest_us = CONFIG_CAR_TURRET_REST_US;
    config.push_us = CONFIG_CAR_TURRET_PUSH_US;
    config.timing = {CONFIG_CAR_TURRET_SPIN_MS, CONFIG_CAR_TURRET_PUSH_MS, CONFIG_CAR_TURRET_RECOVER_MS};
#endif
    return config;
}

bool TurretHardwareConfig::IsValid() const {
    // 当前板型的候选扩展脚白名单，排除音频、底盘、按键、USB、串口和 Flash/PSRAM。
    // 这些脚仍需核对到货板的原理图；白名单不是接线已验证的承诺。
    auto available = [](int pin) { return pin == 8 || pin == 9 || pin == 10 || pin == 11 || pin == 17 || pin == 38; };
    if (!available(flywheel_gpio) || !available(push_gpio) || !available(confirm_gpio) ||
        (tilt_gpio != -1 && !available(tilt_gpio)) || !timing.IsValid() ||
        rest_us < 500 || rest_us > 2500 || push_us < 500 || push_us > 2500 || rest_us == push_us) return false;
    const int pins[] = {flywheel_gpio, push_gpio, confirm_gpio, tilt_gpio};
    for (int i = 0; i < 4; ++i) for (int j = i + 1; j < 4; ++j)
        if (pins[i] >= 0 && pins[i] == pins[j]) return false;
    return true;
}

static esp_err_t ServoPulse(ledc_channel_t channel, int pulse_us) {
    uint32_t duty = static_cast<uint32_t>(pulse_us) * 16383 / 20000;
    esp_err_t error = ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, duty);
    return error == ESP_OK ? ledc_update_duty(LEDC_LOW_SPEED_MODE, channel) : error;
}

bool TurretController::Initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (attempted_) return initialized_;
    attempted_ = true;
    if (!config_.enabled) { reason_ = "disabled_by_build_config"; ESP_LOGI(TAG, "Turret disabled; no GPIO/PWM initialized"); return false; }
    if (!config_.IsValid()) { FailLocked("invalid_wiring_or_calibration_config"); return false; }
    auto pin = static_cast<gpio_num_t>(config_.flywheel_gpio);
    flywheel_touched_ = true;
    // 先清输出锁存值，再设输出模式；外部 MOSFET 还必须有硬件下拉。
    if (gpio_reset_pin(pin) != ESP_OK || gpio_set_level(pin, 0) != ESP_OK ||
        gpio_set_direction(pin, GPIO_MODE_OUTPUT) != ESP_OK) { FailLocked("flywheel_gpio_init_failed"); return false; }
    ledc_timer_config_t timer = {};
    timer.speed_mode = LEDC_LOW_SPEED_MODE;
    timer.duty_resolution = LEDC_TIMER_14_BIT;
    timer.timer_num = LEDC_TIMER_2;
    timer.freq_hz = 50; timer.clk_cfg = LEDC_AUTO_CLK;
    if (ledc_timer_config(&timer) != ESP_OK) { FailLocked("servo_timer_init_failed"); return false; }
    for (int i = 0; i < 2; ++i) {
        int gpio = i == 0 ? config_.push_gpio : config_.tilt_gpio;
        if (gpio < 0) continue;
        ledc_channel_config_t channel = {};
        channel.gpio_num = gpio; channel.speed_mode = LEDC_LOW_SPEED_MODE;
        channel.channel = i == 0 ? LEDC_CHANNEL_4 : LEDC_CHANNEL_5;
        channel.timer_sel = LEDC_TIMER_2; channel.duty = 0;
        (i == 0 ? push_touched_ : tilt_touched_) = true;
        if (ledc_channel_config(&channel) != ESP_OK) { FailLocked("servo_channel_init_failed"); return false; }
    }
    TaskHandle_t handle = nullptr;
    if (xTaskCreate([](void* arg) {
        auto* controller = static_cast<TurretController*>(arg);
        while (true) { controller->Poll(); vTaskDelay(pdMS_TO_TICKS(20)); }
    }, "turret_control", 4096, this, tskIDLE_PRIORITY + 3, &handle) != pdPASS) {
        FailLocked("executor_creation_failed"); return false;
    }
    task_handle_ = handle; initialized_ = true; reason_ = "ready_disarmed";
    ESP_LOGI(TAG, "Turret initialized disarmed; physical confirmation required, PWM timer2 channels4/5");
    return true;
}

void TurretController::ForceOffLocked() {
    if (flywheel_touched_) {
        auto pin = static_cast<gpio_num_t>(config_.flywheel_gpio);
        esp_err_t reset = gpio_reset_pin(pin);
        esp_err_t level = gpio_set_level(pin, 0);
        esp_err_t mode = gpio_set_direction(pin, GPIO_MODE_OUTPUT);
        if (reset != ESP_OK || level != ESP_OK || mode != ESP_OK) ESP_LOGE(TAG, "Cannot force flywheel GPIO low; disconnect power");
    }
    for (int i = 0; i < 2; ++i) {
        if (!(i == 0 ? push_touched_ : tilt_touched_)) continue;
        auto channel = i == 0 ? LEDC_CHANNEL_4 : LEDC_CHANNEL_5;
        auto pin = static_cast<gpio_num_t>(i == 0 ? config_.push_gpio : config_.tilt_gpio);
        esp_err_t stop = ledc_stop(LEDC_LOW_SPEED_MODE, channel, 0);
        esp_err_t reset = gpio_reset_pin(pin);
        esp_err_t level = gpio_set_level(pin, 0);
        esp_err_t mode = gpio_set_direction(pin, GPIO_MODE_OUTPUT);
        if (stop != ESP_OK || reset != ESP_OK || level != ESP_OK || mode != ESP_OK)
            ESP_LOGE(TAG, "Cannot disable servo channel %d; disconnect power", channel);
    }
}

void TurretController::FailLocked(const char* reason) {
    control_.Fault(); physical_press_eligible_ = false; tilt_pulse_us_ = 0;
    initialized_ = false; reason_ = reason;
    ForceOffLocked();
    ESP_LOGE(TAG, "Turret fault latched: %s; reboot required after inspection", reason);
}

bool TurretController::ApplyOutputLocked() {
    if (!initialized_) return control_.state() != TurretState::Fault;
    const bool running = control_.flywheel();
    // 取消时先关飞轮；不能把“回到初始角度”当作停机动作，直接撤销舵机脉冲。
    if (!running && gpio_set_level(static_cast<gpio_num_t>(config_.flywheel_gpio), 0) != ESP_OK) {
        FailLocked("flywheel_stop_failed"); return false;
    }
    int pulse = !running ? 0 : control_.pushing() ? config_.push_us : config_.rest_us;
    if (ServoPulse(LEDC_CHANNEL_4, pulse) != ESP_OK ||
        (config_.tilt_gpio >= 0 && ServoPulse(LEDC_CHANNEL_5, tilt_pulse_us_) != ESP_OK)) {
        FailLocked("servo_output_failed"); return false;
    }
    if (running && gpio_set_level(static_cast<gpio_num_t>(config_.flywheel_gpio), 1) != ESP_OK) {
        FailLocked("flywheel_start_failed"); return false;
    }
    return true;
}

bool TurretController::Request(int count) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || !network_available_ || control_.state() != TurretState::Disarmed || !control_.Request(count, NowMs())) {
        ESP_LOGW(TAG, "Request rejected: initialized=%d network=%d state=%s count=%d",
            initialized_, network_available_, TurretControl::StateName(control_.state()), count);
        return false;
    }
    physical_press_eligible_ = false; reason_ = "hold_physical_button_within_10s";
    ESP_LOGI(TAG, "Request pending: %d feed cycles; physical confirmation required", count);
    return true;
}

bool TurretController::BeginPhysicalConfirmation() {
    std::lock_guard<std::mutex> lock(mutex_);
    physical_press_eligible_ = initialized_ && network_available_ &&
        control_.state() == TurretState::AwaitingConfirmation && NowMs() < control_.deadline_ms() &&
        gpio_get_level(static_cast<gpio_num_t>(config_.confirm_gpio)) == 0;
    return physical_press_eligible_;
}

bool TurretController::ConfirmPhysical() {
    std::lock_guard<std::mutex> lock(mutex_);
    bool eligible = physical_press_eligible_; physical_press_eligible_ = false;
    if (!eligible || !initialized_ || !network_available_ || control_.state() != TurretState::AwaitingConfirmation ||
        gpio_get_level(static_cast<gpio_num_t>(config_.confirm_gpio)) != 0) return false;
    bool accepted = control_.Confirm(NowMs());
    if (!accepted) { tilt_pulse_us_ = 0; reason_ = "confirmation_expired"; }
    else { reason_ = "physical_confirmation_accepted"; ESP_LOGI(TAG, "Physical confirmation accepted; bounded feed sequence started"); }
    return ApplyOutputLocked() && accepted;
}

bool TurretController::Disarm(const char* reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    control_.Disarm(); physical_press_eligible_ = false; tilt_pulse_us_ = 0;
    if (control_.state() != TurretState::Fault) reason_ = reason;
    bool ok = ApplyOutputLocked();
    ESP_LOGI(TAG, "Disarm requested: %s", reason);
    return ok;
}

void TurretController::SetNetworkAvailable(bool available) {
    std::lock_guard<std::mutex> lock(mutex_);
    network_available_ = available;
    if (!available) {
        control_.Disarm(); physical_press_eligible_ = false; tilt_pulse_us_ = 0;
        if (control_.state() != TurretState::Fault) reason_ = "network_unavailable";
        ApplyOutputLocked();
        ESP_LOGW(TAG, "Network unavailable; turret request and output canceled");
    }
}

bool TurretController::SetTilt(int angle) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || !network_available_ || config_.tilt_gpio < 0 ||
        control_.state() != TurretState::Disarmed || angle < 60 || angle > 120) return false;
    // 仅约定舵机命令角度，不代表测得的炮口俯仰；到货后核验行程。
    tilt_pulse_us_ = 500 + angle * 2000 / 180;
    tilt_deadline_ms_ = NowMs() + 2000;
    reason_ = "tilt_command_accepted";
    return ApplyOutputLocked();
}

void TurretController::Poll() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return;
    // 直接复核按钮电平，松手取消不只依赖 UI 回调是否成功注册/及时执行。
    if ((control_.busy() || physical_press_eligible_) &&
        gpio_get_level(static_cast<gpio_num_t>(config_.confirm_gpio)) != 0) {
        control_.Disarm(); physical_press_eligible_ = false; tilt_pulse_us_ = 0;
        reason_ = "physical_button_released";
        ESP_LOGW(TAG, "Physical button released; feed sequence canceled");
        ApplyOutputLocked(); return;
    }
    auto previous_state = control_.state();
    int requested = control_.requested();
    bool changed = control_.Poll(NowMs());
    if (changed && control_.state() == TurretState::Disarmed) {
        physical_press_eligible_ = false; tilt_pulse_us_ = 0;
        reason_ = previous_state == TurretState::AwaitingConfirmation ? "confirmation_expired" :
            control_.completed_cycles() >= requested ? "feed_sequence_completed" : "executor_late_canceled";
    }
    if (tilt_pulse_us_ != 0 && NowMs() >= tilt_deadline_ms_) {
        tilt_pulse_us_ = 0; changed = true;
        ESP_LOGI(TAG, "Tilt pulse lease expired; servo released");
    }
    if (!changed) return;
    ESP_LOGI(TAG, "Turret state=%s completed_feed_cycles=%d", TurretControl::StateName(control_.state()), control_.completed_cycles());
    ApplyOutputLocked();
}

std::string TurretController::GetStatus() {
    std::lock_guard<std::mutex> lock(mutex_);
    // 查询只返回快照，不推进推弹动作；推进只发生在执行器任务。
    cJSON* status = cJSON_CreateObject();
    cJSON_AddBoolToObject(status, "enabled", config_.enabled);
    cJSON_AddBoolToObject(status, "initialized", initialized_);
    cJSON_AddBoolToObject(status, "network_available", network_available_);
    cJSON_AddBoolToObject(status, "hardware_fault", control_.state() == TurretState::Fault);
    cJSON_AddStringToObject(status, "state", TurretControl::StateName(control_.state()));
    cJSON_AddNumberToObject(status, "requested_cycles", control_.requested());
    cJSON_AddNumberToObject(status, "completed_feed_cycles", control_.completed_cycles());
    cJSON_AddNumberToObject(status, "confirmation_remaining_ms", control_.state() == TurretState::AwaitingConfirmation ?
        std::max<int64_t>(0, control_.deadline_ms() - NowMs()) : 0);
    cJSON_AddBoolToObject(status, "flywheel_commanded", control_.flywheel());
    cJSON_AddBoolToObject(status, "tilt_available", initialized_ && config_.tilt_gpio >= 0);
    cJSON_AddStringToObject(status, "reason", reason_.c_str());
    cJSON_AddStringToObject(status, "feedback", "commanded_output_only_no_dart_or_hit_sensor");
    char* json = cJSON_PrintUnformatted(status);
    std::string result = json ? json : "{}";
    cJSON_free(json); cJSON_Delete(status); return result;
}
}  // namespace car
