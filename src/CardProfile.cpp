#include "payguard/CardProfile.h"

#include <cmath>

namespace payguard {

void CardProfile::recordAttempt(std::int64_t timestamp, Money amount) {
    recent_.push_back({timestamp, amount});
    const std::int64_t horizon = timestamp - kRetentionSeconds;
    while (!recent_.empty() &&
           (recent_.front().timestamp < horizon || recent_.size() > kMaxRecentAttempts)) {
        recent_.pop_front();
    }
}

void CardProfile::recordApprovedAmount(Money amount) {
    if (amount <= 0) {
        return;
    }
    const auto x = static_cast<double>(amount);
    amountStats_.add(x);
    logAmountStats_.add(std::log(x));
}

void CardProfile::recordLocation(const std::string& city, std::int64_t timestamp) {
    hasLocation_ = true;
    if (lastCity_ != city) {
        lastCity_ = city;
    }
    lastTimestamp_ = timestamp;
}

std::size_t CardProfile::countAttemptsSince(std::int64_t since, std::size_t limit) const {
    std::size_t count = 0;
    for (auto it = recent_.rbegin(); it != recent_.rend() && count < limit; ++it) {
        if (it->timestamp < since) {
            break;
        }
        ++count;
    }
    return count;
}

}  // namespace payguard
