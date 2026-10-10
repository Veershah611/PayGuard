#include "payguard/AlertQueue.h"

namespace payguard {

void AlertQueue::onAlert(const Transaction& txn, const Decision& decision) {
    if (decision.verdict != Verdict::Flag) {
        return;  // declines are already stopped; analysts review what went through
    }
    Alert alert;
    alert.txnId = txn.id;
    alert.cardId = txn.cardId;
    alert.merchantId = txn.merchantId;
    alert.city = txn.city;
    alert.amount = txn.amount;
    alert.timestamp = txn.timestamp;
    alert.riskScore = decision.riskScore;
    alert.reasons = decision.reasons;
    push(std::move(alert));
}

void AlertQueue::push(Alert alert) {
    std::lock_guard<std::mutex> lock(mutex_);
    alert.sequence = nextSequence_++;
    heap_.push(std::move(alert));
}

std::optional<Alert> AlertQueue::pop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (heap_.empty()) {
        return std::nullopt;
    }
    Alert top = heap_.top();
    heap_.pop();
    return top;
}

std::size_t AlertQueue::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return heap_.size();
}

std::vector<Alert> AlertQueue::drainSorted() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Alert> sorted;
    sorted.reserve(heap_.size());
    while (!heap_.empty()) {
        sorted.push_back(heap_.top());
        heap_.pop();
    }
    return sorted;
}

}  // namespace payguard
