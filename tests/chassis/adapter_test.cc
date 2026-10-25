// SPDX-License-Identifier: GPL-3.0-or-later
#include "application.h"
#include <cassert>
#include <iostream>
#include <stdexcept>

void AllStopped() { for (auto duty : duties) assert(duty == 0); }
int main() {
    ResetRuntime();
    {
        Application app;
        assert(!app.HandleMotorActionWithDuration(4, 80, 1000));
        assert(task_creations == 0);  // 未联网不得初始化或输出
        app.SetMotorNetworkAvailable(true);
        assert(app.HandleMotorActionWithDuration(4, 80, 1000));
        assert(duties[0] == 818 && duties[2] == 818 && duties[1] == 0 && duties[3] == 0);
        test_now = 400;
        assert(app.HandleMotorActionWithDuration(2, 60, 1500));
        assert(duties[0] == 0 && duties[2] == 0 && duties[1] == 613 && duties[3] == 613);
        Tick(1000);
        assert(duties[1] == 613 && task_creations == 1); // 旧截止时间不生效，只有一个执行器
        Tick(1900); AllStopped();
        test_now = 2000;
        assert(app.HandleWebMotorControl(4, 80));
        test_now = 2600;
        size_t before = writes.size();
        assert(app.HandleWebMotorControl(4, 80) && writes.size() == before); // 心跳续期但不抖动 PWM
        Tick(2750); assert(duties[0] > 0);
        Tick(3350); AllStopped();
        assert(app.HandleMotorPattern("dance", 50));
        assert(app.HandleMotorActionWithDuration(0, 0, 0));
        Tick(10000); AllStopped();
        assert(app.motor_control_.pending_steps() == 0);
        assert(app.HandleMotorActionWithDuration(4, 80, 1000));
        app.SetMotorNetworkAvailable(false); AllStopped();
        assert(!app.HandleWebMotorControl(4, 80));
        app.SetMotorNetworkAvailable(true); Tick(20000); AllStopped(); // 重连不会恢复旧序列
        assert(!app.HandleMotorActionWithDuration(4, 80, 1000, 0));
    }
    ResetRuntime();
    {
        Application app; app.SetMotorNetworkAvailable(true); fail_task = true;
        assert(!app.HandleMotorActionWithDuration(4, 80, 1000)); AllStopped();
        assert(app.motor_task_handle_ == nullptr);
        fail_task = false;
        assert(app.HandleMotorActionWithDuration(4, 80, 1000)); // 创建失败可重试
    }
    for (int failure = 0; failure < 3; ++failure) {
        ResetRuntime(); Application app; app.SetMotorNetworkAvailable(true);
        if (failure == 0) fail_timer = true;
        if (failure == 1) fail_channel = 2;
        if (failure == 2) fail_duty_channel = 2;
        assert(!app.HandleMotorActionWithDuration(4, 80, 1000)); AllStopped();
        assert(app.motor_hardware_fault_ && app.motor_control_.source() == car::MotorSource::None);
        fail_timer = false; fail_channel = fail_duty_channel = -1;
        assert(!app.HandleMotorActionWithDuration(4, 80, 1000)); // 故障不能自动降级满速或恢复
        assert(app.GetMotorStatus().find("\"hardware_fault\":true") != std::string::npos);
    }
    ResetRuntime();
    {
        Application app; app.SetMotorNetworkAvailable(true);
        assert(app.HandleMotorActionWithDuration(4, 80, 1000));
        fail_stop = true;
        assert(!app.HandleMotorActionWithDuration(0, 0, 0)); AllStopped();
        assert(app.motor_hardware_fault_);
    }
    ResetRuntime();
    {
        Application app;
        nvs["forward_ms"] = 900; nvs["backward_ms"] = -1; nvs["def_speed_pct"] = 500;
        app.LoadMotorActionConfig();
        auto config = app.GetMotorActionConfig();
        assert(config.forward_duration_ms == 900 && config.backward_duration_ms == 1500 && config.default_speed_percent == 80);
        assert(nvs["backward_ms"] == -1 && nvs_writes == 0); // 加载不修改历史配置
        config.forward_duration_ms = 1100; app.SetMotorActionConfig(config);
        assert(nvs["forward_ms"] == 1100 && nvs_writes == 10);
        config.default_speed_percent = -1;
        try { app.SetMotorActionConfig(config); assert(false); } catch (const std::invalid_argument&) {}
        assert(nvs_writes == 10);
    }
    std::cout << "PASS: production motor adapter, PWM mapping, executor deadlines, disconnect, task/driver failure injection, NVS fallback\n";
}
