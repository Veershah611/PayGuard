#include "payguard/rules/RoundAmountBurstRule.h"

#include <stdexcept>
#include <string>

namespace payguard {

RoundAmountBurstRule::RoundAmountBurstRule(int minCount, std::int64_t windowSeconds,
                                           Money multiple, double score)
    : minCount_(minCount), windowSeconds_(windowSeconds), multiple_(multiple), score_(score) {
    if (minCount < 2 || static_cast<std::size_t>(minCount) > CardProfile::kMaxRecentAttempts) {
        throw std::invalid_argument("round_amount_burst: min_count must be between 2 and " +
                                    std::to_string(CardProfile::kMaxRecentAttempts));
    }
    if (windowSeconds < 1 || windowSeconds > CardProfile::kRetentionSeconds) {
        throw std::invalid_argument("round_amount_burst: window_seconds must be between 1 and " +
                                    std::to_string(CardProfile::kRetentionSeconds));
    }
    if (multiple <= 0) {
        throw std::invalid_argument("round_amount_burst: multiple must be positive");
    }
}

RuleResult RoundAmountBurstRule::evaluate(const Transaction& txn,
                                          const CardProfile& profile) const {
    if (!isRound(txn.amount)) {
        return RuleResult::pass();
    }
    const std::int64_t since = txn.timestamp - windowSeconds_ + 1;
    int count = 1;  // the current transaction
    const auto& recent = profile.recentAttempts();
    for (auto it = recent.rbegin(); it != recent.rend() && it->timestamp >= since; ++it) {
        if (isRound(it->amount) && ++count >= minCount_) {
            return RuleResult::hit(score_, name());
        }
    }
    return RuleResult::pass();
}

}  // namespace payguard
