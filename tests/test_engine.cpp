#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <map>
#include <random>
#include <sstream>

#include "payguard/AlertQueue.h"
#include "payguard/AuditLogger.h"
#include "payguard/AuthorizationEngine.h"
#include "payguard/Config.h"
#include "payguard/Loaders.h"
#include "payguard/MetricsCollector.h"
#include "payguard/Pipeline.h"
#include "payguard/Report.h"
#include "payguard/RiskyCardTracker.h"
#include "payguard/TransactionSource.h"
#include "payguard/rules/RuleFactory.h"

using namespace payguard;

namespace {

const std::string kDataDir = PAYGUARD_TEST_DATA_DIR;
constexpr std::int64_t kT0 = 1'728'550'000;

// Test double: triggers with a fixed score and counts how often it runs.
class FixedScoreRule : public FraudRule {
public:
    explicit FixedScoreRule(double score, std::atomic<int>* calls = nullptr)
        : score_(score), calls_(calls) {}
    RuleResult evaluate(const Transaction&, const CardProfile&) const override {
        if (calls_ != nullptr) {
            ++*calls_;
        }
        return score_ > 0 ? RuleResult::hit(score_, name()) : RuleResult::pass();
    }
    std::string name() const override { return "FIXED"; }

private:
    double score_;
    std::atomic<int>* calls_;
};

std::vector<std::unique_ptr<FraudRule>> rulesOf(std::initializer_list<double> scores,
                                                std::atomic<int>* calls = nullptr) {
    std::vector<std::unique_ptr<FraudRule>> rules;
    for (double s : scores) {
        rules.push_back(std::make_unique<FixedScoreRule>(s, calls));
    }
    return rules;
}

Transaction txn(const std::string& card, Money amount, std::string city = "Pune",
                std::int64_t ts = kT0, std::string id = "T1") {
    Transaction t;
    t.id = std::move(id);
    t.cardId = card;
    t.merchantId = "M1";
    t.amount = amount;
    t.city = std::move(city);
    t.timestamp = ts;
    return t;
}

}  // namespace

// ---------------------------------------------------------------- Decision logic

TEST(Engine, ScoreThresholdsMapToVerdicts) {
    struct Case {
        double score;
        Verdict verdict;
        bool debited;
    };
    for (const Case c : {Case{0, Verdict::Approve, true}, Case{39.99, Verdict::Approve, true},
                         Case{40, Verdict::Flag, true}, Case{69.99, Verdict::Flag, true},
                         Case{70, Verdict::Decline, false}, Case{100, Verdict::Decline, false}}) {
        AccountStore store;
        store.add("C1", rupees(1000), 0, rupees(1000), "Pune", CardStatus::Active);
        AuthorizationEngine engine(store, rulesOf({c.score}));
        const Decision d = engine.process(txn("C1", rupees(100)));
        EXPECT_EQ(d.verdict, c.verdict) << "score " << c.score;
        EXPECT_DOUBLE_EQ(d.riskScore, c.score);
        EXPECT_EQ(store.find("C1")->balance(), c.debited ? rupees(900) : rupees(1000));
        if (c.verdict == Verdict::Decline) {
            EXPECT_EQ(d.reason, DeclineReason::FraudSuspected);
        }
    }
}

TEST(Engine, ScoresAreSummedAndClamped) {
    AccountStore store;
    store.add("C1", rupees(1000), 0, rupees(1000), "Pune", CardStatus::Active);
    AuthorizationEngine engine(store, rulesOf({25, 0, 30}));
    Decision d = engine.process(txn("C1", rupees(1)));
    EXPECT_DOUBLE_EQ(d.riskScore, 55);
    EXPECT_EQ(d.verdict, Verdict::Flag);
    EXPECT_EQ(d.ruleHits, 0b101u);
    EXPECT_EQ(d.reasons, (std::vector<std::string>{"FIXED", "FIXED"}));

    AuthorizationEngine heavy(store, rulesOf({80, 80}));
    d = heavy.process(txn("C1", rupees(1)));
    EXPECT_DOUBLE_EQ(d.riskScore, 100);
}

TEST(Engine, CustomThresholds) {
    AccountStore store;
    store.add("C1", rupees(1000), 0, rupees(1000), "Pune", CardStatus::Active);
    EngineOptions options;
    options.thresholds = {20, 50};
    AuthorizationEngine engine(store, rulesOf({30}), options);
    EXPECT_EQ(engine.process(txn("C1", 1)).verdict, Verdict::Flag);
}

