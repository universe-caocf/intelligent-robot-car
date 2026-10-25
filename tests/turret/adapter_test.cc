// SPDX-License-Identifier: GPL-3.0-or-later
#include "application.h"
#include "turret_controller.h"
#include <sdkconfig.h>
#include <cassert>
#include <iostream>
using namespace car;
TurretHardwareConfig Config() {
    TurretHardwareConfig config; config.enabled = true;
    config.flywheel_gpio = 8; config.push_gpio = 9; config.confirm_gpio = 11; config.tilt_gpio = 10;
    return config;
}
void AllStopped() { for (auto value : duties) assert(value == 0); }
void Start(TurretController& controller, int count = 1) {
    assert(controller.Request(count));
    input_levels[11] = 0;
    assert(controller.BeginPhysicalConfirmation());
    assert(controller.ConfirmPhysical());
}
int main() {
    ResetRuntime();
    {
        TurretHardwareConfig config; TurretController disabled(config);
        assert(!disabled.Initialize() && !disabled.Request(1) && !disabled.ConfirmPhysical());
        assert(disabled.CanSleep());
        disabled.SetNetworkAvailable(true); assert(disabled.Disarm("stop"));
        assert(disabled.GetStatus().find("disabled") != std::string::npos);
        assert(gpio_calls == 0 && task_creations == 0 && timer_configs.empty() && writes.empty());
        assert(TurretHardwareConfig::FromBuildConfig().enabled == static_cast<bool>(CONFIG_CAR_TURRET_ENABLED));
    }
    for (int bad = 0; bad < 7; ++bad) {
        ResetRuntime(); auto config = Config();
        if (bad == 0) config.flywheel_gpio = -1;
        if (bad == 1) config.confirm_gpio = config.push_gpio;
        if (bad == 2) config.push_gpio = 12; // 底盘占用脚
        if (bad == 3) config.rest_us = config.push_us;
        if (bad == 4) config.timing.spin_ms = 0;
        if (bad == 5) config.tilt_min_us = config.tilt_max_us;
        if (bad == 6) config.tilt_min_angle = config.tilt_max_angle;
        TurretController controller(config);
        assert(!controller.Initialize() && gpio_calls == 0 && timer_configs.empty());
        assert(controller.GetStatus().find("invalid_wiring") != std::string::npos);
    }
    ResetRuntime();
    {
        TurretController controller(Config()); assert(controller.Initialize()); AllStopped();
        assert(controller.CanSleep());
        assert((timer_configs == std::vector<std::pair<int, int>>{{2, 14}}));
        assert((configured_channels == std::vector<int>{4, 5}));
        assert(!controller.Request(1)); // 离线不得动作
        controller.SetNetworkAvailable(true);
        input_levels[11] = 0;
        assert(!controller.BeginPhysicalConfirmation()); // 先按住，后申请，不算确认
        assert(controller.Request(2) && !controller.ConfirmPhysical()); AllStopped();
        assert(!controller.CanSleep());
        assert(!controller.Request(3));
        assert(controller.BeginPhysicalConfirmation() && controller.ConfirmPhysical());
        assert(duties[8] == 1 && duties[4] == 1000 * 16383 / 20000);
        assert(!controller.CanSleep());
        assert(!controller.ConfirmPhysical() && !controller.SetTilt(90));
        test_now = 700;
        auto before = writes.size();
        assert(controller.GetStatus().find("spinning") != std::string::npos && writes.size() == before);
        Tick(700); assert(duties[4] == 1500 * 16383 / 20000);
        Tick(950); assert(duties[4] == 1000 * 16383 / 20000);
        Tick(1400); assert(duties[4] == 1500 * 16383 / 20000);
        Tick(1650); Tick(2100); AllStopped();
        assert(controller.GetStatus().find("\"completed_feed_cycles\":2") != std::string::npos);
        assert(task_creations == 1 && controller.Initialize());
        assert(controller.Request(1));
        test_now += 10000;
        assert(controller.BeginPhysicalConfirmation() == false && !controller.ConfirmPhysical());
        Tick(test_now); AllStopped();
        assert(controller.GetStatus().find("disarmed") != std::string::npos);
        assert(controller.SetTilt(90) && duties[5] == 1500 * 16383 / 20000);
        assert(!controller.CanSleep());
        assert(!controller.SetTilt(59) && !controller.SetTilt(121));
        Tick(test_now + 2000); assert(duties[5] == 0); // 俯仰输出不会无限保持堵转
        assert(controller.SetTilt(90));
        assert(controller.Disarm("release")); AllStopped();
        assert(controller.CanSleep());
    }
    for (int phase = 0; phase < 4; ++phase) {
        ResetRuntime(); TurretController controller(Config()); assert(controller.Initialize());
        controller.SetNetworkAvailable(true); assert(controller.Request(3));
        input_levels[11] = 0;
        if (phase > 0) { assert(controller.BeginPhysicalConfirmation() && controller.ConfirmPhysical()); }
        if (phase > 1) Tick(700);
        if (phase > 2) Tick(950);
        assert(controller.Disarm("cancel")); AllStopped();
        Tick(20000); AllStopped(); assert(!controller.ConfirmPhysical());
    }
    ResetRuntime();
    {
        TurretController controller(Config()); assert(controller.Initialize());
        controller.SetNetworkAvailable(true); Start(controller, 3); Tick(700);
        input_levels[11] = 1; Tick(720); AllStopped(); // 模拟释放回调缺失，GPIO轮询仍取消
        Tick(10000); AllStopped();
        assert(controller.GetStatus().find("physical_button_released") != std::string::npos);
    }
    ResetRuntime();
    {
        auto config = Config();
        config.tilt_min_us = 1000; config.tilt_max_us = 2000;
        config.tilt_min_angle = 80; config.tilt_max_angle = 100;
        TurretController controller(config); assert(controller.Initialize());
        controller.SetNetworkAvailable(true);
        assert(!controller.SetTilt(79) && !controller.SetTilt(101));
        assert(controller.SetTilt(80) && duties[5] == (1000 + 80 * 1000 / 180) * 16383 / 20000);
        assert(controller.SetTilt(100) && duties[5] == (1000 + 100 * 1000 / 180) * 16383 / 20000);
        Tick(2000); assert(duties[5] == 0 && controller.CanSleep());
    }
    ResetRuntime();
    {
        auto config = Config(); config.tilt_gpio = -1;
        TurretController controller(config); assert(controller.Initialize());
        controller.SetNetworkAvailable(true); assert(!controller.SetTilt(90));
        Start(controller, 3); controller.SetNetworkAvailable(false); AllStopped();
        assert(!controller.Request(1));
        controller.SetNetworkAvailable(true); Tick(10000); AllStopped();
        assert(!controller.ConfirmPhysical());
        Start(controller); Tick(10901); AllStopped(); // 严重迟到取消，绝不补跑推弹
    }
    for (int failure = 0; failure < 5; ++failure) {
        ResetRuntime();
        if (failure == 0) fail_timer = true;
        if (failure == 1) fail_channel = 4;
        if (failure == 2) fail_channel = 5;
        if (failure == 3) fail_task = true;
        if (failure == 4) { fail_gpio_pin = 8; fail_gpio_once = true; }
        TurretController controller(Config());
        assert(!controller.Initialize()); AllStopped();
        fail_timer = fail_task = false; fail_channel = -1;
        assert(!controller.Initialize() && !controller.Request(1)); // 故障保持，不能自动重启
        assert(!controller.CanSleep());
        assert(controller.GetStatus().find("\"hardware_fault\":true") != std::string::npos);
    }
    for (int failure = 0; failure < 4; ++failure) {
        ResetRuntime(); TurretController controller(Config()); assert(controller.Initialize());
        controller.SetNetworkAvailable(true);
        input_levels[11] = 0;
        assert(controller.Request(1) && controller.BeginPhysicalConfirmation());
        if (failure == 0) fail_duty_channel = 4;
        if (failure == 1) fail_update_channel = 4;
        if (failure == 2) { fail_gpio_pin = 8; fail_gpio_once = true; }
        if (failure < 3) assert(!controller.ConfirmPhysical());
        else {
            assert(controller.ConfirmPhysical());
            fail_gpio_pin = 8; fail_gpio_once = true;
            assert(!controller.Disarm("stop output failure"));
        }
        AllStopped(); Tick(50000); AllStopped();
        assert(!controller.Request(1) && controller.GetStatus().find("fault") != std::string::npos);
    }
#if CONFIG_CAR_TURRET_ENABLED
    ResetRuntime();
    {
        Application app; app.SetMotorNetworkAvailable(true);
        assert(app.HandleMotorActionWithDuration(4, 80, 1000));
        auto& turret = TurretController::GetInstance(); assert(turret.Initialize());
        Start(turret, 3); assert(duties[8] == 1 && duties[0] > 0);
        assert(app.HandleMotorActionWithDuration(0, 0, 0)); AllStopped();
        Tick(10000); AllStopped();
        Start(turret); app.SetMotorNetworkAvailable(false); AllStopped();
        app.SetMotorNetworkAvailable(true); Tick(20000); AllStopped();
        assert(!turret.ConfirmPhysical());
    }
#endif
    std::cout << "PASS: production turret adapter, build mode=" << CONFIG_CAR_TURRET_ENABLED
        << ", physical confirmation, PWM isolation, stop/disconnect, disabled I/O, injected driver/task failures\n";
}
