// SPDX-License-Identifier: GPL-3.0-or-later
#include "application.h"
#include "turret_controller.h"
#include "settings.h"
#include "boards/intelligent-car/config.h"
#include <algorithm>
#include <stdexcept>
#include <esp_log.h>
#include <esp_timer.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <driver/ledc.h>

#define TAG "MotorControl"

// 本模块已用 motor_mutex_ 串行化四路输出，无需 LEDC fade 服务。
static esp_err_t SetMotorDuty(int channel, uint32_t duty) {
    auto number = static_cast<ledc_channel_t>(channel);
    esp_err_t error = ledc_set_duty(LEDC_LOW_SPEED_MODE, number, duty);
    return error == ESP_OK ? ledc_update_duty(LEDC_LOW_SPEED_MODE, number) : error;
}

// 保留板级兼容入口，明确指令按到达顺序覆盖；低优先级自动动作被拒绝。
extern "C" void HandleMotorActionForApplication(int direction, int speed, int duration_ms, int priority) {
    if (!Application::GetInstance().HandleMotorActionWithDuration(direction, speed, duration_ms, priority)) {
        throw std::runtime_error("Motor command rejected: check parameters, network and motor status");
    }
}

bool Application::EnsureMotorTaskLocked() {
    if (motor_task_handle_) return true;
    BaseType_t rc = xTaskCreate([](void* arg) {
        auto* app = static_cast<Application*>(arg);
        while (true) {
            {
                std::lock_guard<std::mutex> lock(app->motor_mutex_);
                if (app->motor_control_.Poll(esp_timer_get_time() / 1000)) {
                    ESP_LOGI(TAG, "Motor deadline reached; source=%d direction=%d",
                        static_cast<int>(app->motor_control_.source()), app->motor_control_.direction());
                    app->ApplyMotorOutputLocked();
                }
            }
            vTaskDelay(pdMS_TO_TICKS(car::kMotorPollMs));
        }
    }, "motor_control", 4096, this, tskIDLE_PRIORITY + 3, &motor_task_handle_);
    if (rc != pdPASS) {
        motor_task_handle_ = nullptr;
        ESP_LOGE(TAG, "Motor executor creation failed; command rejected");
        return false;
    }
    return true;
}

bool Application::HandleMotorActionWithDuration(int direction, int speed, int duration_ms, int priority) {
    if (!car::MotorControl::ValidCommand(direction, speed)) return false;
    if (direction == 0 || speed == 0) { return StopMotorControl("explicit stop"); }
    if (priority < 1) { ESP_LOGW(TAG, "Automatic motor movement disabled"); return false; }
    const car::MotorStep step{direction, speed, duration_ms};
    return HandleMotorSequence(&step, 1);
}

bool Application::HandleMotorSequence(const car::MotorStep* steps, size_t count) {
    std::lock_guard<std::mutex> lock(motor_mutex_);
    if (!motor_network_available_ || motor_hardware_fault_ || !EnsureMotorTaskLocked()) {
        ESP_LOGW(TAG, "Motor command rejected: network=%d fault=%d", motor_network_available_, motor_hardware_fault_);
        return false;
    }
    if (!motor_control_.Start(steps, count, esp_timer_get_time() / 1000)) return false;
    ESP_LOGI(TAG, "Timed motor command accepted: steps=%u direction=%d speed=%d", static_cast<unsigned>(count),
        motor_control_.direction(), motor_control_.speed());
    return ApplyMotorOutputLocked();
}

bool Application::HandleMotorPattern(const std::string& pattern, int speed) {
    auto config = GetMotorActionConfig();
    if (speed < 0 || speed > 100) return false;
    if (speed == 0) { return StopMotorControl("pattern speed zero"); }
    if (pattern == "wiggle") {
        int half = std::max(car::kMinDurationMs, config.wiggle_duration_ms / 2);
        const car::MotorStep steps[] = {{3, speed, half}, {1, speed, half}};
        return HandleMotorSequence(steps, 2);
    }
    if (pattern == "dance") {
        int part = std::max(car::kMinDurationMs, config.dance_duration_ms / 4);
        const car::MotorStep steps[] = {{4, speed, part}, {3, speed, part}, {1, speed, part}, {2, speed, part}};
        return HandleMotorSequence(steps, 4);
    }
    return false;
}

bool Application::HandleWebMotorControl(int direction, int speed) {
    if (!car::MotorControl::ValidCommand(direction, speed)) return false;
    if (direction == 0 || speed == 0) { return StopMotorControl("web stop"); }
    std::lock_guard<std::mutex> lock(motor_mutex_);
    if (!motor_network_available_ || motor_hardware_fault_ || !EnsureMotorTaskLocked()) return false;
    // 心跳总是续期，不能因为方向和速度相同而丢弃。
    if (!motor_control_.WebCommand(direction, speed, esp_timer_get_time() / 1000)) return false;
    return ApplyMotorOutputLocked();
}

