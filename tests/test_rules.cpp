#include <gtest/gtest.h>

#include <cmath>

#include "payguard/CardProfile.h"
#include "payguard/Config.h"
#include "payguard/rules/AmountAnomalyRule.h"
#include "payguard/rules/BlocklistRule.h"
#include "payguard/rules/GeoImpossibilityRule.h"
#include "payguard/rules/RoundAmountBurstRule.h"
#include "payguard/rules/RuleFactory.h"
#include "payguard/rules/VelocityRule.h"

using namespace payguard;
using nlohmann::json;

namespace {

constexpr std::int64_t kT0 = 1'728'550'000;

Transaction txnAt(std::int64_t ts, Money amount = rupees(100), std::string city = "Mumbai",
                  std::string merchant = "M1") {
    Transaction t;
    t.id = "T";
    t.cardId = "C1";
    t.merchantId = std::move(merchant);
    t.amount = amount;
    t.city = std::move(city);
    t.timestamp = ts;
    return t;
}

std::shared_ptr<const CityTable> cities() {
    auto table = std::make_shared<CityTable>();
    table->add("Mumbai", {19.0760, 72.8777});
    table->add("Pune", {18.5204, 73.8567});  // ~120 km from Mumbai
    table->add("Delhi", {28.6139, 77.2090});  // ~1150 km from Mumbai
    table->add("London", {51.5074, -0.1278});
    return table;
}

}  // namespace

// ---------------------------------------------------------------- CardProfile

TEST(CardProfile, SlidingWindowDropsExpiredAttempts) {
    CardProfile p;
    p.recordAttempt(kT0, 1);
    p.recordAttempt(kT0 + 10, 1);
    p.recordAttempt(kT0 + CardProfile::kRetentionSeconds + 5, 1);  // evicts the first
    EXPECT_EQ(p.recentAttempts().size(), 2u);
    EXPECT_EQ(p.recentAttempts().front().timestamp, kT0 + 10);
}

TEST(CardProfile, WindowIsBoundedInSize) {
    CardProfile p;
    for (std::size_t i = 0; i < CardProfile::kMaxRecentAttempts + 50; ++i) {
        p.recordAttempt(kT0, 1);  // all in the same second
    }
    EXPECT_EQ(p.recentAttempts().size(), CardProfile::kMaxRecentAttempts);
}

TEST(CardProfile, CountsAttemptsSinceWithEarlyStop) {
    CardProfile p;
    for (int i = 0; i < 10; ++i) {
        p.recordAttempt(kT0 + i * 10, 1);
    }
    EXPECT_EQ(p.countAttemptsSince(kT0 + 50, 100), 5u);
    EXPECT_EQ(p.countAttemptsSince(kT0, 3), 3u);
    EXPECT_EQ(p.countAttemptsSince(kT0 + 1000, 100), 0u);
}

TEST(CardProfile, TracksApprovedAmountsInBothScales) {
    CardProfile p;
    p.recordApprovedAmount(100);
    p.recordApprovedAmount(300);
    p.recordApprovedAmount(0);  // ignored
    EXPECT_EQ(p.amountStats().count(), 2u);
    EXPECT_DOUBLE_EQ(p.amountStats().mean(), 200.0);
    EXPECT_NEAR(p.logAmountStats().mean(), (std::log(100.0) + std::log(300.0)) / 2, 1e-12);
}

// ---------------------------------------------------------------- F1 Velocity

TEST(VelocityRule, TriggersOnSixthTransactionWithinWindow) {
    VelocityRule rule(5, 60, 45);
    CardProfile p;
    for (int i = 0; i < 5; ++i) {
        const Transaction t = txnAt(kT0 + i * 10);
        EXPECT_FALSE(rule.evaluate(t, p).triggered) << "attempt " << i + 1;
        p.recordAttempt(t.timestamp, t.amount);
    }
    const RuleResult r = rule.evaluate(txnAt(kT0 + 50), p);
    EXPECT_TRUE(r.triggered);
    EXPECT_DOUBLE_EQ(r.score, 45);
    EXPECT_EQ(r.reason, "VELOCITY");
}

TEST(VelocityRule, WindowBoundaryIsExclusiveOfOldestSecond) {
    VelocityRule rule(5, 60, 45);
    CardProfile p;
    for (int i = 0; i < 5; ++i) {
        p.recordAttempt(kT0 + i, 1);  // kT0 .. kT0+4
    }
    // At kT0+60 the attempt at kT0 is exactly 60 s old: outside (now-60, now].
    EXPECT_FALSE(rule.evaluate(txnAt(kT0 + 60), p).triggered);
    EXPECT_TRUE(rule.evaluate(txnAt(kT0 + 59), p).triggered);
}

