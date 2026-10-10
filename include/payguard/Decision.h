#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace payguard {

enum class Verdict : std::uint8_t { Approve, Flag, Decline };

enum class DeclineReason : std::uint8_t {
    None,
    UnknownCard,         // A1
    CardBlocked,         // A2
    InvalidAmount,       // A3
    UnknownCity,         // transaction city missing from the geo table
    InsufficientFunds,   // A4
    DailyLimitExceeded,  // A5
    FraudSuspected,      // risk score >= decline threshold
};

constexpr std::size_t kDeclineReasonCount = 8;

constexpr std::string_view toString(Verdict verdict) {
    switch (verdict) {
        case Verdict::Approve: return "APPROVE";
        case Verdict::Flag: return "FLAG";
        case Verdict::Decline: return "DECLINE";
    }
    return "UNKNOWN";
}

constexpr std::string_view toString(DeclineReason reason) {
    switch (reason) {
        case DeclineReason::None: return "NONE";
        case DeclineReason::UnknownCard: return "UNKNOWN_CARD";
        case DeclineReason::CardBlocked: return "CARD_BLOCKED";
        case DeclineReason::InvalidAmount: return "INVALID_AMOUNT";
        case DeclineReason::UnknownCity: return "UNKNOWN_CITY";
        case DeclineReason::InsufficientFunds: return "INSUFFICIENT_FUNDS";
        case DeclineReason::DailyLimitExceeded: return "DAILY_LIMIT_EXCEEDED";
        case DeclineReason::FraudSuspected: return "FRAUD_SUSPECTED";
    }
    return "UNKNOWN";
}

struct Decision {
    Verdict verdict = Verdict::Approve;
    DeclineReason reason = DeclineReason::None;
    double riskScore = 0.0;            // 0–100, sum of triggered rule scores (clamped)
    std::uint32_t ruleHits = 0;        // bit i set => engine rule i triggered
    std::vector<std::string> reasons;  // names of triggered rules, in evaluation order

    bool approved() const { return verdict != Verdict::Decline; }
    // True when the fraud rules (not a hard check) raised this transaction.
    bool isFraudAlert() const {
        return verdict == Verdict::Flag || reason == DeclineReason::FraudSuspected;
    }
};

}  // namespace payguard
