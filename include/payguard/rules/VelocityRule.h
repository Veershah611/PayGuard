#pragma once

#include <cstdint>

#include "payguard/rules/FraudRule.h"

namespace payguard {

// F1: more than `maxTxns` attempts on one card within `windowSeconds`
// (counting the current one). Uses the profile's sliding window; the scan
// stops after maxTxns entries, so cost is bounded regardless of history.
class VelocityRule : public FraudRule {
public:
    VelocityRule(int maxTxns, std::int64_t windowSeconds, double score);

    RuleResult evaluate(const Transaction& txn, const CardProfile& profile) const override;
    std::string name() const override { return "VELOCITY"; }

private:
    int maxTxns_;
    std::int64_t windowSeconds_;
    double score_;
};

}  // namespace payguard