TEST(VelocityRule, RejectsBadParameters) {
    EXPECT_THROW(VelocityRule(0, 60, 1), std::invalid_argument);
    EXPECT_THROW(VelocityRule(5, 0, 1), std::invalid_argument);
    EXPECT_THROW(VelocityRule(5, CardProfile::kRetentionSeconds + 1, 1), std::invalid_argument);
    EXPECT_THROW(VelocityRule(static_cast<int>(CardProfile::kMaxRecentAttempts), 60, 1),
                 std::invalid_argument);
}

// ---------------------------------------------------------------- F2 Amount anomaly

TEST(AmountAnomalyRule, NeedsMinimumHistory) {
    AmountAnomalyRule rule(10, 3.0, 40, AmountAnomalyRule::Scale::Linear);
    CardProfile p;
    for (int i = 0; i < 9; ++i) {
        p.recordApprovedAmount(rupees(100 + i));
    }
    EXPECT_FALSE(rule.evaluate(txnAt(kT0, rupees(1'000'000)), p).triggered);
    p.recordApprovedAmount(rupees(100));
    EXPECT_TRUE(rule.evaluate(txnAt(kT0, rupees(1'000'000)), p).triggered);
}

TEST(AmountAnomalyRule, LinearThresholdIsMeanPlusKStdDev) {
    AmountAnomalyRule rule(2, 3.0, 40, AmountAnomalyRule::Scale::Linear);
    CardProfile p;
    for (Money m : {Money{900}, Money{1000}, Money{1100}}) {
        p.recordApprovedAmount(m);  // mean 1000, sample sd 100 -> threshold 1300
    }
    EXPECT_FALSE(rule.evaluate(txnAt(kT0, 1300), p).triggered);
    EXPECT_TRUE(rule.evaluate(txnAt(kT0, 1301), p).triggered);
}

