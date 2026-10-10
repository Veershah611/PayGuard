#include "payguard/Pipeline.h"

#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace payguard {

Pipeline::Pipeline(AuthorizationEngine& engine, MetricsCollector& metrics,
                   PipelineOptions options)
    : engine_(engine), metrics_(metrics), options_(options) {
    if (options_.threads == 0) {
        throw std::invalid_argument("pipeline needs at least one worker thread");
    }
    if (options_.batchSize == 0 || options_.queueCapacity == 0) {
        throw std::invalid_argument("batch size and queue capacity must be positive");
    }
    if (metrics_.workers() < options_.threads) {
        throw std::invalid_argument("MetricsCollector has fewer buffers than worker threads");
    }
}

std::size_t Pipeline::shardFor(const std::string& cardId, std::size_t shards) {
    return std::hash<std::string>{}(cardId) % shards;
}

void Pipeline::workerLoop(std::size_t worker, BatchQueue& queue) {
    using Clock = std::chrono::steady_clock;
    std::string auditChunk;
    while (auto batch = queue.pop()) {
        if (audit_ != nullptr) {
            auditChunk.clear();
            auditChunk.reserve(batch->size() * 80);
        }
        for (const Transaction& txn : *batch) {
            // Latency covers the decision only: lookup, checks, rules, debit.
            const auto start = Clock::now();
            const Decision decision = engine_.process(txn);
            const auto stop = Clock::now();
            const double latencyUs =
                std::chrono::duration<double, std::micro>(stop - start).count();

            metrics_.record(worker, txn, decision, latencyUs);
            if (audit_ != nullptr) {
                AuditLogger::appendLine(auditChunk, txn, decision, latencyUs);
            }
            if (decision.verdict != Verdict::Approve) {
                for (AlertListener* listener : listeners_) {
                    listener->onAlert(txn, decision);
                }
            }
        }
        if (audit_ != nullptr) {
            audit_->write(std::move(auditChunk));
            auditChunk = std::string();
        }
    }
}

PipelineResult Pipeline::run(TransactionSource& source) {
    const std::size_t workers = options_.threads;
    std::vector<std::unique_ptr<BatchQueue>> queues;
    queues.reserve(workers);
    for (std::size_t i = 0; i < workers; ++i) {
        queues.push_back(std::make_unique<BatchQueue>(options_.queueCapacity));
    }

    std::mutex errorMutex;
    std::exception_ptr firstError;

    const auto start = std::chrono::steady_clock::now();

    std::vector<std::thread> threads;
    threads.reserve(workers);
    for (std::size_t i = 0; i < workers; ++i) {
        threads.emplace_back([this, i, &queues, &errorMutex, &firstError] {
            try {
                workerLoop(i, *queues[i]);
            } catch (...) {
                {
                    std::lock_guard<std::mutex> lock(errorMutex);
                    if (!firstError) {
                        firstError = std::current_exception();
                    }
                }
                // Keep draining so the reader never blocks on a dead worker.
                while (queues[i]->pop()) {
                }
            }
        });
    }

    // Reader: route by card into per-worker batches.
    std::uint64_t produced = 0;
    std::vector<Batch> pending(workers);
    for (auto& batch : pending) {
        batch.reserve(options_.batchSize);
    }
    try {
        Transaction txn;
        while (source.next(txn)) {
            const std::size_t shard = shardFor(txn.cardId, workers);
            pending[shard].push_back(std::move(txn));
            ++produced;
            if (pending[shard].size() >= options_.batchSize) {
                queues[shard]->push(std::move(pending[shard]));
                pending[shard] = Batch();
                pending[shard].reserve(options_.batchSize);
            }
        }
        for (std::size_t shard = 0; shard < workers; ++shard) {
            if (!pending[shard].empty()) {
                queues[shard]->push(std::move(pending[shard]));
            }
        }
    } catch (...) {
        std::lock_guard<std::mutex> lock(errorMutex);
        if (!firstError) {
            firstError = std::current_exception();
        }
    }

    for (auto& queue : queues) {
        queue->close();
    }
    for (auto& thread : threads) {
        thread.join();
    }
    const auto stop = std::chrono::steady_clock::now();

    if (firstError) {
        std::rethrow_exception(firstError);
    }

    PipelineResult result;
    result.processed = produced;
    result.wallSeconds = std::chrono::duration<double>(stop - start).count();
    return result;
}

}  // namespace payguard
