// SPDX-License-Identifier: GPL-3.0-or-later
#include "turret_control.h"
#include <cassert>
#include <iostream>
using namespace car;
int main() {
    TurretControl disabled;
    assert(!disabled.Request(1, 0) && !disabled.Confirm(0)); disabled.Disarm();
    assert(disabled.state() == TurretState::Disabled);
    TurretControl bad(true, {0, 250, 450}); assert(bad.state() == TurretState::Fault);
    for (int count = 1; count <= 3; ++count) {
        TurretControl control(true);
        assert(!control.Confirm(0) && !control.Request(0, 0) && !control.Request(4, 0));
        assert(control.Request(count, 0) && !control.flywheel());
        assert(!control.Request(1, 1)); // 不替换、不续期待确认申请
        assert(control.Confirm(100) && control.flywheel() && !control.pushing());
        assert(!control.Request(1, 200) && !control.Confirm(200));
        assert(!control.Poll(799));
        int64_t now = 800;
        assert(control.Poll(now) && control.pushing());
        for (int i = 0; i < count; ++i) {
            assert(control.pushing());
            now += 250; assert(control.Poll(now) && control.state() == TurretState::Recovering);
            now += 450; assert(control.Poll(now));
            assert(control.completed_cycles() == i + 1);
        }
        assert(control.state() == TurretState::Disarmed && !control.flywheel());
        assert(control.completed_cycles() == count && !control.Poll(now + 10000));
    }
    for (int phase = 0; phase < 4; ++phase) {
        TurretControl control(true); assert(control.Request(3, 0));
        if (phase > 0) assert(control.Confirm(0));
        if (phase > 1) assert(control.Poll(700));
        if (phase > 2) assert(control.Poll(950));
        control.Disarm();
        assert(!control.Poll(20000) && !control.Confirm(20000) && !control.flywheel());
    }
    TurretControl expired(true); assert(expired.Request(1, 0));
    assert(!expired.Confirm(10000) && expired.state() == TurretState::Disarmed);
    assert(expired.Request(1, 20000) && expired.Poll(30000));
    TurretControl late(true); assert(late.Request(3, 0) && late.Confirm(0));
    assert(late.Poll(901) && !late.flywheel() && late.completed_cycles() == 0);
    late.Fault(); late.Disarm(); assert(!late.Request(1, 1000) && late.state() == TurretState::Fault);
    TurretControl budget(true, {100, 1000, 1000});
    assert(budget.Request(1, 0) && budget.Confirm(0));
    assert(budget.Poll(300) && budget.Poll(1500));
    assert(budget.Poll(2301) && !budget.flywheel()); // 总时限独立于当前阶段截止时间
    std::cout << "PASS: turret state, confirmation expiry, bounded cycles, cancellation at every phase, scheduler delay, fault latch\n";
}
