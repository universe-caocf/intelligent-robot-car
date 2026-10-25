// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace car {
constexpr int kDefaultSpeed = 80;
constexpr int kMinDurationMs = 100;
constexpr int kMaxDurationMs = 10000;
constexpr int kWebLeaseMs = 750;
constexpr int kMotorPollMs = 20;

// 这里只定义软件初值；速度、角度和距离必须在实车上标定。
struct MotorActionConfig {
    int forward_duration_ms = 1500;
    int backward_duration_ms = 1500;
    int left_turn_duration_ms = 600;
    int right_turn_duration_ms = 600;
    int spin_duration_ms = 2500;
    int wiggle_duration_ms = 600;
    int dance_duration_ms = 1500;
    int quick_forward_duration_ms = 500;
    int quick_backward_duration_ms = 500;
    int default_speed_percent = kDefaultSpeed;

    bool IsValid() const {
        for (int duration : {forward_duration_ms, backward_duration_ms,
                left_turn_duration_ms, right_turn_duration_ms, spin_duration_ms,
                wiggle_duration_ms, dance_duration_ms, quick_forward_duration_ms,
                quick_backward_duration_ms}) {
            if (duration < kMinDurationMs || duration > kMaxDurationMs) return false;
        }
        return default_speed_percent >= 0 && default_speed_percent <= 100;
    }
};

struct MotorStep {
    int direction;  // 0 停止，1 右转，2 后退，3 左转，4 前进
    int speed;
    int duration_ms;
};

enum class MotorSource { None, Timed, Web };

// 与 ESP-IDF 无关的状态机。调用方用同一把锁保护状态变更和电机输出。
// 没有每条指令独立的延迟任务；新动作和停止直接替换整个序列。
class MotorControl {
public:
    bool Start(const MotorStep* steps, size_t count, int64_t now_ms) {
        if (steps == nullptr || count == 0 || count > steps_.size()) return false;
        int total = 0;
        for (size_t i = 0; i < count; ++i) {
            const auto& step = steps[i];
            if (!ValidCommand(step.direction, step.speed) ||
                    step.duration_ms < kMinDurationMs || step.duration_ms > kMaxDurationMs) return false;
            total += step.duration_ms;
        }
        if (total > kMaxDurationMs) return false;
        std::copy_n(steps, count, steps_.begin());
        count_ = count;
        index_ = 0;
        source_ = MotorSource::Timed;
        deadline_ms_ = now_ms + steps_[0].duration_ms;
        SetOutput(steps_[0]);
        return true;
    }

    bool WebCommand(int direction, int speed, int64_t now_ms) {
        if (!ValidCommand(direction, speed)) return false;
        if (direction == 0 || speed == 0) { Stop(); return true; }
        // 相同心跳也续期；定时语音动作不使用这个租约。
        source_ = MotorSource::Web;
        count_ = 0;
        index_ = 0;
        direction_ = direction;
        speed_ = speed;
        deadline_ms_ = now_ms + kWebLeaseMs;
        return true;
    }

    bool Poll(int64_t now_ms) {
        if (source_ == MotorSource::None || now_ms < deadline_ms_) return false;
        if (source_ == MotorSource::Timed) {
            // 调度延迟时跳过已经过期的步骤，不能补跑旧动作。
            while (++index_ < count_) {
                deadline_ms_ += steps_[index_].duration_ms;
                if (now_ms < deadline_ms_) { SetOutput(steps_[index_]); return true; }
            }
        }
        Stop();
        return true;
    }

    void Stop() {
        source_ = MotorSource::None;
        count_ = index_ = 0;
        direction_ = speed_ = 0;
        deadline_ms_ = 0;
    }

    static bool ValidCommand(int direction, int speed) {
        return direction >= 0 && direction <= 4 && speed >= 0 && speed <= 100;
    }
    int direction() const { return direction_; }
    int speed() const { return speed_; }
    MotorSource source() const { return source_; }
    int64_t deadline_ms() const { return deadline_ms_; }
    size_t pending_steps() const { return count_ > index_ ? count_ - index_ - 1 : 0; }

private:
    void SetOutput(const MotorStep& step) {
        direction_ = step.speed == 0 ? 0 : step.direction;
        speed_ = direction_ == 0 ? 0 : step.speed;
    }
    std::array<MotorStep, 8> steps_{};
    size_t count_ = 0;
    size_t index_ = 0;
    MotorSource source_ = MotorSource::None;
    int direction_ = 0;
    int speed_ = 0;
    int64_t deadline_ms_ = 0;
};
}  // namespace car
