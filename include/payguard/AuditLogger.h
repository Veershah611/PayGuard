#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "payguard/Decision.h"
#include "payguard/Transaction.h"

namespace payguard {

// Append-only audit trail, one line per decision:
//   T000002 | C1001 | 4800.00 | FLAG | score=55 | GEO_IMPOSSIBLE,AMOUNT_ANOMALY | 12.4us
//
// In Async mode a dedicated writer thread owns the file, so workers never
// block on disk I/O; they hand over whole chunks of lines, which keeps lock
// traffic to one acquisition per chunk. A byte cap on pending data applies
// back-pressure if the disk falls behind. The file handle is RAII: the
// destructor drains everything and closes it.
class AuditLogger {
public:
    enum class Mode { Sync, Async };

    // Truncates `path`; lines are only ever appended after that.
    explicit AuditLogger(const std::string& path, Mode mode = Mode::Async,
                         std::size_t maxPendingBytes = 16u << 20);
    ~AuditLogger();

    AuditLogger(const AuditLogger&) = delete;
    AuditLogger& operator=(const AuditLogger&) = delete;

    // `chunk` holds one or more complete, '\n'-terminated lines.
    void write(std::string chunk);

    // Blocks until everything written so far has reached the OS.
    void flush();

    std::uint64_t linesWritten() const;

    // Appends the audit line for one decision to `out`.
    static void appendLine(std::string& out, const Transaction& txn, const Decision& decision,
                           double latencyUs);

private:
    void writerLoop();
    void writeChunk(const std::string& chunk);

    std::ofstream out_;
    const Mode mode_;
    const std::size_t maxPendingBytes_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;      // writer: work available or stopping
    std::condition_variable progress_;  // producers / flush(): space freed or batch written
    std::vector<std::string> pending_;
    std::size_t pendingBytes_ = 0;
    std::uint64_t submitted_ = 0;  // chunks handed over
    std::uint64_t written_ = 0;    // chunks written by the writer thread
    std::uint64_t flushed_ = 0;    // chunks known to have reached the OS
    bool flushRequested_ = false;
    std::uint64_t lines_ = 0;
    bool stopping_ = false;
    std::thread writer_;
};

}  // namespace payguard
