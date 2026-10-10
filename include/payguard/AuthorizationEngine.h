#pragma once

#include <memory>
#include <string>
#include <vector>

#include "payguard/AccountStore.h"
#include "payguard/CityTable.h"
#include "payguard/Config.h"
#include "payguard/Decision.h"
#include "payguard/Transaction.h"
#include "payguard/rules/FraudRule.h"

namespace payguard {

struct EngineOptions {
    DecisionThresholds thresholds;
    bool declineUnknownCity = true;  // only applies when a city table is supplied
};

// Decides APPROVE / FLAG / DECLINE for one transaction:
//   1. hard checks A1–A5 (any failure -> DECLINE with that reason)
//   2. every enabled fraud rule, scores summed into a 0–100 risk score
//   3. thresholds -> verdict; APPROVE and FLAG debit the account
//
// Steps 1–3 run under the card's account lock, so the funds check and the
// debit are one atomic step and the card's fraud profile is never read
// half-updated. process() is safe to call from many threads at once.
class AuthorizationEngine {
public:
    static constexpr std::size_t kMaxRules = 32;  // Decision::ruleHits is a 32-bit mask

    AuthorizationEngine(AccountStore& accounts, std::vector<std::unique_ptr<FraudRule>> rules,
                        EngineOptions options = {},
                        std::shared_ptr<const CityTable> cities = nullptr);

    Decision process(const Transaction& txn);

    std::size_t ruleCount() const { return rules_.size(); }
    std::vector<std::string> ruleNames() const;
    const EngineOptions& options() const { return options_; }

private:
    AccountStore& accounts_;
    std::vector<std::unique_ptr<FraudRule>> rules_;
    EngineOptions options_;
    std::shared_ptr<const CityTable> cities_;
};

}  // namespace payguard
