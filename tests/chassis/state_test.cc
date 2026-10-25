// SPDX-License-Identifier: GPL-3.0-or-later
#include "motor_control.h"
#include <cassert>
#include <iostream>

int main() {
    car::MotorControl motor;
    const car::MotorStep forward{4, 80, 1000}, backward{2, 60, 1500};
    assert(motor.Start(&forward, 1, 0));
    assert(!motor.Poll(999) && motor.direction() == 4);
    assert(motor.Poll(1000) && motor.direction() == 0);
    // 新指令覆盖旧截止时间，旧动作不能在 1000ms 时误停新动作。
    assert(motor.Start(&forward, 1, 0));
    assert(motor.Start(&backward, 1, 400));
    assert(!motor.Poll(1000) && motor.direction() == 2);
    assert(motor.Poll(1900) && motor.direction() == 0);
    const car::MotorStep sequence[] = {{4, 50, 200}, {3, 50, 200}, {2, 50, 200}};
    assert(motor.Start(sequence, 3, 0) && motor.pending_steps() == 2);
    assert(motor.Poll(200) && motor.direction() == 3);
    motor.Stop();
    assert(!motor.Poll(400) && motor.pending_steps() == 0 && motor.direction() == 0);
    assert(!motor.Poll(10000));
    assert(motor.Start(sequence, 3, 0));
    assert(motor.Start(&backward, 1, 100));
    assert(!motor.Poll(400) && motor.direction() == 2 && motor.pending_steps() == 0);
    // 相同网页心跳续期；租约只作用于网页持续遥控。
    assert(motor.WebCommand(4, 80, 0));
    assert(motor.WebCommand(4, 80, 600));
    assert(!motor.Poll(750) && motor.direction() == 4);
    assert(!motor.Poll(1349));
    assert(motor.Poll(1350) && motor.direction() == 0);
    assert(motor.WebCommand(3, 50, 0));
    assert(motor.Start(&backward, 1, 100));
    assert(!motor.Poll(750) && motor.source() == car::MotorSource::Timed);
    assert(motor.WebCommand(4, 80, 800));
    assert(!motor.Poll(1000));
    assert(motor.Poll(1550));
    // 非法输入不覆盖正在运行的有效动作。
    assert(motor.Start(&forward, 1, 0));
    for (car::MotorStep invalid : {car::MotorStep{-1, 80, 1000}, {5, 80, 1000},
            {4, -1, 1000}, {4, 101, 1000}, {4, 80, 0}, {4, 80, 99}, {4, 80, 10001}}) {
        assert(!motor.Start(&invalid, 1, 100));
        assert(motor.direction() == 4 && motor.deadline_ms() == 1000);
    }
    assert(!motor.Start(nullptr, 1, 0));
    assert(!motor.Start(sequence, 0, 0));
    car::MotorStep too_many[9]{};
    assert(!motor.Start(too_many, 9, 0));
    const car::MotorStep too_long[] = {{4, 80, 6000}, {2, 80, 6000}};
    assert(!motor.Start(too_long, 2, 0));
    assert(!motor.WebCommand(4, 101, 0) && motor.direction() == 4);
    assert(motor.Start(sequence, 3, 0));
    assert(motor.Poll(450) && motor.direction() == 2);
    assert(motor.Poll(5000) && motor.direction() == 0);
    assert(motor.Start(sequence, 3, 0));
    assert(motor.Poll(5000) && motor.direction() == 0); // 不能补跑过期动作
    const int64_t large_clock = int64_t{1} << 40;
    assert(motor.WebCommand(4, 100, large_clock));
    assert(!motor.Poll(large_clock + 749));
    assert(motor.Poll(large_clock + 750));
    assert(motor.WebCommand(4, 80, 0));
    assert(motor.WebCommand(0, 80, 1) && motor.direction() == 0);
    assert(motor.WebCommand(4, 80, 0));
    assert(motor.WebCommand(4, 0, 1) && motor.source() == car::MotorSource::None);
    car::MotorActionConfig config;
    assert(config.IsValid() && config.default_speed_percent == 80);
    assert(config.forward_duration_ms == 1500 && config.quick_forward_duration_ms == 500);
    config.forward_duration_ms = -1;
    assert(!config.IsValid());
    config = {};
    config.default_speed_percent = 101;
    assert(!config.IsValid());
    config.default_speed_percent = 0;
    assert(config.IsValid());
    std::cout << "PASS: replacement, sequence cancel, lease renewal, takeover, invalid input, delayed scheduling, clock width, defaults\n";
}
