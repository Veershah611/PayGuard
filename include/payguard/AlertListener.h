#pragma once

#include <cstddef>
#include <mutex>
#include <ostream>

#include "payguard/Decision.h"
#include "payguard/Transaction.h"

namespace payguard {

// Observer notified of every FLAG and DECLINE. The engine pipeline depends
// only on this interface; console output, the analyst heap and the risky-card
// tracker are interchangeable subscribers.
//
// onAlert() is called from worker threads concurrently, so implementations
// must be thread-safe.
class AlertListener {
public:
    virtual ~AlertListener() = default;
    virtual void onAlert(const Transaction& txn, const Decision& decision) = 0;
};

// Prints the first `limit` fraud alerts (FLAG or FRAUD_SUSPECTED) to a stream
// and counts the rest, so a large run does not flood the console.
class ConsoleAlertListener : public AlertListener {
public:
    ConsoleAlertListener(std::ostream& out, std::size_t limit) : out_(out), limit_(limit) {}

    void onAlert(const Transaction& txn, const Decision& decision) override;
    std::size_t suppressed() const;

private:
    std::ostream& out_;
    const std::size_t limit_;
    mutable std::mutex mutex_;
    std::size_t printed_ = 0;
    std::size_t suppressed_ = 0;
};

}  // namespace payguard
