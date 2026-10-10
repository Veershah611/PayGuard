#include "payguard/rules/BlocklistRule.h"

#include <stdexcept>

namespace payguard {

BlocklistRule::BlocklistRule(
    std::shared_ptr<const std::unordered_set<std::string>> blockedMerchants, double score)
    : blocked_(std::move(blockedMerchants)), score_(score) {
    if (!blocked_) {
        throw std::invalid_argument("blocked_merchant: a blocklist is required");
    }
}

RuleResult BlocklistRule::evaluate(const Transaction& txn, const CardProfile& /*profile*/) const {
    if (blocked_->count(txn.merchantId) != 0) {
        return RuleResult::hit(score_, name());
    }
    return RuleResult::pass();
}

}  // namespace payguard