TEST(AmountAnomalyRule, LogScaleUsesLogMoments) {
    AmountAnomalyRule rule(2, 2.0, 40, AmountAnomalyRule::Scale::Log);
    CardProfile p;
    for (Money m : {Money{100}, Money{1000}, Money{10000}}) {
        p.recordApprovedAmount(m);  // logs evenly spaced: mean ln(1000), sd ln(10)
    }
    // Threshold: exp(ln 1000 + 2 ln 10) = 100,000.
    EXPECT_FALSE(rule.evaluate(txnAt(kT0, 99'000), p).triggered);
    EXPECT_TRUE(rule.evaluate(txnAt(kT0, 101'000), p).triggered);
}

TEST(AmountAnomalyRule, LogScaleIgnoresHeavyTailThatLinearFlags) {
    // Spending that varies multiplicatively (half, typical, double). In linear
    // space mean + 3sd is ~3,070, so a 3,500 purchase looks anomalous; in log
    // space the threshold is ~5,600 and only a genuinely large amount fires.
    CardProfile p;
    const Money cycle[] = {rupees(500), rupees(1000), rupees(2000)};
    for (int i = 0; i < 30; ++i) {
        p.recordApprovedAmount(cycle[i % 3]);
    }
    AmountAnomalyRule log(10, 3.0, 40, AmountAnomalyRule::Scale::Log);
    AmountAnomalyRule linear(10, 3.0, 40, AmountAnomalyRule::Scale::Linear);
    EXPECT_TRUE(linear.evaluate(txnAt(kT0, rupees(3500)), p).triggered);
    EXPECT_FALSE(log.evaluate(txnAt(kT0, rupees(3500)), p).triggered);
    EXPECT_TRUE(log.evaluate(txnAt(kT0, rupees(20'000)), p).triggered);
}

// ---------------------------------------------------------------- F3 Geo

TEST(GeoImpossibilityRule, FlagsImpossibleTravel) {
    GeoImpossibilityRule rule(cities(), 900, 100, 45);
    CardProfile p;
    p.recordLocation("Mumbai", kT0);
    // ~7,190 km in 1 hour.
    EXPECT_TRUE(rule.evaluate(txnAt(kT0 + 3600, 1, "London"), p).triggered);
    // ~7,190 km in 10 hours = ~719 km/h: plausible by air.
    EXPECT_FALSE(rule.evaluate(txnAt(kT0 + 36'000, 1, "London"), p).triggered);
}

TEST(GeoImpossibilityRule, SpeedBoundary) {
    GeoImpossibilityRule rule(cities(), 900, 100, 45);
    CardProfile p;
    p.recordLocation("Mumbai", kT0);
    const double km = haversineKm(*cities()->find("Mumbai"), *cities()->find("Delhi"));
    const auto atLimit = static_cast<std::int64_t>(std::ceil(km / 900.0 * 3600.0));
    EXPECT_FALSE(rule.evaluate(txnAt(kT0 + atLimit, 1, "Delhi"), p).triggered);
    EXPECT_TRUE(rule.evaluate(txnAt(kT0 + atLimit - 60, 1, "Delhi"), p).triggered);
}

TEST(GeoImpossibilityRule, IgnoresSameCityShortHopsAndUnknowns) {
    GeoImpossibilityRule rule(cities(), 900, 200, 45);
    CardProfile p;
    EXPECT_FALSE(rule.evaluate(txnAt(kT0, 1, "London"), p).triggered);  // no history yet
    p.recordLocation("Mumbai", kT0);
    EXPECT_FALSE(rule.evaluate(txnAt(kT0 + 1, 1, "Mumbai"), p).triggered);
    EXPECT_FALSE(rule.evaluate(txnAt(kT0 + 60, 1, "Pune"), p).triggered);  // < min distance
    EXPECT_FALSE(rule.evaluate(txnAt(kT0 + 60, 1, "Atlantis"), p).triggered);
}

TEST(GeoImpossibilityRule, SimultaneousOrBackwardsTimeIsImpossible) {
    GeoImpossibilityRule rule(cities(), 900, 100, 45);
    CardProfile p;
    p.recordLocation("Mumbai", kT0);
    EXPECT_TRUE(rule.evaluate(txnAt(kT0, 1, "Delhi"), p).triggered);
    EXPECT_TRUE(rule.evaluate(txnAt(kT0 - 10, 1, "Delhi"), p).triggered);
}

// ---------------------------------------------------------------- F4 Blocklist

TEST(BlocklistRule, ExactMatchOnly) {
    auto blocked = std::make_shared<const std::unordered_set<std::string>>(
        std::unordered_set<std::string>{"M666"});
    BlocklistRule rule(blocked, 80);
    CardProfile p;
    const RuleResult hit = rule.evaluate(txnAt(kT0, 1, "Mumbai", "M666"), p);
    EXPECT_TRUE(hit.triggered);
    EXPECT_DOUBLE_EQ(hit.score, 80);
    EXPECT_EQ(hit.reason, "BLOCKED_MERCHANT");
    EXPECT_FALSE(rule.evaluate(txnAt(kT0, 1, "Mumbai", "M6666"), p).triggered);
    EXPECT_FALSE(rule.evaluate(txnAt(kT0, 1, "Mumbai", "m666"), p).triggered);
}

// ---------------------------------------------------------------- F5 Round amounts

TEST(RoundAmountBurstRule, ThirdRoundAmountInWindowTriggers) {
    RoundAmountBurstRule rule(3, 600, rupees(1000), 40);
    CardProfile p;
    p.recordAttempt(kT0, rupees(1000));
    p.recordAttempt(kT0 + 60, rupees(250));  // not round, does not count
    EXPECT_FALSE(rule.evaluate(txnAt(kT0 + 120, rupees(2000)), p).triggered);
    p.recordAttempt(kT0 + 120, rupees(2000));
    EXPECT_TRUE(rule.evaluate(txnAt(kT0 + 180, rupees(5000)), p).triggered);
    // Current amount not round: never triggers.
    EXPECT_FALSE(rule.evaluate(txnAt(kT0 + 180, rupees(5000) + 1), p).triggered);
}

TEST(RoundAmountBurstRule, OldRoundAmountsFallOutOfWindow) {
    RoundAmountBurstRule rule(3, 600, rupees(1000), 40);
    CardProfile p;
    p.recordAttempt(kT0, rupees(1000));
    p.recordAttempt(kT0 + 10, rupees(1000));
    EXPECT_TRUE(rule.evaluate(txnAt(kT0 + 599, rupees(1000)), p).triggered);
    EXPECT_FALSE(rule.evaluate(txnAt(kT0 + 600, rupees(1000)), p).triggered);
}

// ---------------------------------------------------------------- Factory and config

TEST(RuleFactory, BuildsDefaultConfigInOrder) {
    const Config config = Config::loadFile(std::string(PAYGUARD_TEST_DATA_DIR) +
                                           "/../../config/rules.json");
    RuleContext ctx;
    ctx.cities = cities();
    RuleFactory factory;
    const auto rules = factory.createAll(config.rules, ctx);
    ASSERT_EQ(rules.size(), 5u);
    EXPECT_EQ(rules[0]->name(), "VELOCITY");
    EXPECT_EQ(rules[1]->name(), "AMOUNT_ANOMALY");
    EXPECT_EQ(rules[2]->name(), "GEO_IMPOSSIBLE");
    EXPECT_EQ(rules[3]->name(), "BLOCKED_MERCHANT");
    EXPECT_EQ(rules[4]->name(), "ROUND_AMOUNT_BURST");
    EXPECT_DOUBLE_EQ(config.thresholds.flag, 40);
    EXPECT_DOUBLE_EQ(config.thresholds.decline, 70);
}

TEST(RuleFactory, AppliesParametersAndSkipsDisabledRules) {
    RuleFactory factory;
    const json rules = json::parse(R"([
        {"type": "velocity", "max_txns": 2, "window_seconds": 10, "score": 7},
        {"type": "blocked_merchant", "enabled": false}
    ])");
    const auto created = factory.createAll(rules, RuleContext{});
    ASSERT_EQ(created.size(), 1u);

    CardProfile p;
    p.recordAttempt(kT0, 1);
    EXPECT_FALSE(created[0]->evaluate(txnAt(kT0 + 1), p).triggered);
    p.recordAttempt(kT0 + 1, 1);
    const RuleResult r = created[0]->evaluate(txnAt(kT0 + 2), p);
    EXPECT_TRUE(r.triggered);
    EXPECT_DOUBLE_EQ(r.score, 7);
}

TEST(RuleFactory, RejectsBadConfig) {
    RuleFactory factory;
    const RuleContext ctx;
    auto make = [&](const char* text) { return factory.create(json::parse(text), ctx); };
    EXPECT_THROW(make(R"({"type": "teleport"})"), ConfigError);
    EXPECT_THROW(make(R"({"enabled": true})"), ConfigError);
    EXPECT_THROW(make(R"({"type": "velocity", "max_txn": 3})"), ConfigError);  // typo
    EXPECT_THROW(make(R"({"type": "velocity", "max_txns": "five"})"), ConfigError);
    EXPECT_THROW(make(R"({"type": "velocity", "max_txns": 2.5})"), ConfigError);
    EXPECT_THROW(make(R"({"type": "velocity", "max_txns": 0})"), ConfigError);
    EXPECT_THROW(make(R"({"type": "amount_anomaly", "scale": "cubic"})"), ConfigError);
    EXPECT_THROW(make(R"({"type": "geo_impossibility"})"), ConfigError);  // no city table
    EXPECT_THROW(factory.createAll(json::parse(R"([{"type": "velocity", "enabled": "yes"}])"), ctx),
                 ConfigError);
    EXPECT_THROW(factory.createAll(json::object(), ctx), ConfigError);
}

TEST(RuleFactory, NewRuleTypesPlugIn) {
    class AlwaysRule : public FraudRule {
    public:
        RuleResult evaluate(const Transaction&, const CardProfile&) const override {
            return RuleResult::hit(1, name());
        }
        std::string name() const override { return "ALWAYS"; }
    };
    RuleFactory factory;
    factory.registerRule("always", [](const RuleParams&, const RuleContext&) {
        return std::make_unique<AlwaysRule>();
    });
    EXPECT_THROW(factory.registerRule("always", nullptr), std::logic_error);
    const auto rule = factory.create(json::parse(R"({"type": "always"})"), RuleContext{});
    EXPECT_EQ(rule->name(), "ALWAYS");
}

TEST(Config, ValidatesThresholdsAndSections) {
    EXPECT_NO_THROW(Config::fromJson(json::parse(R"({})")));
    EXPECT_THROW(Config::fromJson(json::parse(R"([])")), ConfigError);
    EXPECT_THROW(Config::fromJson(json::parse(R"({"decision": {"flag_threshold": 80, "decline_threshold": 70}})")),
                 ConfigError);
    EXPECT_THROW(Config::fromJson(json::parse(R"({"decision": {"decline_threshold": 101}})")),
                 ConfigError);
    EXPECT_THROW(Config::fromJson(json::parse(R"({"decision": {"flag_threshold": "high"}})")),
                 ConfigError);
    EXPECT_THROW(Config::fromJson(json::parse(R"({"engine": {"queue_capacity": 0}})")),
                 ConfigError);
    EXPECT_THROW(Config::fromJson(json::parse(R"({"rules": {}})")), ConfigError);
    EXPECT_THROW(Config::loadFile("does/not/exist.json"), ConfigError);

    const Config c = Config::fromJson(json::parse(
        R"({"engine": {"threads": 3, "decline_unknown_city": false}, "decision": {"flag_threshold": 30}})"));
    EXPECT_EQ(c.engine.threads, 3u);
    EXPECT_FALSE(c.engine.declineUnknownCity);
    EXPECT_DOUBLE_EQ(c.thresholds.flag, 30);
    EXPECT_DOUBLE_EQ(c.thresholds.decline, 70);
}
