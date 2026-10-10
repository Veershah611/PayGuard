#include <cstdio>

#include "payguard/AlertListener.h"

namespace payguard {

void ConsoleAlertListener::onAlert(const Transaction& txn, const Decision& decision) {
    if (!decision.isFraudAlert()) {
        return;  // hard-check declines are routine, not alerts
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (printed_ >= limit_) {
        ++suppressed_;
        return;
    }
    ++printed_;
    char score[16];
    std::snprintf(score, sizeof(score), "%.0f", decision.riskScore);
    out_ << "[ALERT] " << toString(decision.verdict) << ' ' << txn.id << " card=" << txn.cardId
         << " amount=" << formatMoney(txn.amount) << " city=" << txn.city << " score=" << score
         << " rules=";
    for (std::size_t i = 0; i < decision.reasons.size(); ++i) {
        out_ << (i == 0 ? "" : ",") << decision.reasons[i];
    }
    out_ << '\n';
}

std::size_t ConsoleAlertListener::suppressed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return suppressed_;
}

}  // namespace payguard
