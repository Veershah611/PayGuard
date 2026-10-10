#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "payguard/AlertListener.h"
#include "payguard/AuditLogger.h"
#include "payguard/AuthorizationEngine.h"
#include "payguard/MetricsCollector.h"
#include "payguard/TransactionQueue.h"
#include "payguard/TransactionSource.h"

namespace payguard {

struct PipelineOptions {
    std::size_t threads = 1;
    std::size_t queueCapacity = 1024;  // batches per worker queue
    std::size_t batchSize = 64;        // transactions per batch
};

struct PipelineResult {
    std::uint64_t processed = 0;
    double wallSeconds = 0.0;
    double throughput() const {
        return wallSeconds > 0.0 ? static_cast<double>(processed) / wallSeconds : 0.0;
    }
};

// reader -> per-worker bounded queues -> N workers -> engine -> audit/alerts/metrics
//
// The reader routes each transaction by hash(card_id) % N, so every card is
// always handled by the same worker. That keeps each card's transactions in
// order (velocity and geo rules depend on it) and means two workers almost
// never want the same account lock. Transactions travel in small batches to
// keep queue synchronisation off the hot path.
class Pipeline {
public:
    Pipeline(AuthorizationEngine& engine, MetricsCollector& metrics, PipelineOptions options);

    void setAuditLogger(AuditLogger* logger) { audit_ = logger; }
    void addListener(AlertListener* listener) { listeners_.push_back(listener); }

    // Runs the source to completion; blocks until every worker is done.
    PipelineResult run(TransactionSource& source);

    static std::size_t shardFor(const std::string& cardId, std::size_t shards);

private:
    using Batch = std::vector<Transaction>;
    using BatchQueue = TransactionQueue<Batch>;

    void workerLoop(std::size_t worker, BatchQueue& queue);

    AuthorizationEngine& engine_;
    MetricsCollector& metrics_;
    PipelineOptions options_;
    AuditLogger* audit_ = nullptr;
    std::vector<AlertListener*> listeners_;
};

}  // namespace payguard
