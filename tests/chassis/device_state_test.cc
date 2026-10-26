// SPDX-License-Identifier: GPL-3.0-or-later
#include "device_state_machine.h"
#include <cassert>
#include <iostream>
int main() {
    DeviceStateMachine state;
    assert(state.TransitionTo(kDeviceStateStarting));
    assert(state.TransitionTo(kDeviceStateActivating));
    // 模拟xTaskCreate失败；必须允许回到下一次网络事件可重试的状态。
    assert(state.TransitionTo(kDeviceStateStarting));
    assert(state.TransitionTo(kDeviceStateActivating));
    assert(state.TransitionTo(kDeviceStateIdle));
    assert(!state.TransitionTo(kDeviceStateStarting));
    assert(state.TransitionTo(kDeviceStateWifiConfiguring));
    assert(state.TransitionTo(kDeviceStateActivating));
    std::cout << "PASS: production activation state recovery and transition guards\n";
}
