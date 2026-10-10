#include "payguard/AuthorizationEngine.h"

#include <algorithm>
#include <cassert>
#include <stdexcept>

namespace payguard {

namespace {

Decision declined(DeclineReason reason) {
    Decision decision;
    decision.verdict = Verdict::Decline;
    decision.reason = reason;
    return decision;
}

DeclineReason toDeclineReason(FundsCheck check) {
    return check == FundsCheck::InsufficientFunds ? DeclineReason::InsufficientFunds
                                                  : DeclineReason::DailyLimitExceeded;
}

}  // namespace

AuthorizationEngine::AuthorizationEngine(AccountStore& accounts,
                                         std::vector<std::unique_ptr<FraudRule>> rules,
                                         EngineOptions options,
                                         std::shared_ptr<const CityTable> cities)
    : accounts_(accounts),
      rules_(std::move(rules)),
      options_(options),
      cities_(std::move(cities)) {
    if (rules_.size() > kMaxRules) {
        throw std::invalid_argument("at most " + std::to_string(kMaxRules) +
                                    " fraud rules are supported");
    }
    for (const auto& rule : rules_) {
        if (!rule) {
            throw std::invalid_argument("null fraud rule");
        }
    }
}

std::vector<std::string> AuthorizationEngine::ruleNames() const {
    std::vector<std::string> names;
    names.reserve(rules_.size());
    for (const auto& rule : rules_) {
        names.push_back(rule->name());
    }
    return names;
}

Decision AuthorizationEngine::process(const Transaction& txn) {
    // A1: card exists. The map is read-only during processing, so no lock.
    Account* account = accounts_.find(txn.cardId);
    if (account == nullptr) {
        return declined(DeclineReason::UnknownCard);
    }

    auto locked = account->lock();

    // A2: card is active.
    if (locked.status() != CardStatus::Active) {
        return declined(DeclineReason::CardBlocked);
    }
    // A3: amount is positive and valid (the loader maps bad amounts to 0).
    if (txn.amount <= 0) {
        return declined(DeclineReason::InvalidAmount);
    }
    if (options_.declineUnknownCity && cities_ && !cities_->empty() &&
        !cities_->contains(txn.city)) {
        return declined(DeclineReason::UnknownCity);
    }

    CardProfile& profile = locked.profile();

    // A4 / A5: funds and daily limit. Failed attempts still count towards
    // velocity, since card testing usually shows up as a run of declines.
    const FundsCheck funds = locked.checkFunds(txn.amount, txn.timestamp);
    if (funds != FundsCheck::Ok) {
        profile.recordAttempt(txn.timestamp, txn.amount);
        return declined(toDeclineReason(funds));
    }

    // Soft rules see the profile as it was before this transaction.
    Decision decision;
    double score = 0.0;
    for (std::size_t i = 0; i < rules_.size(); ++i) {
        RuleResult result = rules_[i]->evaluate(txn, profile);
        if (result.triggered) {
            score += result.score;
            decision.ruleHits |= 1u << i;
            decision.reasons.push_back(std::move(result.reason));
        }
    }
    decision.riskScore = std::clamp(score, 0.0, 100.0);
    profile.recordAttempt(txn.timestamp, txn.amount);

    if (decision.riskScore >= options_.thresholds.decline) {
        decision.verdict = Verdict::Decline;
        decision.reason = DeclineReason::FraudSuspected;
        return decision;
    }
    decision.verdict =
        decision.riskScore >= options_.thresholds.flag ? Verdict::Flag : Verdict::Approve;

    // Same lock as the funds check above, so this cannot fail.
    [[maybe_unused]] const FundsCheck debited = locked.debit(txn.amount, txn.timestamp);
    assert(debited == FundsCheck::Ok);

    // Only unsuspicious approvals update the card's baseline. If a flagged
    // charge in London moved the "last location", the cardholder's next real
    // purchase at home would look geo-impossible too.
    if (decision.verdict == Verdict::Approve) {
        profile.recordLocation(txn.city, txn.timestamp);
        profile.recordApprovedAmount(txn.amount);
    }
    return decision;
}

}  // namespace payguard
