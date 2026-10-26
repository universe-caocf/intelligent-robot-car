// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "motor_control.h"
#include <string>
// 只替代设备适配层；HTTP 处理器和状态机均使用生产源码。
class Application {
public:
    static Application& GetInstance() { static Application app; return app; }
    car::MotorActionConfig config;
    car::MotorControl motor;
    int64_t now = 0;
    int calls = 0;
    bool available = true;
    car::MotorActionConfig GetMotorActionConfig() { return config; }
    std::string GetMotorStatus() { return "{\"feedback\":\"commanded_output_only_no_motion_sensor\"}"; }
    bool HandleMotorActionWithDuration(int direction, int speed, int duration, int = 2) {
        ++calls;
        if (!car::MotorControl::ValidCommand(direction, speed)) return false;
        if (direction == 0 || speed == 0) { motor.Stop(); return true; }
        car::MotorStep step{direction, speed, duration};
        return available && motor.Start(&step, 1, now);
    }
    bool HandleMotorPattern(const std::string&, int) { ++calls; return available; }
};
