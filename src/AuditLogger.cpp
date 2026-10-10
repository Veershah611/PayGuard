#include "payguard/AuditLogger.h"

#include <algorithm>
#include <cstdio>

#include "payguard/Loaders.h"

namespace payguard {

AuditLogger::AuditLogger(const std::string& path, Mode mode, std::size_t maxPendingBytes)
    : out_(path, std::ios::out | std::ios::trunc | std::ios::binary),
      mode_(mode),
      maxPendingBytes_(std::max<std::size_t>(maxPendingBytes, 1)) {
    if (!out_) {
        throw IoError("cannot open audit log '" + path + "' for writing");
    }
    if (mode_ == Mode::Async) {
        writer_ = std::thread(&AuditLogger::writerLoop, this);
    }
}

AuditLogger::~AuditLogger() {
    if (mode_ == Mode::Async) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_one();
        writer_.join();
    }
    out_.flush();
}

void AuditLogger::writeChunk(const std::string& chunk) {
    out_.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
}

void AuditLogger::write(std::string chunk) {
    if (chunk.empty()) {
        return;
    }
    const auto lines = static_cast<std::uint64_t>(std::count(chunk.begin(), chunk.end(), '\n'));
    if (mode_ == Mode::Sync) {
        std::lock_guard<std::mutex> lock(mutex_);
        writeChunk(chunk);
        lines_ += lines;
        return;
    }
    {
        std::unique_lock<std::mutex> lock(mutex_);
        // Back-pressure, but always admit a chunk into an empty buffer so an
        // oversized chunk cannot wait forever.
        progress_.wait(lock, [this] { return pendingBytes_ < maxPendingBytes_ || stopping_; });
        pendingBytes_ += chunk.size();
        pending_.push_back(std::move(chunk));
        ++submitted_;
        lines_ += lines;
    }
    wake_.notify_one();
}

void AuditLogger::writerLoop() {
    std::vector<std::string> batch;
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
        wake_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
        if (pending_.empty() && stopping_) {
            return;
        }
        batch.swap(pending_);
        pendingBytes_ = 0;
        lock.unlock();
        progress_.notify_all();  // producers may continue while we write

        for (const auto& chunk : batch) {
            writeChunk(chunk);
        }
        const auto count = batch.size();
        batch.clear();

        lock.lock();
        written_ += count;
        if (pending_.empty() || flushRequested_) {
            out_.flush();  // idle or asked to: push buffered bytes to the OS
            flushed_ = written_;
        }
        progress_.notify_all();
    }
}

void AuditLogger::flush() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (mode_ == Mode::Sync) {
        out_.flush();
        return;
    }
    // The writer thread owns the stream, so ask it to flush rather than
    // touching out_ from here.
    const std::uint64_t target = submitted_;
    flushRequested_ = true;
    wake_.notify_one();
    progress_.wait(lock, [this, target] { return flushed_ >= target; });
    flushRequested_ = false;
}

std::uint64_t AuditLogger::linesWritten() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lines_;
}

void AuditLogger::appendLine(std::string& out, const Transaction& txn, const Decision& decision,
                             double latencyUs) {
    char number[48];
    out += txn.id;
    out += " | ";
    out += txn.cardId;
    out += " | ";
    out += formatMoney(txn.amount);
    out += " | ";
    out += toString(decision.verdict);
    std::snprintf(number, sizeof(number), " | score=%.0f | ", decision.riskScore);
    out += number;

    bool first = true;
    auto addReason = [&](std::string_view reason) {
        if (!first) {
            out += ',';
        }
        out += reason;
        first = false;
    };
    if (decision.reason != DeclineReason::None) {
        addReason(toString(decision.reason));
    }
    for (const auto& reason : decision.reasons) {
        addReason(reason);
    }
    if (first) {
        out += '-';
    }

    std::snprintf(number, sizeof(number), " | %.1fus\n", latencyUs);
    out += number;
}

}  // namespace payguard
