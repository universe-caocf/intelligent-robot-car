// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace car {
enum class TurretState { Disabled, Disarmed, AwaitingConfirmation, Spinning, Pushing, Recovering, Fault };

struct TurretTiming {
    int spin_ms = 700;
    int push_ms = 250;
    int recover_ms = 450;
    bool IsValid() const {
        return spin_ms >= 100 && spin_ms <= 3000 && push_ms >= 100 && push_ms <= 1000 &&
            recover_ms >= 100 && recover_ms <= 1000;
    }
};

// 纯状态机：云端只能申请；独立实体按钮确认后，才允许有限次推弹。
class TurretControl {
public:
    explicit TurretControl(bool enabled = false, TurretTiming timing = {}) : timing_(timing) {
        state_ = !enabled ? TurretState::Disabled : timing.IsValid() ? TurretState::Disarmed : TurretState::Fault;
    }
    bool Request(int count, int64_t now_ms) {
        Poll(now_ms);
        if (state_ != TurretState::Disarmed || count < 1 || count > 3) return false;
        requested_ = count; completed_ = 0;
        state_ = TurretState::AwaitingConfirmation;
        deadline_ms_ = now_ms + kConfirmationMs;
        return true;
    }
    bool Confirm(int64_t now_ms) {
        Poll(now_ms);
        if (state_ != TurretState::AwaitingConfirmation) return false;
        state_ = TurretState::Spinning;
        deadline_ms_ = now_ms + timing_.spin_ms;
        operation_deadline_ms_ = now_ms + timing_.spin_ms + requested_ * (timing_.push_ms + timing_.recover_ms) + 200;
        return true;
    }
    bool Poll(int64_t now_ms) {
        if (state_ == TurretState::AwaitingConfirmation && now_ms >= deadline_ms_) { Disarm(); return true; }
        if (!busy()) return false;
        if (now_ms > operation_deadline_ms_) { Disarm(); return true; }
        if (now_ms < deadline_ms_) return false;
        // 严重调度迟到时直接取消，不补跑推弹；设备适配层的查询不调用此方法。
        if (now_ms - deadline_ms_ > 200) { Disarm(); return true; }
        if (state_ == TurretState::Spinning) {
            state_ = TurretState::Pushing; deadline_ms_ = now_ms + timing_.push_ms;
        } else if (state_ == TurretState::Pushing) {
            state_ = TurretState::Recovering; deadline_ms_ = now_ms + timing_.recover_ms;
        } else {
            ++completed_;
            if (completed_ >= requested_) Disarm();
            else { state_ = TurretState::Pushing; deadline_ms_ = now_ms + timing_.push_ms; }
        }
        return true;
    }
    void Disarm() {
        if (state_ != TurretState::Disabled && state_ != TurretState::Fault) state_ = TurretState::Disarmed;
        requested_ = 0; deadline_ms_ = operation_deadline_ms_ = 0;
    }
    void Fault() { Disarm(); state_ = TurretState::Fault; }
    bool busy() const { return state_ == TurretState::Spinning || state_ == TurretState::Pushing || state_ == TurretState::Recovering; }
    bool flywheel() const { return busy(); }
    bool pushing() const { return state_ == TurretState::Pushing; }
    TurretState state() const { return state_; }
    int requested() const { return requested_; }
    int completed_cycles() const { return completed_; }
    int64_t deadline_ms() const { return deadline_ms_; }
    static constexpr int kConfirmationMs = 10000;
    static const char* StateName(TurretState state) {
        switch (state) {
            case TurretState::Disabled: return "disabled";
            case TurretState::Disarmed: return "disarmed";
            case TurretState::AwaitingConfirmation: return "awaiting_confirmation";
            case TurretState::Spinning: return "spinning";
            case TurretState::Pushing: return "pushing";
            case TurretState::Recovering: return "recovering";
            case TurretState::Fault: return "fault";
        }
        return "fault";
    }
private:
    TurretTiming timing_;
    TurretState state_;
    int requested_ = 0, completed_ = 0;
    int64_t deadline_ms_ = 0, operation_deadline_ms_ = 0;
};
}  // namespace car
