#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "payguard/Account.h"
#include "payguard/CityTable.h"
#include "payguard/Transaction.h"

namespace payguard {

class AccountStore;

// Thrown when an input file cannot be opened at all. Bad rows inside a file
// never throw; they are skipped and counted in LoadStats.
class IoError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct LoadStats {
    static constexpr std::size_t kMaxSampleErrors = 10;

    std::size_t rows = 0;       // data rows seen (header, blank and comment lines excluded)
    std::size_t loaded = 0;     // rows accepted
    std::size_t malformed = 0;  // rows skipped
    std::vector<std::string> sampleErrors;  // first few problems, for the report

    void reject(std::size_t lineNumber, const std::string& message);
};

struct AccountRecord {
    std::string cardId;
    Money balance = 0;
    Money creditLimit = 0;
    Money dailyLimit = 0;
    std::string homeCity;
    CardStatus status = CardStatus::Active;
};

// Row parsers. Return false (and describe the problem in `error`) for a
// malformed row. A transaction whose amount is unparseable, zero or negative is
// still returned, with amount 0, so the engine can decline it as
// INVALID_AMOUNT and the decision appears in the audit log.
bool parseAccountRow(const std::vector<std::string_view>& fields, AccountRecord& out,
                     std::string& error);
bool parseTransactionRow(const std::vector<std::string_view>& fields, Transaction& out,
                         std::string& error);

// accounts.csv: card_id,balance,credit_limit,daily_limit,home_city,status
LoadStats loadAccounts(const std::string& path, AccountStore& store);

// One merchant ID per line; '#' starts a comment.
std::unordered_set<std::string> loadBlocklist(const std::string& path,
                                              LoadStats* stats = nullptr);

// cities.csv: city,latitude,longitude
CityTable loadCities(const std::string& path, LoadStats* stats = nullptr);

// Ground truth produced by the data generator.
struct Label {
    bool fraud = false;
    std::string type;  // e.g. VELOCITY, GEO, AMOUNT, BLOCKED_MERCHANT, ROUND_AMOUNT
};
using LabelMap = std::unordered_map<std::string, Label>;

// labels.csv: txn_id,is_fraud,fraud_type
LabelMap loadLabels(const std::string& path, LoadStats* stats = nullptr);

}  // namespace payguard
