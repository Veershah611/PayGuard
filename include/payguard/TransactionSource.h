#pragma once

#include <cstddef>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "payguard/Loaders.h"
#include "payguard/Transaction.h"

namespace payguard {

// Where transactions come from (CSV file, in-memory vector, generator, socket).
class TransactionSource {
public:
    virtual ~TransactionSource() = default;
    // Fills `out` with the next transaction; returns false at end of stream.
    virtual bool next(Transaction& out) = 0;
};

// Streams transactions.csv one row at a time, so memory stays flat no matter
// how large the file is. Malformed rows are skipped and counted.
class CsvTransactionSource : public TransactionSource {
public:
    explicit CsvTransactionSource(const std::string& path);  // throws IoError
    bool next(Transaction& out) override;
    const LoadStats& stats() const { return stats_; }

private:
    std::ifstream in_;
    std::string line_;
    std::vector<std::string_view> fields_;
    std::string error_;
    std::size_t lineNumber_ = 0;
    bool firstRow_ = true;
    LoadStats stats_;
};

// Replays an in-memory list; used by tests and benchmarks.
class VectorTransactionSource : public TransactionSource {
public:
    explicit VectorTransactionSource(const std::vector<Transaction>& transactions)
        : transactions_(transactions) {}
    bool next(Transaction& out) override;

private:
    const std::vector<Transaction>& transactions_;
    std::size_t position_ = 0;
};

}  // namespace payguard
