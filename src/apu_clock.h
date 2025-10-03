// apu_clock.h
#pragma once

// Small fractional accumulator for resampling CPU->audio sample rate
struct ClockFrac {
    ClockFrac() = default;
    explicit ClockFrac(double initial) : acc(initial) {}

    [[nodiscard]] inline int step(double inc) {
        acc += inc;
        int n = static_cast<int>(acc);
        acc -= n;
        return n;
    }

    double get_acc() const { return acc; }

private:
    double acc = 0.0;
};
