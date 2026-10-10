#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "payguard/AlertListener.h"

namespace payguard {

struct RiskyCard {
    std::string cardId;
    std::uint64_t alerts = 0;  // fraud alerts (FLAG or FRAUD_SUSPECTED)
    double totalScore = 0.0;
    double maxScore = 0.0;
};

// Returns the k riskiest cards (by total score, then alert count, then card
// ID), riskiest first. Uses a size-k min-heap: O(n log k) time, O(k) extra
// memory, instead of sorting all n cards.
std::vector<RiskyCard> selectTopK(const std::vector<RiskyCard>& cards, std::size_t k);

// Aggregates fraud alerts per card for the end-of-run "top risky cards" report.
class RiskyCardTracker : public AlertListener {
public:
    void onAlert(const Transaction& txn, const Decision& decision) override;
    std::vector<RiskyCard> topK(std::size_t k) const;
    std::size_t cardCount() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, RiskyCard> cards_;
};

}  // namespace payguard
