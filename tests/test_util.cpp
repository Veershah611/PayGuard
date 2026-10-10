#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <numeric>
#include <random>
#include <set>
#include <thread>

#include "payguard/AccountStore.h"
#include "payguard/CityTable.h"
#include "payguard/Loaders.h"
#include "payguard/MetricsCollector.h"
#include "payguard/Money.h"
#include "payguard/RunningStats.h"
#include "payguard/StringUtil.h"
#include "payguard/TransactionQueue.h"
#include "payguard/TransactionSource.h"

using namespace payguard;

namespace {
const std::string kDataDir = PAYGUARD_TEST_DATA_DIR;
}

// ---------------------------------------------------------------- Money

TEST(Money, ParsesRupeesIntoPaise) {
    EXPECT_EQ(parseMoney("250.00"), 25000);
    EXPECT_EQ(parseMoney("250"), 25000);
    EXPECT_EQ(parseMoney("250.5"), 25050);
    EXPECT_EQ(parseMoney("0.07"), 7);
    EXPECT_EQ(parseMoney(" 12.34 "), 1234);
    EXPECT_EQ(parseMoney("-10.00"), -1000);
    EXPECT_EQ(parseMoney("+3"), 300);
    EXPECT_EQ(parseMoney(".5"), 50);
    EXPECT_EQ(parseMoney("7."), 700);
}

TEST(Money, RejectsMalformedAmounts) {
    EXPECT_FALSE(parseMoney(""));
    EXPECT_FALSE(parseMoney("abc"));
    EXPECT_FALSE(parseMoney("1.234"));  // sub-paisa precision
    EXPECT_FALSE(parseMoney("1,000"));
    EXPECT_FALSE(parseMoney("12a"));
    EXPECT_FALSE(parseMoney("."));
    EXPECT_FALSE(parseMoney("-"));
    EXPECT_FALSE(parseMoney("99999999999999999999"));  // overflow guard
    EXPECT_FALSE(parseMoney("1000000001"));            // above kMaxAmount
    EXPECT_TRUE(parseMoney("1000000000"));
}

TEST(Money, FormatsWithTwoDecimals) {
    EXPECT_EQ(formatMoney(25000), "250.00");
    EXPECT_EQ(formatMoney(7), "0.07");
    EXPECT_EQ(formatMoney(0), "0.00");
    EXPECT_EQ(formatMoney(-1050), "-10.50");
    EXPECT_EQ(formatMoney(INT64_MIN), "-92233720368547758.08");
}

TEST(Money, RoundTrips) {
    for (Money m : {Money{1}, Money{99}, Money{100}, Money{123456789}, Money{-5}}) {
        EXPECT_EQ(parseMoney(formatMoney(m)), m);
    }
}

// ---------------------------------------------------------------- Strings

TEST(StringUtil, TrimAndSplit) {
    EXPECT_EQ(trim("  a b \r\n"), "a b");
    EXPECT_EQ(trim(""), "");
    std::vector<std::string_view> fields;
    splitFields("a, b ,,c\r", ',', fields);
    ASSERT_EQ(fields.size(), 4u);
    EXPECT_EQ(fields[0], "a");
    EXPECT_EQ(fields[1], "b");
    EXPECT_EQ(fields[2], "");
    EXPECT_EQ(fields[3], "c");
}

TEST(StringUtil, Numbers) {
    EXPECT_EQ(parseInt64("1728550000"), 1728550000);
    EXPECT_EQ(parseInt64("-5"), -5);
    EXPECT_FALSE(parseInt64("12x"));
    EXPECT_FALSE(parseInt64(""));
    EXPECT_DOUBLE_EQ(*parseDouble("-33.8688"), -33.8688);
    EXPECT_FALSE(parseDouble("nan"));
    EXPECT_FALSE(parseDouble("1e999"));
    EXPECT_EQ(withThousands(0), "0");
    EXPECT_EQ(withThousands(999), "999");
    EXPECT_EQ(withThousands(1000), "1,000");
    EXPECT_EQ(withThousands(1234567), "1,234,567");
    EXPECT_TRUE(equalsIgnoreCase("Txn_ID", "txn_id"));
}

// ---------------------------------------------------------------- Geo

TEST(CityTable, HaversineKnownDistances) {
    const GeoPoint mumbai{19.0760, 72.8777};
    const GeoPoint london{51.5074, -0.1278};
    const GeoPoint delhi{28.6139, 77.2090};
    EXPECT_NEAR(haversineKm(mumbai, london), 7190.0, 30.0);
    EXPECT_NEAR(haversineKm(mumbai, delhi), 1150.0, 15.0);
    EXPECT_DOUBLE_EQ(haversineKm(mumbai, mumbai), 0.0);
    EXPECT_NEAR(haversineKm(mumbai, london), haversineKm(london, mumbai), 1e-9);
}