void Application::SetMotorNetworkAvailable(bool available) {
    car::TurretController::GetInstance().SetNetworkAvailable(available);
    std::lock_guard<std::mutex> lock(motor_mutex_);
    motor_network_available_ = available;
    if (!available) {
        motor_control_.Stop();
        StopMotorOutputLocked();
        ESP_LOGW(TAG, "Network unavailable: motor stopped and sequence cleared");
    }
}

bool Application::StopMotorControl(const char* reason) {
    // 保留旧停车入口兼容性，同时取消炮塔；两模块不嵌套持锁。
    bool turret_stopped = car::TurretController::GetInstance().Disarm(reason);
    std::lock_guard<std::mutex> lock(motor_mutex_);
    motor_control_.Stop();
    StopMotorOutputLocked();
    ESP_LOGI(TAG, "Motor stop requested, sequence cleared: %s", reason);
    return !motor_hardware_fault_ && turret_stopped;
}

void Application::StopMotorOutputLocked() {
    if (motor_pwm_initialized_) {
        for (int i = 0; i < 4; ++i) {
            if (SetMotorDuty(i, 0) != ESP_OK) {
                motor_hardware_fault_ = true;
                ESP_LOGE(TAG, "Motor PWM stop failed on channel %d", i);
                ledc_stop(LEDC_LOW_SPEED_MODE, static_cast<ledc_channel_t>(i), 0);
            }
        }
    }
    if (motor_hardware_fault_) {
        motor_control_.Stop();
        // 驱动调用失败后断开 PWM 路由并拉低 GPIO；故障锁定到重启，不能悄悄恢复行驶。
        for (auto pin : {MOTOR_LF_GPIO, MOTOR_LB_GPIO, MOTOR_RF_GPIO, MOTOR_RB_GPIO}) {
            esp_err_t reset = gpio_reset_pin(pin);
            esp_err_t mode = gpio_set_direction(pin, GPIO_MODE_OUTPUT);
            esp_err_t level = gpio_set_level(pin, 0);
            if (reset != ESP_OK || mode != ESP_OK || level != ESP_OK) ESP_LOGE(TAG, "Failed to force motor GPIO %d low", pin);
        }
        motor_pwm_initialized_ = false;
    }
    applied_motor_direction_ = applied_motor_speed_ = 0;
}

bool Application::InitMotorPwmLocked() {
    if (motor_pwm_initialized_) return true;
    ledc_timer_config_t timer = {};
    timer.speed_mode = LEDC_LOW_SPEED_MODE;
    timer.duty_resolution = LEDC_TIMER_10_BIT;
    timer.timer_num = LEDC_TIMER_0;
    timer.freq_hz = 20000;
    timer.clk_cfg = LEDC_AUTO_CLK;
    const gpio_num_t pins[] = {MOTOR_LF_GPIO, MOTOR_LB_GPIO, MOTOR_RF_GPIO, MOTOR_RB_GPIO};
    esp_err_t err = ledc_timer_config(&timer);
    for (int i = 0; err == ESP_OK && i < 4; ++i) {
        ledc_channel_config_t channel = {};
        channel.gpio_num = pins[i];
        channel.speed_mode = LEDC_LOW_SPEED_MODE;
        channel.channel = static_cast<ledc_channel_t>(i);
        channel.timer_sel = LEDC_TIMER_0;
        channel.duty = 0;
        err = ledc_channel_config(&channel);
    }
    if (err != ESP_OK) {
        // 部分初始化失败也拉低所有引脚，禁止回退到 GPIO 满速行驶。
        motor_hardware_fault_ = true;
        StopMotorOutputLocked();
        ESP_LOGE(TAG, "Motor PWM initialization failed: %s; reboot required", esp_err_to_name(err));
        return false;
    }
    motor_pwm_initialized_ = true;
    return true;
}

bool Application::ApplyMotorOutputLocked() {
    int direction = motor_control_.direction();
    int speed = motor_control_.speed();
    if (direction == 0 || speed == 0) { StopMotorOutputLocked(); return !motor_hardware_fault_; }
    if (!InitMotorPwmLocked()) { motor_control_.Stop(); return false; }
    if (direction == applied_motor_direction_ && speed == applied_motor_speed_) return true;
    // 先关闭四路再输出目标方向；不用独立异步 fade，避免旧渐变复活输出。
    StopMotorOutputLocked();
    if (motor_hardware_fault_) { motor_control_.Stop(); return false; }
    const int channels[5][2] = {{0, 0}, {0, 3}, {1, 3}, {1, 2}, {0, 2}};
    uint32_t duty = static_cast<uint32_t>(speed) * 1023 / 100;
    for (int i : channels[direction]) {
        if (SetMotorDuty(i, duty) != ESP_OK) {
            motor_hardware_fault_ = true;
            motor_control_.Stop();
            StopMotorOutputLocked();
            ESP_LOGE(TAG, "Motor output failed; command canceled, reboot required");
            return false;
        }
    }
    applied_motor_direction_ = direction;
    applied_motor_speed_ = speed;
    return true;
}