TEST(Engine, HardChecksRunInOrderAndSkipRules) {
    std::atomic<int> calls{0};
    AccountStore store;
    store.add("ACTIVE", rupees(100), 0, rupees(50), "Pune", CardStatus::Active);
    store.add("BLOCKED", rupees(100), 0, rupees(50), "Pune", CardStatus::Blocked);
    auto cities = std::make_shared<CityTable>();
    cities->add("Pune", {18.52, 73.85});
    AuthorizationEngine engine(store, rulesOf({10}, &calls), {}, cities);

    EXPECT_EQ(engine.process(txn("NOPE", rupees(1))).reason, DeclineReason::UnknownCard);
    // Blocked is checked before the amount.
    EXPECT_EQ(engine.process(txn("BLOCKED", 0)).reason, DeclineReason::CardBlocked);
    EXPECT_EQ(engine.process(txn("ACTIVE", 0)).reason, DeclineReason::InvalidAmount);
    EXPECT_EQ(engine.process(txn("ACTIVE", -5)).reason, DeclineReason::InvalidAmount);
    EXPECT_EQ(engine.process(txn("ACTIVE", 1, "Atlantis")).reason, DeclineReason::UnknownCity);
    EXPECT_EQ(engine.process(txn("ACTIVE", rupees(101))).reason, DeclineReason::InsufficientFunds);
    EXPECT_EQ(engine.process(txn("ACTIVE", rupees(51))).reason, DeclineReason::DailyLimitExceeded);
    EXPECT_EQ(calls.load(), 0);
    EXPECT_EQ(store.find("ACTIVE")->balance(), rupees(100));

    const Decision ok = engine.process(txn("ACTIVE", rupees(50)));
    EXPECT_EQ(ok.verdict, Verdict::Approve);
    EXPECT_EQ(calls.load(), 1);
}

TEST(Engine, UnknownCityAllowedWhenDisabled) {
    AccountStore store;
    store.add("C1", rupees(100), 0, rupees(100), "Pune", CardStatus::Active);
    auto cities = std::make_shared<CityTable>();
    cities->add("Pune", {18.52, 73.85});
    EngineOptions options;
    options.declineUnknownCity = false;
    AuthorizationEngine engine(store, {}, options, cities);
    EXPECT_EQ(engine.process(txn("C1", 1, "Atlantis")).verdict, Verdict::Approve);
}

TEST(Engine, OnlyCleanApprovalsUpdateTheCardBaseline) {
    AccountStore store;
    store.add("C1", rupees(1000), 0, rupees(1000), "Pune", CardStatus::Active);
    AuthorizationEngine approve(store, rulesOf({0}));
    AuthorizationEngine flag(store, rulesOf({50}));
    Account& account = *store.find("C1");

    approve.process(txn("C1", rupees(10), "Pune", kT0));
    flag.process(txn("C1", rupees(20), "Delhi", kT0 + 60));
    {
        auto locked = account.lock();
        const CardProfile& p = locked.profile();
        EXPECT_EQ(p.lastCity(), "Pune");  // the flagged Delhi charge is not trusted
        EXPECT_EQ(p.lastTimestamp(), kT0);
        EXPECT_EQ(p.amountStats().count(), 1u);
        EXPECT_EQ(p.recentAttempts().size(), 2u);  // but both count for velocity
    }
    EXPECT_EQ(account.balance(), rupees(970));  // FLAG still debits
}

TEST(Engine, RejectsInvalidRuleSets) {
    AccountStore store;
    std::vector<std::unique_ptr<FraudRule>> withNull;
    withNull.push_back(nullptr);
    EXPECT_THROW(AuthorizationEngine(store, std::move(withNull)), std::invalid_argument);
    std::vector<std::unique_ptr<FraudRule>> tooMany;
    for (std::size_t i = 0; i <= AuthorizationEngine::kMaxRules; ++i) {
        tooMany.push_back(std::make_unique<FixedScoreRule>(0));
    }
    EXPECT_THROW(AuthorizationEngine(store, std::move(tooMany)), std::invalid_argument);
}

// ---------------------------------------------------------------- End-to-end on sample data