TEST(CityTable, LookupAndDuplicates) {
    CityTable table;
    EXPECT_TRUE(table.add("Pune", {18.52, 73.85}));
    EXPECT_FALSE(table.add("Pune", {0, 0}));
    ASSERT_NE(table.find("Pune"), nullptr);
    EXPECT_DOUBLE_EQ(table.find("Pune")->latitude, 18.52);
    EXPECT_EQ(table.find("Atlantis"), nullptr);
}

// ---------------------------------------------------------------- Loaders

TEST(Loaders, ParsesTransactionRows) {
    std::vector<std::string_view> fields;
    Transaction txn;
    std::string error;

    splitFields("T1,C1,M1,250.00,Mumbai,1728550000", ',', fields);
    ASSERT_TRUE(parseTransactionRow(fields, txn, error));
    EXPECT_EQ(txn.id, "T1");
    EXPECT_EQ(txn.amount, 25000);
    EXPECT_EQ(txn.timestamp, 1728550000);

    // Bad amounts are kept (as 0) so the engine declines them visibly.
    splitFields("T2,C1,M1,abc,Mumbai,1728550000", ',', fields);
    ASSERT_TRUE(parseTransactionRow(fields, txn, error));
    EXPECT_EQ(txn.amount, 0);
    splitFields("T3,C1,M1,-5.00,Mumbai,1728550000", ',', fields);
    ASSERT_TRUE(parseTransactionRow(fields, txn, error));
    EXPECT_EQ(txn.amount, 0);

    splitFields("T4,C1,M1,5.00,Mumbai", ',', fields);
    EXPECT_FALSE(parseTransactionRow(fields, txn, error));
    splitFields("T5,C1,M1,5.00,Mumbai,soon", ',', fields);
    EXPECT_FALSE(parseTransactionRow(fields, txn, error));
    splitFields("T6,,M1,5.00,Mumbai,1", ',', fields);
    EXPECT_FALSE(parseTransactionRow(fields, txn, error));
}

TEST(Loaders, ParsesAccountRows) {
    std::vector<std::string_view> fields;
    AccountRecord record;
    std::string error;
    splitFields("C1,5000.00,10000.00,2000.00,Ahmedabad,active", ',', fields);
    ASSERT_TRUE(parseAccountRow(fields, record, error));
    EXPECT_EQ(record.balance, 500000);
    EXPECT_EQ(record.status, CardStatus::Active);

    splitFields("C1,5000.00,10000.00,2000.00,Ahmedabad,FROZEN", ',', fields);
    EXPECT_FALSE(parseAccountRow(fields, record, error));
    splitFields("C1,5000.00,-1.00,2000.00,Ahmedabad,ACTIVE", ',', fields);
    EXPECT_FALSE(parseAccountRow(fields, record, error));
    splitFields("C1,lots,0,0,Ahmedabad,ACTIVE", ',', fields);
    EXPECT_FALSE(parseAccountRow(fields, record, error));
}

TEST(Loaders, LoadsSampleFiles) {
    AccountStore store;
    const LoadStats stats = loadAccounts(kDataDir + "/accounts.csv", store);
    EXPECT_EQ(stats.loaded, 7u);
    EXPECT_EQ(stats.malformed, 0u);
    ASSERT_NE(store.find("C1002"), nullptr);
    EXPECT_EQ(store.find("C1002")->status(), CardStatus::Blocked);
    EXPECT_EQ(store.find("C1002")->balance(), 12050);

    LoadStats blockStats;
    const auto blocked = loadBlocklist(kDataDir + "/blocked_merchants.txt", &blockStats);
    EXPECT_EQ(blocked.size(), 3u);  // comment line ignored
    EXPECT_EQ(blocked.count("M666"), 1u);

    const CityTable cities = loadCities(kDataDir + "/cities.csv");
    EXPECT_GE(cities.size(), 30u);
    EXPECT_TRUE(cities.contains("New York"));
}

TEST(Loaders, StreamsTransactionsAndCountsMalformedRows) {
    CsvTransactionSource source(kDataDir + "/transactions.csv");
    Transaction txn;
    std::size_t count = 0;
    while (source.next(txn)) {
        ++count;
    }
    EXPECT_EQ(count, 31u);
    EXPECT_EQ(source.stats().loaded, 31u);
    EXPECT_EQ(source.stats().malformed, 2u);
    EXPECT_EQ(source.stats().sampleErrors.size(), 2u);
}

TEST(Loaders, MissingFileThrowsIoError) {
    AccountStore store;
    EXPECT_THROW(loadAccounts(kDataDir + "/nope.csv", store), IoError);
    EXPECT_THROW(CsvTransactionSource(kDataDir + "/nope.csv"), IoError);
}

