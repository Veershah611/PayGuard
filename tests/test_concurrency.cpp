// Stress tests for correctness under concurrency. Run them under
// ThreadSanitizer (-DPAYGUARD_SANITIZER=thread) to also catch data races.

#include <gtest/gtest.h>

#include <atomic>
#include <fstream>
#include <random>
#include <thread>
#include <vector>

#include "payguard/AccountStore.h"
#include "payguard/AuditLogger.h"
#include "payguard/AuthorizationEngine.h"
#include "payguard/MetricsCollector.h"
#include "payguard/Pipeline.h"
#include "payguard/TransactionQueue.h"
#include "payguard/rules/RuleFactory.h"

using namespace payguard;

namespace {

constexpr std::int64_t kT0 = 1'728'550'000;
constexpr int kThreads = 8;

template <typename Fn>
void runThreads(int count, Fn&& fn) {
    std::vector<std::thread> threads;
    for (int i = 0; i < count; ++i) {
        threads.emplace_back(fn, i);
    }
    for (auto& t : threads) {
        t.join();
    }
}

}  // namespace

TEST(Concurrency, ParallelDebitsNeverOverspend) {
    // 8 threads race to spend from one account; exactly balance/amount succeed.
    Account account("C1", rupees(1000), 0, rupees(1'000'000), "Pune", CardStatus::Active);
    std::atomic<int> successes{0};
    runThreads(kThreads, [&](int) {
        for (int i = 0; i < 1000; ++i) {
            if (account.debit(rupees(1), kT0) == FundsCheck::Ok) {
                ++successes;
            }
        }
    });
    EXPECT_EQ(successes.load(), 1000);
    EXPECT_EQ(account.balance(), 0);
}

TEST(Concurrency, EngineOnSharedCardsHasNoLostUpdates) {
    // Without sharding: every thread hits the same few cards, so the
    // check-and-debit must be atomic under the account lock.
    AccountStore store;
    const std::vector<std::string> cards = {"C1", "C2", "C3", "C4"};
    for (const auto& c : cards) {
        store.add(c, rupees(5000), 0, rupees(1'000'000), "Pune", CardStatus::Active);
    }
    AuthorizationEngine engine(store, {});
    std::atomic<std::int64_t> approvedSpend{0};
    std::atomic<int> approvals{0};

    runThreads(kThreads, [&](int worker) {
        Transaction t;
        t.merchantId = "M1";
        t.city = "Pune";
        t.amount = rupees(7);
        for (int i = 0; i < 2000; ++i) {
            t.id = "T" + std::to_string(worker) + "_" + std::to_string(i);
            t.cardId = cards[static_cast<std::size_t>(i) % cards.size()];
            t.timestamp = kT0 + i;
            if (engine.process(t).approved()) {
                approvedSpend += t.amount;
                ++approvals;
            }
        }
    });

    // 5000 / 7 = 714 approvals per card, no more and no fewer.
    EXPECT_EQ(approvals.load(), 714 * 4);
    EXPECT_EQ(rupees(5000) * 4 - approvedSpend.load(), store.totalBalance());
    for (const auto& c : cards) {
        EXPECT_GE(store.find(c)->balance(), 0);
    }
}

TEST(Concurrency, MillionRandomTransfersConserveMoney) {
    constexpr int kAccounts = 100;
    constexpr int kTransfers = 1'000'000;
    std::vector<std::unique_ptr<Account>> accounts;
    for (int i = 0; i < kAccounts; ++i) {
        accounts.push_back(std::make_unique<Account>("C" + std::to_string(i), rupees(100), 0, 0,
                                                     "Pune", CardStatus::Active));
    }
    std::atomic<int> ok{0};
    std::atomic<int> rejected{0};

    // Random pairs in both directions: a naive lock order would deadlock here.
    runThreads(kThreads, [&](int worker) {
        std::mt19937 rng(static_cast<unsigned>(worker) + 1);
        std::uniform_int_distribution<int> pick(0, kAccounts - 1);
        std::uniform_int_distribution<Money> amount(1, 500);
        for (int i = 0; i < kTransfers / kThreads; ++i) {
            const int a = pick(rng);
            const int b = pick(rng);
            if (a == b) {
                ++rejected;
                continue;
            }
            const auto r = Account::transfer(*accounts[static_cast<std::size_t>(a)],
                                             *accounts[static_cast<std::size_t>(b)], amount(rng));
            (r == TransferResult::Ok ? ok : rejected)++;
        }
    });

    EXPECT_EQ(ok.load() + rejected.load(), kTransfers);
    EXPECT_GT(ok.load(), kTransfers / 2);
    Money total = 0;
    for (const auto& acc : accounts) {
        EXPECT_GE(acc->balance(), 0) << acc->cardId();
        total += acc->balance();
    }
    EXPECT_EQ(total, rupees(100) * kAccounts);
}

