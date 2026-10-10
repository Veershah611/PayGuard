#include "payguard/RiskyCardTracker.h"

#include <algorithm>
#include <queue>

namespace payguard {

namespace {

// True if `a` ranks as riskier than `b`.
bool riskier(const RiskyCard& a, const RiskyCard& b) {
    if (a.totalScore != b.totalScore) {
        return a.totalScore > b.totalScore;
    }
    if (a.alerts != b.alerts) {
        return a.alerts > b.alerts;
    }
    return a.cardId < b.cardId;
}

}  // namespace

std::vector<RiskyCard> selectTopK(const std::vector<RiskyCard>& cards, std::size_t k) {
    if (k == 0) {
        return {};
    }
    // With `riskier` as the comparator the heap's top is the least risky of
    // the current top-k, i.e. the one to evict when something riskier arrives.
    std::priority_queue<RiskyCard, std::vector<RiskyCard>, decltype(&riskier)> heap(&riskier);
    for (const auto& card : cards) {
        if (heap.size() < k) {
            heap.push(card);
        } else if (riskier(card, heap.top())) {
            heap.pop();
            heap.push(card);
        }
    }
    std::vector<RiskyCard> top;
    top.reserve(heap.size());
    while (!heap.empty()) {
        top.push_back(heap.top());
        heap.pop();
    }
    std::reverse(top.begin(), top.end());
    return top;
}

void RiskyCardTracker::onAlert(const Transaction& txn, const Decision& decision) {
    if (!decision.isFraudAlert()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    RiskyCard& card = cards_[txn.cardId];
    if (card.cardId.empty()) {
        card.cardId = txn.cardId;
    }
    ++card.alerts;
    card.totalScore += decision.riskScore;
    card.maxScore = std::max(card.maxScore, decision.riskScore);
}

std::vector<RiskyCard> RiskyCardTracker::topK(std::size_t k) const {
    std::vector<RiskyCard> all;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        all.reserve(cards_.size());
        for (const auto& entry : cards_) {
            all.push_back(entry.second);
        }
    }
    return selectTopK(all, k);
}

std::size_t RiskyCardTracker::cardCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cards_.size();
}

}  // namespace payguard
