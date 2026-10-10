#pragma once

#include <cstddef>
#include <ostream>
#include <string>
#include <vector>

#include "payguard/AlertQueue.h"
#include "payguard/Loaders.h"
#include "payguard/MetricsCollector.h"
#include "payguard/Pipeline.h"
#include "payguard/RiskyCardTracker.h"

namespace payguard {

// Sum of balances before - approved spend must equal sum of balances after.
struct BalanceCheck {
    Money initialTotal = 0;
    Money finalTotal = 0;
    Money approvedSpend = 0;
    std::size_t overdrawnAccounts = 0;  // balance below -credit_limit

    Money discrepancy() const { return initialTotal - approvedSpend - finalTotal; }
    bool passed() const { return discrepancy() == 0 && overdrawnAccounts == 0; }
};

struct RunReport {
    std::size_t threads = 0;
    std::size_t accounts = 0;
    PipelineResult pipeline;
    LoadStats transactionStats;
    LoadStats accountStats;
    MetricsCollector::Summary metrics;
    BalanceCheck balance;
    std::vector<RiskyCard> topCards;
    std::size_t alertsQueued = 0;
};

void writeMetricsReport(std::ostream& out, const RunReport& report);

// alerts.csv, riskiest first.
void writeAlertsCsv(std::ostream& out, const std::vector<Alert>& alerts);

std::string formatPercent(double fraction);

}  // namespace payguard
