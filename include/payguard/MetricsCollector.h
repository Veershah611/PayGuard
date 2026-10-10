#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "payguard/Decision.h"
#include "payguard/Loaders.h"
#include "payguard/Money.h"
#include "payguard/Transaction.h"

namespace payguard {

struct LatencySummary {
    std::size_t count = 0;
    double mean = 0.0;
    double p50 = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
    double p999 = 0.0;
    double max = 0.0;
};

// Percentiles via std::nth_element: O(n) on average, no full sort. Takes the
// samples by value because nth_element reorders them.
LatencySummary summarizeLatencies(std::vector<double> samples);

// Detection accuracy against ground-truth labels.
//   caught      = fraud that was FLAGged or DECLINEd for any reason
//   fraud alert = FLAG or FRAUD_SUSPECTED decline (raised by the fraud rules)
//   recall      = caught fraud / all fraud
//   FPR         = legit with a fraud alert / all legit
//   precision   = fraud alerts that were fraud / all fraud alerts
struct AccuracyCounts {
    std::uint64_t fraudTotal = 0;
    std::uint64_t fraudCaught = 0;
    std::uint64_t fraudAlerted = 0;
    std::uint64_t legitTotal = 0;
    std::uint64_t legitAlerted = 0;  // false positives
    std::uint64_t unlabeled = 0;

    double recall() const;
    double ruleRecall() const;  // fraud alerts only, excluding hard-check declines
    double falsePositiveRate() const;
    double precision() const;
};

struct TypeCounts {
    std::uint64_t total = 0;
    std::uint64_t caught = 0;
};

// Records one sample per decision. Each worker writes only to its own
// cache-line-aligned buffer, so recording takes no locks and causes no false
// sharing; buffers are merged once, after the workers have stopped.
class MetricsCollector {
public:
    MetricsCollector(std::size_t workers, std::vector<std::string> ruleNames,
                     const LabelMap* labels = nullptr, std::size_t expectedSamples = 0);

    // Called only by worker `worker`.
    void record(std::size_t worker, const Transaction& txn, const Decision& decision,
                double latencyUs);

    struct Summary {
        std::uint64_t total = 0;
        std::uint64_t approved = 0;
        std::uint64_t flagged = 0;
        std::uint64_t declined = 0;
        std::array<std::uint64_t, kDeclineReasonCount> declineReasons{};
        std::vector<std::string> ruleNames;
        std::vector<std::uint64_t> ruleHits;
        Money approvedSpend = 0;  // debited by APPROVE + FLAG
        LatencySummary latency;
        bool hasLabels = false;
        AccuracyCounts accuracy;
        std::map<std::string, TypeCounts> byFraudType;
    };

    // Call only after all workers have finished.
    Summary summarize() const;

    std::size_t workers() const { return buffers_.size(); }

private:
    struct alignas(64) Buffer {
        std::vector<double> latenciesUs;
        std::uint64_t approved = 0;
        std::uint64_t flagged = 0;
        std::uint64_t declined = 0;
        std::array<std::uint64_t, kDeclineReasonCount> declineReasons{};
        std::vector<std::uint64_t> ruleHits;
        Money approvedSpend = 0;
        AccuracyCounts accuracy;
        std::unordered_map<std::string, TypeCounts> byFraudType;
    };

    std::vector<std::string> ruleNames_;
    const LabelMap* labels_;
    std::vector<Buffer> buffers_;
};

}  // namespace payguard
