#include "payguard/rules/AmountAnomalyRule.h"

#include <cmath>
#include <stdexcept>

namespace payguard {

AmountAnomalyRule::AmountAnomalyRule(std::uint64_t minHistory, double stdDevs, double score,
                                     Scale scale)
    : minHistory_(minHistory), stdDevs_(stdDevs), score_(score), scale_(scale) {
    if (minHistory < 2) {
        throw std::invalid_argument("amount_anomaly: min_history must be at least 2");
    }
    if (stdDevs <= 0.0) {
        throw std::invalid_argument("amount_anomaly: std_devs must be positive");
    }
}

RuleResult AmountAnomalyRule::evaluate(const Transaction& txn, const CardProfile& profile) const {
    if (txn.amount <= 0) {
        return RuleResult::pass();
    }
    const RunningStats& stats =
        scale_ == Scale::Log ? profile.logAmountStats() : profile.amountStats();
    if (stats.count() < minHistory_) {
        return RuleResult::pass();
    }
    const double x = static_cast<double>(txn.amount);
    const double value = scale_ == Scale::Log ? std::log(x) : x;
    if (value > stats.mean() + stdDevs_ * stats.stddev()) {
        return RuleResult::hit(score_, name());
    }
    return RuleResult::pass();
}

}  // namespace payguard
