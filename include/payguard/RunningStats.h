#pragma once

#include <cmath>
#include <cstdint>

namespace payguard {

// Welford's online algorithm: running mean and variance in O(1) time and
// memory per sample, numerically stable (no sum-of-squares cancellation).
class RunningStats {
public:
    void add(double x) {
        ++count_;
        const double delta = x - mean_;
        mean_ += delta / static_cast<double>(count_);
        m2_ += delta * (x - mean_);
    }

    std::uint64_t count() const { return count_; }
    double mean() const { return mean_; }

    // Sample variance (n - 1 denominator); 0 until there are two samples.
    double variance() const {
        return count_ < 2 ? 0.0 : m2_ / static_cast<double>(count_ - 1);
    }
    double stddev() const { return std::sqrt(variance()); }

private:
    std::uint64_t count_ = 0;
    double mean_ = 0.0;
    double m2_ = 0.0;  // sum of squared deviations from the running mean
};

}  // namespace payguard
