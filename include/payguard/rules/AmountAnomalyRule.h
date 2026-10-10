#pragma once

#include <cstdint>

#include "payguard/rules/FraudRule.h"

namespace payguard {

// F2: amount above mean + k * stddev of the card's approved spending, once the
// card has at least `minHistory` approved transactions. Statistics come from
// Welford's running moments, so no history is stored.
//
// Scale::Log applies the test to log(amount). Card spending is close to
// log-normal (long right tail), so a linear z-score fires on a few percent of
// perfectly normal purchases; in log space the same k gives the false-positive
// rate the k suggests.
class AmountAnomalyRule : public FraudRule {
public:
    enum class Scale { Linear, Log };

    AmountAnomalyRule(std::uint64_t minHistory, double stdDevs, double score,
                      Scale scale = Scale::Log);

    RuleResult evaluate(const Transaction& txn, const CardProfile& profile) const override;
    std::string name() const override { return "AMOUNT_ANOMALY"; }

private:
    std::uint64_t minHistory_;
    double stdDevs_;
    double score_;
    Scale scale_;
};

}  // namespace payguard
