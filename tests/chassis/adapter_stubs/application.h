// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#define _APPLICATION_H_
#include "motor_control.h"
#include "runtime.h"
#include <mutex>
#include <string>
// 只声明设备适配层需要的接口/成员；方法体编译自生产 motor_control.cc。
class Application {
public:
    static Application& GetInstance() { static Application app; return app; }
    using MotorActionConfig = car::MotorActionConfig;
    void LoadMotorActionConfig();
    void SaveMotorActionConfig();
    MotorActionConfig GetMotorActionConfig();
    void SetMotorActionConfig(const MotorActionConfig& config);
    bool HandleWebMotorControl(int, int);
    bool HandleMotorActionWithDuration(int, int, int, int = 2);
    bool HandleMotorSequence(const car::MotorStep*, size_t);
    bool HandleMotorPattern(const std::string&, int);
    std::string GetMotorStatus();
    bool EnsureMotorTaskLocked();
    bool ApplyMotorOutputLocked();
    bool InitMotorPwmLocked();
    void StopMotorOutputLocked();
    bool StopMotorControl(const char*);
    void SetMotorNetworkAvailable(bool);
    MotorActionConfig motor_action_config_;
    std::mutex motor_config_mutex_, motor_mutex_;
    car::MotorControl motor_control_;
    TaskHandle_t motor_task_handle_ = nullptr;
    bool motor_network_available_ = false, motor_pwm_initialized_ = false, motor_hardware_fault_ = false;
    int applied_motor_direction_ = 0, applied_motor_speed_ = 0;
};
