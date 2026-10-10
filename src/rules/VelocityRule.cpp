#include "payguard/rules/VelocityRule.h"

#include <stdexcept>

namespace payguard {

VelocityRule::VelocityRule(int maxTxns, std::int64_t windowSeconds, double score)
    : maxTxns_(maxTxns), windowSeconds_(windowSeconds), score_(score) {
    if (maxTxns < 1 || static_cast<std::size_t>(maxTxns) >= CardProfile::kMaxRecentAttempts) {
        throw std::invalid_argument("velocity: max_txns must be between 1 and " +
                                    std::to_string(CardProfile::kMaxRecentAttempts - 1));
    }
    if (windowSeconds < 1 || windowSeconds > CardProfile::kRetentionSeconds) {
        throw std::invalid_argument("velocity: window_seconds must be between 1 and " +
                                    std::to_string(CardProfile::kRetentionSeconds));
    }
}

RuleResult VelocityRule::evaluate(const Transaction& txn, const CardProfile& profile) const {
    // Window is (now - windowSeconds, now]. The current attempt is not yet in
    // the profile, so it triggers once maxTxns earlier attempts are in window.
    const std::int64_t since = txn.timestamp - windowSeconds_ + 1;
    const auto limit = static_cast<std::size_t>(maxTxns_);
    if (profile.countAttemptsSince(since, limit) >= limit) {
        return RuleResult::hit(score_, name());
    }
    return RuleResult::pass();
}

}  // namespace payguard
