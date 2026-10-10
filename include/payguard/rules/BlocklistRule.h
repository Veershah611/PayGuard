#pragma once

#include <memory>
#include <string>
#include <unordered_set>

#include "payguard/rules/FraudRule.h"

namespace payguard {

// F4: merchant is on the blocklist (exact match, O(1) hash-set lookup).
class BlocklistRule : public FraudRule {
public:
    BlocklistRule(std::shared_ptr<const std::unordered_set<std::string>> blockedMerchants,
                  double score);

    RuleResult evaluate(const Transaction& txn, const CardProfile& profile) const override;
    std::string name() const override { return "BLOCKED_MERCHANT"; }

private:
    std::shared_ptr<const std::unordered_set<std::string>> blocked_;
    double score_;
};

}  // namespace payguard
