#include "payguard/MetricsCollector.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace payguard {

namespace {

double ratio(std::uint64_t numerator, std::uint64_t denominator) {
    return denominator == 0 ? 0.0
                            : static_cast<double>(numerator) / static_cast<double>(denominator);
}

// Nearest-rank percentile; `samples` is partially reordered.
double percentile(std::vector<double>& samples, double p) {
    const auto n = samples.size();
    auto rank = static_cast<std::size_t>(std::ceil(p * static_cast<double>(n)));
    rank = std::clamp<std::size_t>(rank, 1, n) - 1;
    std::nth_element(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(rank),
                     samples.end());
    return samples[rank];
}

}  // namespace

LatencySummary summarizeLatencies(std::vector<double> samples) {
    LatencySummary summary;
    summary.count = samples.size();
    if (samples.empty()) {
        return summary;
    }
    summary.mean = std::accumulate(samples.begin(), samples.end(), 0.0) /
                   static_cast<double>(samples.size());
    summary.max = *std::max_element(samples.begin(), samples.end());
    summary.p50 = percentile(samples, 0.50);
    summary.p95 = percentile(samples, 0.95);
    summary.p99 = percentile(samples, 0.99);
    summary.p999 = percentile(samples, 0.999);
    return summary;
}

double AccuracyCounts::recall() const { return ratio(fraudCaught, fraudTotal); }
double AccuracyCounts::ruleRecall() const { return ratio(fraudAlerted, fraudTotal); }
double AccuracyCounts::falsePositiveRate() const { return ratio(legitAlerted, legitTotal); }
double AccuracyCounts::precision() const {
    return ratio(fraudAlerted, fraudAlerted + legitAlerted);
}

MetricsCollector::MetricsCollector(std::size_t workers, std::vector<std::string> ruleNames,
                                   const LabelMap* labels, std::size_t expectedSamples)
    : ruleNames_(std::move(ruleNames)), labels_(labels), buffers_(workers) {
    if (workers == 0) {
        throw std::invalid_argument("MetricsCollector needs at least one worker");
    }
    for (auto& buffer : buffers_) {
        buffer.ruleHits.assign(ruleNames_.size(), 0);
        buffer.latenciesUs.reserve(expectedSamples / workers + 1);
    }
}

void MetricsCollector::record(std::size_t worker, const Transaction& txn,
                              const Decision& decision, double latencyUs) {
    Buffer& b = buffers_[worker];
    b.latenciesUs.push_back(latencyUs);

    switch (decision.verdict) {
        case Verdict::Approve: ++b.approved; break;
        case Verdict::Flag: ++b.flagged; break;
        case Verdict::Decline: ++b.declined; break;
    }
    ++b.declineReasons[static_cast<std::size_t>(decision.reason)];
    if (decision.approved()) {
        b.approvedSpend += txn.amount;
    }
    for (std::uint32_t mask = decision.ruleHits; mask != 0; mask &= mask - 1) {
        std::size_t bit = 0;
        while (((mask >> bit) & 1u) == 0) {
            ++bit;
        }
        if (bit < b.ruleHits.size()) {
            ++b.ruleHits[bit];
        }
    }

    if (labels_ == nullptr) {
        return;
    }
    const auto it = labels_->find(txn.id);
    if (it == labels_->end()) {
        ++b.accuracy.unlabeled;
        return;
    }
    const Label& label = it->second;
    const bool alerted = decision.isFraudAlert();
    if (label.fraud) {
        const bool caught = decision.verdict != Verdict::Approve;
        ++b.accuracy.fraudTotal;
        b.accuracy.fraudCaught += caught ? 1 : 0;
        b.accuracy.fraudAlerted += alerted ? 1 : 0;
        TypeCounts& type = b.byFraudType[label.type.empty() ? "UNSPECIFIED" : label.type];
        ++type.total;
        type.caught += caught ? 1 : 0;
    } else {
        ++b.accuracy.legitTotal;
        b.accuracy.legitAlerted += alerted ? 1 : 0;
    }
}

MetricsCollector::Summary MetricsCollector::summarize() const {
    Summary s;
    s.ruleNames = ruleNames_;
    s.ruleHits.assign(ruleNames_.size(), 0);
    s.hasLabels = labels_ != nullptr;

    std::vector<double> latencies;
    std::size_t sampleCount = 0;
    for (const auto& b : buffers_) {
        sampleCount += b.latenciesUs.size();
    }
    latencies.reserve(sampleCount);

    for (const auto& b : buffers_) {
        latencies.insert(latencies.end(), b.latenciesUs.begin(), b.latenciesUs.end());
        s.approved += b.approved;
        s.flagged += b.flagged;
        s.declined += b.declined;
        for (std::size_t i = 0; i < kDeclineReasonCount; ++i) {
            s.declineReasons[i] += b.declineReasons[i];
        }
        for (std::size_t i = 0; i < s.ruleHits.size(); ++i) {
            s.ruleHits[i] += b.ruleHits[i];
        }
        s.approvedSpend += b.approvedSpend;
        s.accuracy.fraudTotal += b.accuracy.fraudTotal;
        s.accuracy.fraudCaught += b.accuracy.fraudCaught;
        s.accuracy.fraudAlerted += b.accuracy.fraudAlerted;
        s.accuracy.legitTotal += b.accuracy.legitTotal;
        s.accuracy.legitAlerted += b.accuracy.legitAlerted;
        s.accuracy.unlabeled += b.accuracy.unlabeled;
        for (const auto& entry : b.byFraudType) {
            TypeCounts& t = s.byFraudType[entry.first];
            t.total += entry.second.total;
            t.caught += entry.second.caught;
        }
    }
    s.total = s.approved + s.flagged + s.declined;
    s.latency = summarizeLatencies(std::move(latencies));
    return s;
}

}  // namespace payguard
