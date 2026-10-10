#pragma once

#include <cstdint>

#include "payguard/Money.h"
#include "payguard/rules/FraudRule.h"

namespace payguard {

// F5: at least `minCount` amounts that are exact multiples of `multiple`
// within `windowSeconds` (counting the current one), a common card-testing
// pattern.
class RoundAmountBurstRule : public FraudRule {
public:
    RoundAmountBurstRule(int minCount, std::int64_t windowSeconds, Money multiple, double score);

    RuleResult evaluate(const Transaction& txn, const CardProfile& profile) const override;
    std::string name() const override { return "ROUND_AMOUNT_BURST"; }

private:
    bool isRound(Money amount) const { return amount > 0 && amount % multiple_ == 0; }

    int minCount_;
    std::int64_t windowSeconds_;
    Money multiple_;
    double score_;
};

}  // namespace payguard