std::string Application::GetMotorStatus() {
    std::lock_guard<std::mutex> lock(motor_mutex_);
    if (motor_control_.Poll(esp_timer_get_time() / 1000)) ApplyMotorOutputLocked();
    const char* source = motor_control_.source() == car::MotorSource::Web ? "web" :
        motor_control_.source() == car::MotorSource::Timed ? "timed" : "none";
    cJSON* status = cJSON_CreateObject();
    cJSON_AddStringToObject(status, "source", source);
    cJSON_AddBoolToObject(status, "active", motor_control_.source() != car::MotorSource::None);
    cJSON_AddNumberToObject(status, "direction", motor_control_.direction());
    cJSON_AddNumberToObject(status, "speed_percent", motor_control_.speed());
    cJSON_AddNumberToObject(status, "remaining_ms", std::max<int64_t>(0, motor_control_.deadline_ms() - esp_timer_get_time() / 1000));
    cJSON_AddNumberToObject(status, "pending_steps", motor_control_.pending_steps());
    cJSON_AddBoolToObject(status, "network_available", motor_network_available_);
    cJSON_AddBoolToObject(status, "hardware_fault", motor_hardware_fault_);
    cJSON_AddStringToObject(status, "feedback", "commanded_output_only_no_motion_sensor");
    char* json = cJSON_PrintUnformatted(status);
    std::string result = json ? json : "{}";
    cJSON_free(json);
    cJSON_Delete(status);
    return result;
}

Application::MotorActionConfig Application::GetMotorActionConfig() {
    std::lock_guard<std::mutex> lock(motor_config_mutex_);
    return motor_action_config_;
}

void Application::LoadMotorActionConfig() {
    Settings settings("motor_config");
    MotorActionConfig defaults;
    auto read_duration = [&](const char* key, int fallback) {
        int value = settings.GetInt(key, fallback);
        if (value >= car::kMinDurationMs && value <= car::kMaxDurationMs) return value;
        ESP_LOGW(TAG, "Invalid saved motor duration %s=%d; using %d without rewriting NVS", key, value, fallback);
        return fallback;
    };
    motor_action_config_.forward_duration_ms = read_duration("forward_ms", defaults.forward_duration_ms);
    motor_action_config_.backward_duration_ms = read_duration("backward_ms", defaults.backward_duration_ms);
    motor_action_config_.left_turn_duration_ms = read_duration("left_turn_ms", defaults.left_turn_duration_ms);
    motor_action_config_.right_turn_duration_ms = read_duration("right_turn_ms", defaults.right_turn_duration_ms);
    motor_action_config_.spin_duration_ms = read_duration("spin_ms", defaults.spin_duration_ms);
    motor_action_config_.wiggle_duration_ms = read_duration("wiggle_ms", defaults.wiggle_duration_ms);
    motor_action_config_.dance_duration_ms = read_duration("dance_ms", defaults.dance_duration_ms);
    motor_action_config_.quick_forward_duration_ms = read_duration("quick_fwd_ms", defaults.quick_forward_duration_ms);
    motor_action_config_.quick_backward_duration_ms = read_duration("quick_bwd_ms", defaults.quick_backward_duration_ms);
    int speed = settings.GetInt("def_speed_pct", defaults.default_speed_percent);
    if (speed < 0 || speed > 100) {
        ESP_LOGW(TAG, "Invalid saved motor speed %d; using %d without rewriting NVS", speed, defaults.default_speed_percent);
        speed = defaults.default_speed_percent;
    }
    motor_action_config_.default_speed_percent = speed;
    ESP_LOGI(TAG, "Motor defaults loaded: speed=%d forward_ms=%d", speed, motor_action_config_.forward_duration_ms);
}

void Application::SaveMotorActionConfig() {
    // 调用方持有 motor_config_mutex_，保证一份配置完整写入。
    Settings settings("motor_config", true);
    settings.SetInt("forward_ms", motor_action_config_.forward_duration_ms);
    settings.SetInt("backward_ms", motor_action_config_.backward_duration_ms);
    settings.SetInt("left_turn_ms", motor_action_config_.left_turn_duration_ms);
    settings.SetInt("right_turn_ms", motor_action_config_.right_turn_duration_ms);
    settings.SetInt("spin_ms", motor_action_config_.spin_duration_ms);
    settings.SetInt("wiggle_ms", motor_action_config_.wiggle_duration_ms);
    settings.SetInt("dance_ms", motor_action_config_.dance_duration_ms);
    settings.SetInt("quick_fwd_ms", motor_action_config_.quick_forward_duration_ms);
    settings.SetInt("quick_bwd_ms", motor_action_config_.quick_backward_duration_ms);
    settings.SetInt("def_speed_pct", motor_action_config_.default_speed_percent);
}

void Application::SetMotorActionConfig(const MotorActionConfig& config) {
    if (!config.IsValid()) throw std::invalid_argument("Motor duration must be 100..10000ms and speed 0..100");
    std::lock_guard<std::mutex> lock(motor_config_mutex_);
    motor_action_config_ = config;
    SaveMotorActionConfig();
    ESP_LOGI(TAG, "Motor configuration saved: speed=%d forward_ms=%d", config.default_speed_percent, config.forward_duration_ms);
}
