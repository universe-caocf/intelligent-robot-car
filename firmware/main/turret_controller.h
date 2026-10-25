// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "turret_control.h"
#include <mutex>
#include <string>

namespace car {
struct TurretHardwareConfig {
    bool enabled = false;
    int flywheel_gpio = -1, push_gpio = -1, confirm_gpio = -1, tilt_gpio = -1;
    int rest_us = 1000, push_us = 1500;
    TurretTiming timing;
    static TurretHardwareConfig FromBuildConfig();
    bool IsValid() const;
};

class TurretController {
public:
    explicit TurretController(TurretHardwareConfig config = TurretHardwareConfig::FromBuildConfig())
        : config_(config), control_(config.enabled, config.timing) {}
    static TurretController& GetInstance() { static TurretController controller; return controller; }
    bool Initialize();
    bool Request(int count);
    // 只由独立实体按钮回调调用；MCP 不暴露确认入口。按下必须晚于申请。
    bool BeginPhysicalConfirmation();
    bool ConfirmPhysical();
    bool Disarm(const char* reason);
    void SetNetworkAvailable(bool available);
    bool SetTilt(int angle);
    std::string GetStatus();
private:
    void Poll();
    bool ApplyOutputLocked();
    void FailLocked(const char* reason);
    void ForceOffLocked();
    TurretHardwareConfig config_;
    TurretControl control_;
    std::mutex mutex_;
    void* task_handle_ = nullptr;
    bool attempted_ = false, initialized_ = false, network_available_ = false;
    bool flywheel_touched_ = false, push_touched_ = false, tilt_touched_ = false;
    bool physical_press_eligible_ = false;
    int tilt_pulse_us_ = 0;
    int64_t tilt_deadline_ms_ = 0;
    std::string reason_ = "startup";
};
}  // namespace car
