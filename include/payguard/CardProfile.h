#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>

#include "payguard/Money.h"
#include "payguard/RunningStats.h"

namespace payguard {

// Per-card fraud state. Memory is bounded: the attempt window keeps at most
// kMaxRecentAttempts entries from the last kRetentionSeconds, and spending
// statistics are kept as running moments (Welford) instead of full history.
//
// Not thread-safe on its own; it lives inside an Account and is only touched
// while that account's lock is held.
class CardProfile {
public:
    static constexpr std::int64_t kRetentionSeconds = 3600;
    static constexpr std::size_t kMaxRecentAttempts = 256;

    struct Attempt {
        std::int64_t timestamp;
        Money amount;
    };

    // Every authorization attempt that reaches the fraud stage, approved or not
    // (card-testing bursts are mostly declines). Assumes per-card timestamps
    // arrive in non-decreasing order, which card sharding guarantees.
    void recordAttempt(std::int64_t timestamp, Money amount);

    // Amounts the engine approved without suspicion; flagged and declined
    // amounts are kept out so fraud cannot shift the card's baseline.
    void recordApprovedAmount(Money amount);

    // Where the card was last used for an unsuspicious, approved purchase.
    void recordLocation(const std::string& city, std::int64_t timestamp);

    // Most recent attempts, oldest first.
    const std::deque<Attempt>& recentAttempts() const { return recent_; }

    // Counts attempts with timestamp >= `since`, scanning newest-first and
    // stopping once `limit` is reached, so the cost is O(min(window, limit)).
    std::size_t countAttemptsSince(std::int64_t since, std::size_t limit) const;

    // Approved amounts in paise, and their natural logs. Spending is roughly
    // log-normal, so the log-space moments model "unusual" far better.
    const RunningStats& amountStats() const { return amountStats_; }
    const RunningStats& logAmountStats() const { return logAmountStats_; }

    bool hasLastLocation() const { return hasLocation_; }
    const std::string& lastCity() const { return lastCity_; }
    std::int64_t lastTimestamp() const { return lastTimestamp_; }

private:
    std::deque<Attempt> recent_;

    RunningStats amountStats_;
    RunningStats logAmountStats_;

    bool hasLocation_ = false;
    std::string lastCity_;
    std::int64_t lastTimestamp_ = 0;
};

}  // namespace payguard