TEST(Concurrency, ShardedPipelineMatchesSequentialRun) {
    // Card sharding preserves per-card order, so a multithreaded run must make
    // exactly the decisions a single-threaded run makes.
    std::mt19937 rng(99);
    constexpr int kCards = 300;
    const char* cityNames[] = {"Mumbai", "Delhi", "London"};
    auto cities = std::make_shared<CityTable>();
    cities->add("Mumbai", {19.0760, 72.8777});
    cities->add("Delhi", {28.6139, 77.2090});
    cities->add("London", {51.5074, -0.1278});
    auto blocked = std::make_shared<const std::unordered_set<std::string>>(
        std::unordered_set<std::string>{"M13"});

    std::vector<Transaction> txns;
    std::int64_t ts = kT0;
    std::uniform_int_distribution<int> card(0, kCards - 1);
    std::uniform_int_distribution<int> gap(0, 20);
    std::uniform_int_distribution<int> city(0, 2);
    std::uniform_int_distribution<int> merchant(1, 40);
    std::lognormal_distribution<double> amount(std::log(50'000.0), 0.9);
    for (int i = 0; i < 60'000; ++i) {
        Transaction t;
        t.id = "T" + std::to_string(i);
        t.cardId = "C" + std::to_string(card(rng));
        t.merchantId = "M" + std::to_string(merchant(rng));
        t.amount = i % 50 == 0 ? rupees(1000) : static_cast<Money>(amount(rng));
        t.city = cityNames[i % 10 == 0 ? city(rng) : 0];
        t.timestamp = (ts += gap(rng));
        txns.push_back(std::move(t));
    }

    const auto config = nlohmann::json::parse(R"([
        {"type": "velocity"}, {"type": "amount_anomaly"}, {"type": "geo_impossibility"},
        {"type": "blocked_merchant"}, {"type": "round_amount_burst"}])");
    auto build = [&](AccountStore& store) {
        for (int c = 0; c < kCards; ++c) {
            store.add("C" + std::to_string(c), rupees(20'000), rupees(5'000), rupees(8'000), "Mumbai",
                      CardStatus::Active);
        }
        RuleFactory factory;
        return std::make_unique<AuthorizationEngine>(
            store, factory.createAll(config, {blocked, cities}), EngineOptions{}, cities);
    };

    AccountStore sequentialStore;
    auto sequential = build(sequentialStore);
    MetricsCollector sequentialMetrics(1, sequential->ruleNames());
    for (const auto& t : txns) {
        sequentialMetrics.record(0, t, sequential->process(t), 0.0);
    }

    AccountStore parallelStore;
    auto parallel = build(parallelStore);
    const Money before = parallelStore.totalBalance();
    MetricsCollector parallelMetrics(kThreads, parallel->ruleNames());
    Pipeline pipeline(*parallel, parallelMetrics, {kThreads, 16, 32});
    VectorTransactionSource source(txns);
    EXPECT_EQ(pipeline.run(source).processed, txns.size());

    const auto s = sequentialMetrics.summarize();
    const auto p = parallelMetrics.summarize();
    EXPECT_EQ(p.approved, s.approved);
    EXPECT_EQ(p.flagged, s.flagged);
    EXPECT_EQ(p.declined, s.declined);
    EXPECT_EQ(p.declineReasons, s.declineReasons);
    EXPECT_EQ(p.ruleHits, s.ruleHits);
    EXPECT_GT(p.flagged + p.declineReasons[static_cast<std::size_t>(DeclineReason::FraudSuspected)], 0u);
    for (int c = 0; c < kCards; ++c) {
        const std::string id = "C" + std::to_string(c);
        ASSERT_EQ(parallelStore.find(id)->balance(), sequentialStore.find(id)->balance()) << id;
    }
    EXPECT_EQ(before - p.approvedSpend, parallelStore.totalBalance());
    EXPECT_EQ(parallelStore.overdrawnCount(), 0u);
}

TEST(Concurrency, QueueDeliversEveryItemExactlyOnce) {
    TransactionQueue<int> queue(64);
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 25'000;
    std::atomic<long long> sum{0};
    std::atomic<int> received{0};

    std::vector<std::thread> consumers;
    for (int i = 0; i < 4; ++i) {
        consumers.emplace_back([&] {
            while (auto item = queue.pop()) {
                sum += *item;
                ++received;
            }
        });
    }
    runThreads(kProducers, [&](int p) {
        for (int i = 1; i <= kPerProducer; ++i) {
            queue.push(p * kPerProducer + i);
        }
    });
    queue.close();
    for (auto& c : consumers) {
        c.join();
    }
    const long long n = kProducers * kPerProducer;
    EXPECT_EQ(received.load(), n);
    EXPECT_EQ(sum.load(), n * (n + 1) / 2);
}

TEST(Concurrency, AsyncAuditLoggerKeepsEveryLine) {
    const std::string path = ::testing::TempDir() + "payguard_concurrent_audit.log";
    {
        AuditLogger logger(path, AuditLogger::Mode::Async, 4096);
        runThreads(kThreads, [&](int worker) {
            for (int i = 0; i < 2000; ++i) {
                logger.write("w" + std::to_string(worker) + " " + std::to_string(i) + "\n");
            }
        });
        logger.flush();
        EXPECT_EQ(logger.linesWritten(), 16'000u);
    }
    std::ifstream in(path);
    std::string line;
    int lines = 0;
    while (std::getline(in, line)) {
        ++lines;
    }
    EXPECT_EQ(lines, 16'000);
}