class SampleDataTest : public ::testing::Test {
protected:
    void SetUp() override {
        loadAccounts(kDataDir + "/accounts.csv", store_);
        blocked_ = std::make_shared<const std::unordered_set<std::string>>(
            loadBlocklist(kDataDir + "/blocked_merchants.txt"));
        cities_ = std::make_shared<const CityTable>(loadCities(kDataDir + "/cities.csv"));
        config_ = Config::loadFile(kDataDir + "/../../config/rules.json");
    }

    std::unique_ptr<AuthorizationEngine> makeEngine() {
        RuleFactory factory;
        EngineOptions options;
        options.thresholds = config_.thresholds;
        return std::make_unique<AuthorizationEngine>(
            store_, factory.createAll(config_.rules, {blocked_, cities_}), options, cities_);
    }

    AccountStore store_;
    std::shared_ptr<const std::unordered_set<std::string>> blocked_;
    std::shared_ptr<const CityTable> cities_;
    Config config_;
};

TEST_F(SampleDataTest, ProducesExpectedDecisions) {
    struct Expected {
        Verdict verdict;
        DeclineReason reason;
        std::vector<std::string> rules;
    };
    const auto approve = Expected{Verdict::Approve, DeclineReason::None, {}};
    std::map<std::string, Expected> expected = {
        {"T000001", approve},
        {"T000002", {Verdict::Flag, DeclineReason::None, {"GEO_IMPOSSIBLE"}}},
        {"T000003", {Verdict::Decline, DeclineReason::UnknownCard, {}}},
        {"T000004", {Verdict::Decline, DeclineReason::CardBlocked, {}}},
        {"T000005", {Verdict::Decline, DeclineReason::InvalidAmount, {}}},
        {"T000006", {Verdict::Decline, DeclineReason::InsufficientFunds, {}}},
        {"T000007", {Verdict::Decline, DeclineReason::DailyLimitExceeded, {}}},
        {"T000008", {Verdict::Decline, DeclineReason::FraudSuspected, {"BLOCKED_MERCHANT"}}},
        {"T000014", {Verdict::Flag, DeclineReason::None, {"VELOCITY"}}},
        {"T000015",
         {Verdict::Decline, DeclineReason::FraudSuspected, {"VELOCITY", "GEO_IMPOSSIBLE"}}},
        {"T000018", {Verdict::Flag, DeclineReason::None, {"ROUND_AMOUNT_BURST"}}},
        {"T000019", {Verdict::Decline, DeclineReason::UnknownCity, {}}},
        {"T000030", {Verdict::Flag, DeclineReason::None, {"AMOUNT_ANOMALY"}}},
    };

    auto engine = makeEngine();
    CsvTransactionSource source(kDataDir + "/transactions.csv");
    Transaction t;
    std::size_t processed = 0;
    while (source.next(t)) {
        const Decision d = engine->process(t);
        ++processed;
        const auto it = expected.find(t.id);
        const Expected& e = it == expected.end() ? approve : it->second;
        EXPECT_EQ(d.verdict, e.verdict) << t.id;
        EXPECT_EQ(d.reason, e.reason) << t.id;
        EXPECT_EQ(d.reasons, e.rules) << t.id;
    }
    EXPECT_EQ(processed, 31u);
    EXPECT_EQ(source.stats().malformed, 2u);

    EXPECT_EQ(store_.find("C1001")->balance(), -rupees(350));
    EXPECT_EQ(store_.find("C1002")->balance(), 12050);
    EXPECT_EQ(store_.find("C1003")->balance(), rupees(800));
    EXPECT_EQ(store_.find("C1004")->balance(), rupees(17'000));
    EXPECT_EQ(store_.find("C1005")->balance(), rupees(14'790));
    EXPECT_EQ(store_.find("C1006")->balance(), rupees(3000));
    EXPECT_EQ(store_.find("C1007")->balance(), rupees(39'480));
}

TEST_F(SampleDataTest, PipelineMatchesSequentialRunAndWritesOutputs) {
    auto engine = makeEngine();
    const std::size_t threads = 4;
    MetricsCollector metrics(threads, engine->ruleNames());
    const std::string auditPath = ::testing::TempDir() + "payguard_sample_audit.log";
    AlertQueue alerts;
    RiskyCardTracker risky;
    const Money before = store_.totalBalance();
    {
        AuditLogger audit(auditPath);
        Pipeline pipeline(*engine, metrics, {threads, 4, 3});
        pipeline.setAuditLogger(&audit);
        pipeline.addListener(&alerts);
        pipeline.addListener(&risky);
        CsvTransactionSource source(kDataDir + "/transactions.csv");
        const PipelineResult result = pipeline.run(source);
        EXPECT_EQ(result.processed, 31u);
        audit.flush();
        EXPECT_EQ(audit.linesWritten(), 31u);
    }

    const auto summary = metrics.summarize();
    EXPECT_EQ(summary.approved, 19u);
    EXPECT_EQ(summary.flagged, 4u);
    EXPECT_EQ(summary.declined, 8u);
    EXPECT_EQ(summary.declineReasons[static_cast<std::size_t>(DeclineReason::FraudSuspected)], 2u);
    EXPECT_EQ(before - summary.approvedSpend, store_.totalBalance());
    EXPECT_EQ(store_.find("C1005")->balance(), rupees(14'790));

    // Per-rule hit counters, in engine rule order.
    EXPECT_EQ(summary.ruleHits, (std::vector<std::uint64_t>{2, 1, 2, 1, 1}));

    // Analyst queue: flagged only, riskiest first.
    const auto sorted = alerts.drainSorted();
    ASSERT_EQ(sorted.size(), 4u);
    EXPECT_DOUBLE_EQ(sorted.front().riskScore, 45);
    EXPECT_DOUBLE_EQ(sorted.back().riskScore, 40);

    const auto top = risky.topK(1);
    ASSERT_EQ(top.size(), 1u);
    EXPECT_EQ(top[0].cardId, "C1005");  // velocity flag (45) + velocity/geo decline (90)

    std::ifstream in(auditPath);
    std::string line;
    std::size_t lines = 0;
    bool sawFlag = false;
    while (std::getline(in, line)) {
        ++lines;
        if (line.rfind("T000002 | C1001 | 4800.00 | FLAG | score=45 | GEO_IMPOSSIBLE | ", 0) == 0) {
            sawFlag = true;
        }
    }
    EXPECT_EQ(lines, 31u);
    EXPECT_TRUE(sawFlag);
}

// ---------------------------------------------------------------- Audit log

TEST(AuditLogger, FormatsLines) {
    Transaction t = txn("C1001", rupees(4800), "London", kT0, "T000002");
    Decision d;
    d.verdict = Verdict::Flag;
    d.riskScore = 55;
    d.reasons = {"GEO_IMPOSSIBLE", "AMOUNT_ANOMALY"};
    std::string out;
    AuditLogger::appendLine(out, t, d, 12.44);
    EXPECT_EQ(out, "T000002 | C1001 | 4800.00 | FLAG | score=55 | GEO_IMPOSSIBLE,AMOUNT_ANOMALY | 12.4us\n");

    Decision declined;
    declined.verdict = Verdict::Decline;
    declined.reason = DeclineReason::FraudSuspected;
    declined.riskScore = 90;
    declined.reasons = {"VELOCITY"};
    out.clear();
    AuditLogger::appendLine(out, t, declined, 0.31);
    EXPECT_EQ(out, "T000002 | C1001 | 4800.00 | DECLINE | score=90 | FRAUD_SUSPECTED,VELOCITY | 0.3us\n");

    out.clear();
    AuditLogger::appendLine(out, t, Decision{}, 1.0);
    EXPECT_EQ(out, "T000002 | C1001 | 4800.00 | APPROVE | score=0 | - | 1.0us\n");
}

TEST(AuditLogger, SyncAndAsyncModesWriteEverything) {
    for (auto mode : {AuditLogger::Mode::Sync, AuditLogger::Mode::Async}) {
        const std::string path = ::testing::TempDir() + "payguard_audit_mode.log";
        {
            AuditLogger logger(path, mode, 64);  // tiny buffer exercises back-pressure
            for (int i = 0; i < 200; ++i) {
                logger.write("line " + std::to_string(i) + "\nmore\n");
            }
            logger.write("");
            logger.flush();
            EXPECT_EQ(logger.linesWritten(), 400u);
        }
        std::ifstream in(path);
        std::string line;
        int count = 0;
        std::string first;
        while (std::getline(in, line)) {
            if (count++ == 0) {
                first = line;
            }
        }
        EXPECT_EQ(count, 400);
        EXPECT_EQ(first, "line 0");
    }
}

TEST(AuditLogger, UnwritablePathThrows) {
    EXPECT_THROW(AuditLogger(::testing::TempDir() + "no/such/dir/audit.log"), IoError);
}

// ---------------------------------------------------------------- Alerts and observers

TEST(AlertQueue, PopsRiskiestFirstAndKeepsArrivalOrderForTies) {
    AlertQueue queue;
    auto push = [&](const char* id, double score) {
        Alert a;
        a.txnId = id;
        a.riskScore = score;
        queue.push(a);
    };
    push("a", 50);
    push("b", 65);
    push("c", 50);
    push("d", 40);
    EXPECT_EQ(queue.size(), 4u);
    EXPECT_EQ(queue.pop()->txnId, "b");
    EXPECT_EQ(queue.pop()->txnId, "a");
    EXPECT_EQ(queue.pop()->txnId, "c");
    EXPECT_EQ(queue.pop()->txnId, "d");
    EXPECT_FALSE(queue.pop());
}

TEST(AlertQueue, OnlyQueuesFlags) {
    AlertQueue queue;
    Decision flag;
    flag.verdict = Verdict::Flag;
    Decision decline;
    decline.verdict = Verdict::Decline;
    decline.reason = DeclineReason::FraudSuspected;
    queue.onAlert(txn("C1", 1), flag);
    queue.onAlert(txn("C1", 1), decline);
    EXPECT_EQ(queue.size(), 1u);
}

TEST(RiskyCards, TopKMatchesFullSort) {
    std::mt19937 rng(11);
    std::uniform_int_distribution<int> score(0, 50);
    std::vector<RiskyCard> cards;
    for (int i = 0; i < 500; ++i) {
        RiskyCard c;
        c.cardId = "C" + std::to_string(i);
        c.totalScore = score(rng);  // many ties, exercising the tie-breakers
        c.alerts = static_cast<std::uint64_t>(score(rng) % 5);
        cards.push_back(c);
    }
    std::vector<RiskyCard> sorted = cards;
    std::sort(sorted.begin(), sorted.end(), [](const RiskyCard& a, const RiskyCard& b) {
        if (a.totalScore != b.totalScore) return a.totalScore > b.totalScore;
        if (a.alerts != b.alerts) return a.alerts > b.alerts;
        return a.cardId < b.cardId;
    });
    const auto top = selectTopK(cards, 10);
    ASSERT_EQ(top.size(), 10u);
    for (std::size_t i = 0; i < top.size(); ++i) {
        EXPECT_EQ(top[i].cardId, sorted[i].cardId);
    }
    EXPECT_TRUE(selectTopK(cards, 0).empty());
    EXPECT_EQ(selectTopK(std::vector<RiskyCard>(cards.begin(), cards.begin() + 3), 10).size(), 3u);
}

TEST(ConsoleAlerts, PrintsUpToLimitAndIgnoresHardDeclines) {
    std::ostringstream out;
    ConsoleAlertListener console(out, 1);
    Decision flag;
    flag.verdict = Verdict::Flag;
    flag.riskScore = 45;
    flag.reasons = {"VELOCITY"};
    Decision hard;
    hard.verdict = Verdict::Decline;
    hard.reason = DeclineReason::InsufficientFunds;

    console.onAlert(txn("C1", rupees(5)), hard);
    console.onAlert(txn("C1", rupees(5)), flag);
    console.onAlert(txn("C1", rupees(5)), flag);
    EXPECT_EQ(console.suppressed(), 1u);
    EXPECT_EQ(out.str(), "[ALERT] FLAG T1 card=C1 amount=5.00 city=Pune score=45 rules=VELOCITY\n");
}

// ---------------------------------------------------------------- Metrics and reports

TEST(Metrics, CountsDecisionsAndAccuracy) {
    LabelMap labels = {{"F1", {true, "GEO"}},  {"F2", {true, "GEO"}},  {"F3", {true, "VELOCITY"}},
                       {"L1", {false, ""}},    {"L2", {false, ""}},    {"L3", {false, ""}}};
    MetricsCollector metrics(2, {"R0", "R1"}, &labels);

    Decision approve;
    Decision flag;
    flag.verdict = Verdict::Flag;
    flag.ruleHits = 0b10;
    Decision hardDecline;
    hardDecline.verdict = Verdict::Decline;
    hardDecline.reason = DeclineReason::InsufficientFunds;
    Decision fraudDecline;
    fraudDecline.verdict = Verdict::Decline;
    fraudDecline.reason = DeclineReason::FraudSuspected;
    fraudDecline.ruleHits = 0b11;

    metrics.record(0, txn("C", 100, "Pune", kT0, "F1"), flag, 1.0);          // caught by rules
    metrics.record(1, txn("C", 100, "Pune", kT0, "F2"), hardDecline, 2.0);   // caught, hard check
    metrics.record(0, txn("C", 100, "Pune", kT0, "F3"), approve, 3.0);       // missed
    metrics.record(1, txn("C", 100, "Pune", kT0, "L1"), fraudDecline, 4.0);  // false positive
    metrics.record(0, txn("C", 100, "Pune", kT0, "L2"), hardDecline, 5.0);   // not a fraud alert
    metrics.record(1, txn("C", 100, "Pune", kT0, "L3"), approve, 6.0);
    metrics.record(0, txn("C", 100, "Pune", kT0, "X"), approve, 7.0);        // unlabeled

    const auto s = metrics.summarize();
    EXPECT_EQ(s.total, 7u);
    EXPECT_EQ(s.approved, 3u);
    EXPECT_EQ(s.flagged, 1u);
    EXPECT_EQ(s.declined, 3u);
    EXPECT_EQ(s.approvedSpend, 400);  // 3 approvals + 1 flag of 100 paise
    EXPECT_EQ(s.ruleHits, (std::vector<std::uint64_t>{1, 2}));
    EXPECT_EQ(s.latency.count, 7u);
    EXPECT_DOUBLE_EQ(s.latency.max, 7.0);

    EXPECT_EQ(s.accuracy.fraudTotal, 3u);
    EXPECT_EQ(s.accuracy.fraudCaught, 2u);
    EXPECT_EQ(s.accuracy.fraudAlerted, 1u);
    EXPECT_EQ(s.accuracy.legitTotal, 3u);
    EXPECT_EQ(s.accuracy.legitAlerted, 1u);
    EXPECT_EQ(s.accuracy.unlabeled, 1u);
    EXPECT_NEAR(s.accuracy.recall(), 2.0 / 3.0, 1e-12);
    EXPECT_NEAR(s.accuracy.falsePositiveRate(), 1.0 / 3.0, 1e-12);
    EXPECT_NEAR(s.accuracy.precision(), 0.5, 1e-12);
    EXPECT_EQ(s.byFraudType.at("GEO").caught, 2u);
    EXPECT_EQ(s.byFraudType.at("VELOCITY").caught, 0u);
}

TEST(Report, WritesSummaryAndAlertsCsv) {
    RunReport report;
    report.threads = 2;
    report.pipeline.processed = 10;
    report.pipeline.wallSeconds = 0.5;
    report.metrics.total = 10;
    report.metrics.approved = 10;
    report.balance.initialTotal = 1000;
    report.balance.approvedSpend = 400;
    report.balance.finalTotal = 600;
    std::ostringstream out;
    writeMetricsReport(out, report);
    EXPECT_NE(out.str().find("Throughput             : 20 txns/sec"), std::string::npos);
    EXPECT_NE(out.str().find("Balance check          : PASSED"), std::string::npos);

    report.balance.finalTotal = 599;
    std::ostringstream failed;
    writeMetricsReport(failed, report);
    EXPECT_NE(failed.str().find("Balance check          : FAILED"), std::string::npos);

    Alert a;
    a.txnId = "T1";
    a.cardId = "C1";
    a.merchantId = "M1";
    a.city = "Pune";
    a.amount = rupees(12);
    a.timestamp = 5;
    a.riskScore = 45;
    a.reasons = {"VELOCITY", "GEO_IMPOSSIBLE"};
    std::ostringstream csv;
    writeAlertsCsv(csv, {a});
    EXPECT_EQ(csv.str(),
              "rank,txn_id,card_id,merchant_id,amount,city,timestamp,risk_score,reasons\n"
              "1,T1,C1,M1,12.00,Pune,5,45,VELOCITY;GEO_IMPOSSIBLE\n");
}
