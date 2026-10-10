#include "payguard/Loaders.h"

#include <fstream>

#include "payguard/AccountStore.h"
#include "payguard/StringUtil.h"

namespace payguard {

void LoadStats::reject(std::size_t lineNumber, const std::string& message) {
    ++malformed;
    if (sampleErrors.size() < kMaxSampleErrors) {
        sampleErrors.push_back("line " + std::to_string(lineNumber) + ": " + message);
    }
}

namespace {

std::ifstream openOrThrow(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw IoError("cannot open '" + path + "'");
    }
    return in;
}

// Calls fn(lineNumber, fields) for each data row of a CSV file. Blank lines and
// '#' comments are ignored; the first row is skipped as a header when its first
// field equals `headerFirstField`.
template <typename Fn>
void forEachCsvRow(const std::string& path, std::string_view headerFirstField, Fn&& fn) {
    std::ifstream in = openOrThrow(path);
    std::string line;
    std::vector<std::string_view> fields;
    std::size_t lineNumber = 0;
    bool firstRow = true;
    while (std::getline(in, line)) {
        ++lineNumber;
        const std::string_view view = trim(line);
        if (view.empty() || view.front() == '#') {
            continue;
        }
        splitFields(view, ',', fields);
        if (firstRow) {
            firstRow = false;
            if (equalsIgnoreCase(fields.front(), headerFirstField)) {
                continue;
            }
        }
        fn(lineNumber, fields);
    }
}

bool parseStatus(std::string_view text, CardStatus& out) {
    if (equalsIgnoreCase(text, "ACTIVE")) {
        out = CardStatus::Active;
        return true;
    }
    if (equalsIgnoreCase(text, "BLOCKED")) {
        out = CardStatus::Blocked;
        return true;
    }
    return false;
}

}  // namespace

bool parseAccountRow(const std::vector<std::string_view>& fields, AccountRecord& out,
                     std::string& error) {
    if (fields.size() != 6) {
        error = "expected 6 fields, got " + std::to_string(fields.size());
        return false;
    }
    if (fields[0].empty()) {
        error = "empty card_id";
        return false;
    }
    const auto balance = parseMoney(fields[1]);
    const auto creditLimit = parseMoney(fields[2]);
    const auto dailyLimit = parseMoney(fields[3]);
    if (!balance || !creditLimit || !dailyLimit) {
        error = "invalid money field";
        return false;
    }
    if (*creditLimit < 0 || *dailyLimit < 0) {
        error = "limits must be non-negative";
        return false;
    }
    if (!parseStatus(fields[5], out.status)) {
        error = "unknown status '" + std::string(fields[5]) + "'";
        return false;
    }
    out.cardId.assign(fields[0]);
    out.balance = *balance;
    out.creditLimit = *creditLimit;
    out.dailyLimit = *dailyLimit;
    out.homeCity.assign(fields[4]);
    return true;
}

bool parseTransactionRow(const std::vector<std::string_view>& fields, Transaction& out,
                         std::string& error) {
    if (fields.size() != 6) {
        error = "expected 6 fields, got " + std::to_string(fields.size());
        return false;
    }
    if (fields[0].empty() || fields[1].empty()) {
        error = "empty txn_id or card_id";
        return false;
    }
    const auto timestamp = parseInt64(fields[5]);
    if (!timestamp) {
        error = "invalid timestamp '" + std::string(fields[5]) + "'";
        return false;
    }
    const auto amount = parseMoney(fields[3]);
    out.id.assign(fields[0]);
    out.cardId.assign(fields[1]);
    out.merchantId.assign(fields[2]);
    out.amount = (amount && *amount > 0) ? *amount : 0;
    out.city.assign(fields[4]);
    out.timestamp = *timestamp;
    return true;
}

LoadStats loadAccounts(const std::string& path, AccountStore& store) {
    LoadStats stats;
    AccountRecord record;
    std::string error;
    forEachCsvRow(path, "card_id", [&](std::size_t lineNumber, const auto& fields) {
        ++stats.rows;
        if (!parseAccountRow(fields, record, error)) {
            stats.reject(lineNumber, error);
            return;
        }
        if (store.add(record.cardId, record.balance, record.creditLimit, record.dailyLimit,
                      record.homeCity, record.status) == nullptr) {
            stats.reject(lineNumber, "duplicate card_id " + record.cardId);
            return;
        }
        ++stats.loaded;
    });
    return stats;
}

std::unordered_set<std::string> loadBlocklist(const std::string& path, LoadStats* stats) {
    std::ifstream in = openOrThrow(path);
    std::unordered_set<std::string> merchants;
    LoadStats local;
    std::string line;
    while (std::getline(in, line)) {
        const std::string_view view = trim(line);
        if (view.empty() || view.front() == '#') {
            continue;
        }
        ++local.rows;
        merchants.emplace(view);
        ++local.loaded;
    }
    if (stats != nullptr) {
        *stats = std::move(local);
    }
    return merchants;
}

CityTable loadCities(const std::string& path, LoadStats* stats) {
    CityTable table;
    LoadStats local;
    forEachCsvRow(path, "city", [&](std::size_t lineNumber, const auto& fields) {
        ++local.rows;
        if (fields.size() != 3 || fields[0].empty()) {
            local.reject(lineNumber, "expected city,latitude,longitude");
            return;
        }
        const auto lat = parseDouble(fields[1]);
        const auto lon = parseDouble(fields[2]);
        if (!lat || !lon || *lat < -90.0 || *lat > 90.0 || *lon < -180.0 || *lon > 180.0) {
            local.reject(lineNumber, "invalid coordinates");
            return;
        }
        if (!table.add(std::string(fields[0]), GeoPoint{*lat, *lon})) {
            local.reject(lineNumber, "duplicate city " + std::string(fields[0]));
            return;
        }
        ++local.loaded;
    });
    if (stats != nullptr) {
        *stats = std::move(local);
    }
    return table;
}

LabelMap loadLabels(const std::string& path, LoadStats* stats) {
    LabelMap labels;
    LoadStats local;
    forEachCsvRow(path, "txn_id", [&](std::size_t lineNumber, const auto& fields) {
        ++local.rows;
        if (fields.size() < 2 || fields.size() > 3 || fields[0].empty()) {
            local.reject(lineNumber, "expected txn_id,is_fraud[,fraud_type]");
            return;
        }
        const auto flag = parseInt64(fields[1]);
        if (!flag || (*flag != 0 && *flag != 1)) {
            local.reject(lineNumber, "is_fraud must be 0 or 1");
            return;
        }
        Label label;
        label.fraud = *flag == 1;
        if (fields.size() == 3) {
            label.type.assign(fields[2]);
        }
        labels[std::string(fields[0])] = std::move(label);
        ++local.loaded;
    });
    if (stats != nullptr) {
        *stats = std::move(local);
    }
    return labels;
}

}  // namespace payguard
