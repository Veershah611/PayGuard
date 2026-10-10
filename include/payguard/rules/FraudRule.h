#pragma once

#include <string>
#include <utility>

#include "payguard/CardProfile.h"
#include "payguard/Transaction.h"

namespace payguard {

struct RuleResult {
    bool triggered = false;
    double score = 0.0;  // contribution to the 0–100 risk score
    std::string reason;  // rule name when triggered, empty otherwise

    static RuleResult pass() { return {}; }
    static RuleResult hit(double score, std::string reason) {
        return {true, score, std::move(reason)};
    }
};

// Strategy interface for soft fraud rules. The engine only ever sees this
// interface, so new rules plug in without changing it (open/closed).
//
// evaluate() is called before the current transaction is recorded in the
// profile, and must be safe to call concurrently from several threads: rules
// hold configuration only, all per-card state lives in CardProfile.
class FraudRule {
public:
    // Virtual so deleting a concrete rule through FraudRule* is well defined.
    virtual ~FraudRule() = default;

    virtual RuleResult evaluate(const Transaction& txn, const CardProfile& profile) const = 0;
    virtual std::string name() const = 0;
};

}  // namespace payguard
