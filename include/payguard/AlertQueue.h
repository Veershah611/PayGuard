#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <vector>

#include "payguard/AlertListener.h"
#include "payguard/Money.h"

namespace payguard {

struct Alert {
    std::string txnId;
    std::string cardId;
    std::string merchantId;
    std::string city;
    Money amount = 0;
    std::int64_t timestamp = 0;
    double riskScore = 0.0;
    std::vector<std::string> reasons;
    std::uint64_t sequence = 0;  // arrival order; breaks ties between equal scores
};

// Analyst review queue: a max-heap of FLAGged transactions keyed by risk
// score, so the riskiest case is always on top. O(log n) insert, O(1) peek.
class AlertQueue : public AlertListener {
public:
    void onAlert(const Transaction& txn, const Decision& decision) override;

    void push(Alert alert);
    std::optional<Alert> pop();  // riskiest first
    std::size_t size() const;

    // Empties the queue, returning alerts riskiest-first.
    std::vector<Alert> drainSorted();

private:
    struct LowerPriority {
        bool operator()(const Alert& a, const Alert& b) const {
            if (a.riskScore != b.riskScore) {
                return a.riskScore < b.riskScore;
            }
            return a.sequence > b.sequence;  // earlier alert wins a tie
        }
    };

    mutable std::mutex mutex_;
    std::priority_queue<Alert, std::vector<Alert>, LowerPriority> heap_;
    std::uint64_t nextSequence_ = 0;
};

}  // namespace payguard