TEST(Loaders, DuplicateAccountsAreRejected) {
    const std::string path = ::testing::TempDir() + "payguard_dup_accounts.csv";
    {
        std::ofstream out(path);
        out << "card_id,balance,credit_limit,daily_limit,home_city,status\n"
               "C1,1.00,0,1.00,Pune,ACTIVE\n"
               "C1,2.00,0,1.00,Pune,ACTIVE\n"
               "\n# comment\n"
               "C2,3.00,0,1.00,Pune,BLOCKED\n";
    }
    AccountStore store;
    const LoadStats stats = loadAccounts(path, store);
    EXPECT_EQ(stats.rows, 3u);
    EXPECT_EQ(stats.loaded, 2u);
    EXPECT_EQ(stats.malformed, 1u);
    EXPECT_EQ(store.find("C1")->balance(), 100);  // first row wins
}

TEST(Loaders, LoadsLabels) {
    const std::string path = ::testing::TempDir() + "payguard_labels.csv";
    {
        std::ofstream out(path);
        out << "txn_id,is_fraud,fraud_type\nT1,0,\nT2,1,VELOCITY\nT3,2,GEO\nT4,1\n";
    }
    LoadStats stats;
    const LabelMap labels = loadLabels(path, &stats);
    EXPECT_EQ(labels.size(), 3u);
    EXPECT_EQ(stats.malformed, 1u);
    EXPECT_FALSE(labels.at("T1").fraud);
    EXPECT_TRUE(labels.at("T2").fraud);
    EXPECT_EQ(labels.at("T2").type, "VELOCITY");
    EXPECT_TRUE(labels.at("T4").type.empty());
}

// ---------------------------------------------------------------- Statistics

TEST(RunningStats, MatchesBruteForce) {
    std::mt19937 rng(7);
    std::lognormal_distribution<double> dist(6.0, 0.8);
    std::vector<double> values;
    RunningStats stats;
    for (int i = 0; i < 5000; ++i) {
        const double x = dist(rng);
        values.push_back(x);
        stats.add(x);

        if (i == 1 || i == 9 || i == 4999) {
            const double n = static_cast<double>(values.size());
            const double mean = std::accumulate(values.begin(), values.end(), 0.0) / n;
            double ss = 0.0;
            for (double v : values) {
                ss += (v - mean) * (v - mean);
            }
            EXPECT_NEAR(stats.mean(), mean, 1e-9 * mean);
            EXPECT_NEAR(stats.variance(), ss / (n - 1), 1e-9 * ss);
        }
    }
    EXPECT_EQ(stats.count(), 5000u);
}

TEST(RunningStats, EdgeCases) {
    RunningStats stats;
    EXPECT_EQ(stats.variance(), 0.0);
    stats.add(5.0);
    EXPECT_EQ(stats.mean(), 5.0);
    EXPECT_EQ(stats.variance(), 0.0);  // undefined with one sample; reported as 0
    stats.add(5.0);
    EXPECT_EQ(stats.stddev(), 0.0);
}

TEST(Latency, PercentilesMatchSortedReference) {
    std::mt19937 rng(3);
    std::exponential_distribution<double> dist(0.5);
    std::vector<double> samples(10'001);
    for (auto& s : samples) {
        s = dist(rng);
    }
    std::vector<double> sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    auto nearestRank = [&](double p) {
        return sorted[static_cast<std::size_t>(std::ceil(p * static_cast<double>(sorted.size()))) - 1];
    };

    const LatencySummary s = summarizeLatencies(samples);
    EXPECT_EQ(s.count, samples.size());
    EXPECT_DOUBLE_EQ(s.p50, nearestRank(0.50));
    EXPECT_DOUBLE_EQ(s.p95, nearestRank(0.95));
    EXPECT_DOUBLE_EQ(s.p99, nearestRank(0.99));
    EXPECT_DOUBLE_EQ(s.p999, nearestRank(0.999));
    EXPECT_DOUBLE_EQ(s.max, sorted.back());
}

TEST(Latency, EmptyAndSingle) {
    EXPECT_EQ(summarizeLatencies({}).count, 0u);
    const LatencySummary one = summarizeLatencies({4.2});
    EXPECT_DOUBLE_EQ(one.p50, 4.2);
    EXPECT_DOUBLE_EQ(one.p99, 4.2);
}

// ---------------------------------------------------------------- Queue

TEST(TransactionQueue, FifoAndCloseDrains) {
    TransactionQueue<int> queue(4);
    EXPECT_TRUE(queue.push(1));
    EXPECT_TRUE(queue.push(2));
    queue.close();
    EXPECT_FALSE(queue.push(3));
    EXPECT_EQ(queue.pop(), 1);
    EXPECT_EQ(queue.pop(), 2);
    EXPECT_EQ(queue.pop(), std::nullopt);
    EXPECT_THROW(TransactionQueue<int>(0), std::invalid_argument);
}

TEST(TransactionQueue, BlocksWhenFullUntilConsumed) {
    TransactionQueue<int> queue(2);
    queue.push(1);
    queue.push(2);
    std::atomic<bool> pushed{false};
    std::thread producer([&] {
        queue.push(3);  // must wait for space
        pushed = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(pushed.load());
    EXPECT_EQ(queue.pop(), 1);
    producer.join();
    EXPECT_TRUE(pushed.load());
    EXPECT_EQ(queue.size(), 2u);
}
