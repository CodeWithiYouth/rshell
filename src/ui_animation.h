#pragma once

#include <cstdint>

namespace rshell::ui {

// A shared, allocation-free time line for small UI transitions. Times are in ms.
class Tween {
public:
    explicit constexpr Tween(float initial = 0.0f) noexcept
        : from_(initial), target_(initial) {}

    float value(std::uint64_t now) const noexcept {
        if (!duration_ || from_ == target_) return target_;
        if (now <= started_) return from_;
        const std::uint64_t elapsed = now - started_;
        if (elapsed >= duration_) return target_;
        const float remaining =
            1.0f - static_cast<float>(elapsed) / static_cast<float>(duration_);
        return target_ + (from_ - target_) * remaining * remaining * remaining;
    }

    bool active(std::uint64_t now) const noexcept {
        return duration_ && from_ != target_ &&
               (now < started_ || now - started_ < duration_);
    }

    void to(float target, std::uint64_t now, unsigned duration) noexcept {
        if (!duration) {
            snap(target);
            return;
        }
        // Repeated mouse/paint events must not keep restarting the same target.
        if (target == target_) return;
        from_ = value(now);
        target_ = target;
        started_ = now;
        duration_ = duration;
    }

    void snap(float target) noexcept {
        from_ = target_ = target;
        duration_ = 0;
    }

    void finish() noexcept { snap(target_); }

private:
    float from_;
    float target_;
    std::uint64_t started_ = 0;
    unsigned duration_ = 0;
};

}  // namespace rshell::ui
