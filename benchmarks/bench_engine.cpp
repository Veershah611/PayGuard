// Performance benchmarks.
//
//   BM_EngineProcess/<cards>   single-threaded decision cost as the number of
//                              cards grows: should stay flat (O(1) lookups)
//   BM_Pipeline/<threads>      end-to-end throughput through the sharded
//                              pipeline for 1..N worker threads
//   BM_Rule*                   cost of individual fraud rules

#include <benchmark/benchmark.h>

#include <algorithm>
#include <map>
#include <memory>
#include <random>
#include <thread>

#include "payguard/AccountStore.h"
#include "payguard/AuthorizationEngine.h"
#include "payguard/MetricsCollector.h"
#include "payguard/Pipeline.h"
#include "payguard/rules/RuleFactory.h"
#include "payguard/rules/VelocityRule.h"

using namespace payguard;

namespace {

constexpr std::int64_t kT0 = 1'728'550'000;

const char* const kCityNames[] = {"Mumbai", "Delhi", "Pune", "Bengaluru", "London"};

std::shared_ptr<const CityTable> benchCities() {
    auto table = std::make_shared<CityTable>();
    table->add("Mumbai", {19.0760, 72.8777});
    table->add("Delhi", {28.6139, 77.2090});
    table->add("Pune", {18.5204, 73.8567});
    table->add("Bengaluru", {12.9716, 77.5946});
    table->add("London", {51.5074, -0.1278});
    return table;
}

std::string cardId(std::size_t i) { return "C" + std::to_string(10'000'000 + i); }

// A world of `cards` accounts with effectively unlimited funds, the default
// rule set, and a pre-generated transaction stream that touches random cards.
struct World {
    explicit World(std::size_t cards, std::size_t transactions = 1'000'000) {
        store.reserve(cards);
        for (std::size_t i = 0; i < cards; ++i) {
            store.add(cardId(i), rupees(1'000'000'000), 0, rupees(1'000'000'000), "Mumbai",
                      CardStatus::Active);
        }
        cities = benchCities();
        blocked = std::make_shared<const std::unordered_set<std::string>>(
            std::unordered_set<std::string>{"M666"});
        const auto config = nlohmann::json::parse(R"([
            {"type": "velocity"}, {"type": "amount_anomaly"}, {"type": "geo_impossibility"},
            {"type": "blocked_merchant"}, {"type": "round_amount_burst"}])");
        RuleFactory factory;
        engine = std::make_unique<AuthorizationEngine>(
            store, factory.createAll(config, {blocked, cities}), EngineOptions{}, cities);

        std::mt19937_64 rng(1234);
        std::uniform_int_distribution<std::size_t> card(0, cards - 1);
        std::lognormal_distribution<double> amount(std::log(80'000.0), 0.6);
        std::uniform_int_distribution<int> merchant(1, 5000);
        txns.reserve(transactions);
        for (std::size_t i = 0; i < transactions; ++i) {
            Transaction t;
            t.id = "T" + std::to_string(i);
            t.cardId = cardId(card(rng));
            t.merchantId = "M" + std::to_string(merchant(rng));
            t.amount = static_cast<Money>(amount(rng)) + 1;
            t.city = kCityNames[i % 97 == 0 ? i % 5 : 0];
            t.timestamp = kT0 + static_cast<std::int64_t>(i / 20);
            txns.push_back(std::move(t));
        }
    }

    AccountStore store;
    std::shared_ptr<const CityTable> cities;
    std::shared_ptr<const std::unordered_set<std::string>> blocked;
    std::unique_ptr<AuthorizationEngine> engine;
    std::vector<Transaction> txns;
};

World& worldFor(std::size_t cards) {
    static std::map<std::size_t, std::unique_ptr<World>> worlds;
    auto& world = worlds[cards];
    if (!world) {
        world = std::make_unique<World>(cards);
    }
    return *world;
}

}  // namespace

static void BM_EngineProcess(benchmark::State& state) {
    World& world = worldFor(static_cast<std::size_t>(state.range(0)));
    std::size_t i = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(world.engine->process(world.txns[i]));
        if (++i == world.txns.size()) {
            i = 0;
        }
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_EngineProcess)->Arg(10'000)->Arg(100'000)->Arg(1'000'000);

static void BM_Pipeline(benchmark::State& state) {
    World& world = worldFor(100'000);
    const auto threads = static_cast<std::size_t>(state.range(0));
    for (auto _ : state) {
        MetricsCollector metrics(threads, world.engine->ruleNames(), nullptr, world.txns.size());
        Pipeline pipeline(*world.engine, metrics, {threads, 1024, 64});
        VectorTransactionSource source(world.txns);
        const auto result = pipeline.run(source);
        benchmark::DoNotOptimize(result.processed);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(world.txns.size()));
}
BENCHMARK(BM_Pipeline)
    ->Arg(1)
    ->Arg(2)
    ->Arg(4)
    ->Arg(8)
    ->Arg(static_cast<std::int64_t>(std::max(1u, std::thread::hardware_concurrency())))
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime()
    ->Iterations(3);

static void BM_RuleVelocity(benchmark::State& state) {
    VelocityRule rule(5, 60, 45);
    CardProfile profile;
    for (int i = 0; i < 4; ++i) {
        profile.recordAttempt(kT0 + i, 100);
    }
    Transaction t;
    t.timestamp = kT0 + 10;
    for (auto _ : state) {
        benchmark::DoNotOptimize(rule.evaluate(t, profile));
    }
}
BENCHMARK(BM_RuleVelocity);

static void BM_RuleGeo(benchmark::State& state) {
    RuleFactory factory;
    auto rule = factory.create(nlohmann::json::parse(R"({"type": "geo_impossibility"})"),
                               {nullptr, benchCities()});
    CardProfile profile;
    profile.recordLocation("Mumbai", kT0);
    Transaction t;
    t.city = "Delhi";
    t.timestamp = kT0 + 7200;
    for (auto _ : state) {
        benchmark::DoNotOptimize(rule->evaluate(t, profile));
    }
}
BENCHMARK(BM_RuleGeo);

BENCHMARK_MAIN();
