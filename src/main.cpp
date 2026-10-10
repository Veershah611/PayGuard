// PayGuard command-line driver: load inputs, run the pipeline, write reports.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>

#include "payguard/AccountStore.h"
#include "payguard/AlertQueue.h"
#include "payguard/AuditLogger.h"
#include "payguard/AuthorizationEngine.h"
#include "payguard/Config.h"
#include "payguard/Loaders.h"
#include "payguard/MetricsCollector.h"
#include "payguard/Pipeline.h"
#include "payguard/Report.h"
#include "payguard/RiskyCardTracker.h"
#include "payguard/StringUtil.h"
#include "payguard/TransactionSource.h"
#include "payguard/rules/RuleFactory.h"

namespace fs = std::filesystem;
using namespace payguard;

namespace {

struct Options {
    std::string dataDir = "data";
    std::optional<std::string> accounts;
    std::optional<std::string> transactions;
    std::optional<std::string> blocklist;
    std::optional<std::string> cities;
    std::optional<std::string> labels;
    std::string config = "config/rules.json";
    std::string outputDir = "output";
    std::optional<std::size_t> threads;
    std::size_t consoleAlerts = 10;
    std::size_t topK = 10;
    bool audit = true;
};

void printUsage() {
    std::cout <<
        R"(Usage: payguard [options]

Inputs (defaults are taken from --data-dir):
  --data-dir DIR        accounts.csv, transactions.csv, blocked_merchants.txt,
                        cities.csv and optional labels.csv        [data]
  --accounts FILE       accounts CSV
  --transactions FILE   transactions CSV
  --blocklist FILE      blocked merchant IDs, one per line
  --cities FILE         city,latitude,longitude table
  --labels FILE         ground truth (txn_id,is_fraud,fraud_type) for accuracy KPIs
  --config FILE         rule configuration                         [config/rules.json]

Run:
  --threads N           worker threads, 0 = all hardware threads   [from config]
  --output DIR          where audit.log, alerts.csv, metrics.txt go [output]
  --console-alerts N    print the first N fraud alerts              [10]
  --top-k K             risky cards listed in the report            [10]
  --no-audit            skip writing audit.log (pure benchmarking)
  -h, --help            show this help
)";
}

// Returns nullopt if the program should exit (help shown or bad arguments).
std::optional<Options> parseArgs(int argc, char** argv, int& exitCode) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() -> std::optional<std::string> {
            if (i + 1 >= argc) {
                std::cerr << "error: " << arg << " needs a value\n";
                return std::nullopt;
            }
            return std::string(argv[++i]);
        };
        auto number = [&]() -> std::optional<std::size_t> {
            const auto text = value();
            if (!text) {
                return std::nullopt;
            }
            const auto parsed = parseInt64(*text);
            if (!parsed || *parsed < 0) {
                std::cerr << "error: " << arg << " needs a non-negative integer\n";
                return std::nullopt;
            }
            return static_cast<std::size_t>(*parsed);
        };

        std::optional<std::string> text;
        std::optional<std::size_t> n;
        if (arg == "-h" || arg == "--help") {
            printUsage();
            exitCode = 0;
            return std::nullopt;
        } else if (arg == "--no-audit") {
            o.audit = false;
            continue;
        } else if (arg == "--threads" || arg == "--console-alerts" || arg == "--top-k") {
            if (!(n = number())) {
                exitCode = 2;
                return std::nullopt;
            }
            if (arg == "--threads") o.threads = *n;
            else if (arg == "--console-alerts") o.consoleAlerts = *n;
            else o.topK = *n;
            continue;
        }

        std::string* target = nullptr;
        std::optional<std::string>* optionalTarget = nullptr;
        if (arg == "--data-dir") target = &o.dataDir;
        else if (arg == "--config") target = &o.config;
        else if (arg == "--output") target = &o.outputDir;
        else if (arg == "--accounts") optionalTarget = &o.accounts;
        else if (arg == "--transactions") optionalTarget = &o.transactions;
        else if (arg == "--blocklist") optionalTarget = &o.blocklist;
        else if (arg == "--cities") optionalTarget = &o.cities;
        else if (arg == "--labels") optionalTarget = &o.labels;
        else {
            std::cerr << "error: unknown option '" << arg << "' (see --help)\n";
            exitCode = 2;
            return std::nullopt;
        }
        if (!(text = value())) {
            exitCode = 2;
            return std::nullopt;
        }
        if (target != nullptr) *target = *text;
        else *optionalTarget = *text;
    }
    return o;
}

// Explicit paths must exist; defaults derived from --data-dir are optional
// for the reference data (blocklist, cities, labels).
std::optional<std::string> resolveOptional(const std::optional<std::string>& explicitPath,
                                           const std::string& dataDir, const char* fileName) {
    if (explicitPath) {
        return explicitPath;
    }
    const fs::path candidate = fs::path(dataDir) / fileName;
    if (fs::exists(candidate)) {
        return candidate.string();
    }
    return std::nullopt;
}

int run(const Options& o) {
    // --- Configuration: fail fast on anything we do not understand.
    const Config config = Config::loadFile(o.config);

    const std::string accountsPath =
        o.accounts.value_or((fs::path(o.dataDir) / "accounts.csv").string());
    const std::string transactionsPath =
        o.transactions.value_or((fs::path(o.dataDir) / "transactions.csv").string());
    const auto blocklistPath = resolveOptional(o.blocklist, o.dataDir, "blocked_merchants.txt");
    const auto citiesPath = resolveOptional(o.cities, o.dataDir, "cities.csv");
    const auto labelsPath = resolveOptional(o.labels, o.dataDir, "labels.csv");

    // --- Reference data.
    AccountStore accounts;
    const LoadStats accountStats = loadAccounts(accountsPath, accounts);
    std::cout << "Loaded " << withThousands(accounts.size()) << " accounts from " << accountsPath;
    if (accountStats.malformed != 0) {
        std::cout << " (" << accountStats.malformed << " malformed rows skipped)";
    }
    std::cout << '\n';

    auto blocked = std::make_shared<const std::unordered_set<std::string>>(
        blocklistPath ? loadBlocklist(*blocklistPath) : std::unordered_set<std::string>{});
    auto cities = std::make_shared<const CityTable>(citiesPath ? loadCities(*citiesPath)
                                                               : CityTable{});
    std::cout << "Loaded " << blocked->size() << " blocked merchants, " << cities->size()
              << " cities\n";

    std::unique_ptr<LabelMap> labels;
    if (labelsPath) {
        labels = std::make_unique<LabelMap>(loadLabels(*labelsPath));
        std::cout << "Loaded " << withThousands(labels->size()) << " ground-truth labels\n";
    }

    // --- Engine.
    RuleFactory factory;
    auto rules = factory.createAll(config.rules, RuleContext{blocked, cities});
    EngineOptions engineOptions;
    engineOptions.thresholds = config.thresholds;
    engineOptions.declineUnknownCity = config.engine.declineUnknownCity;
    AuthorizationEngine engine(accounts, std::move(rules), engineOptions, cities);

    std::cout << "Rules enabled: ";
    const auto names = engine.ruleNames();
    for (std::size_t i = 0; i < names.size(); ++i) {
        std::cout << (i == 0 ? "" : ", ") << names[i];
    }
    std::cout << (names.empty() ? "(none)" : "") << '\n';

    std::size_t threads = o.threads.value_or(config.engine.threads);
    if (threads == 0) {
        threads = std::max(1u, std::thread::hardware_concurrency());
    }

    // --- Outputs and observers.
    fs::create_directories(o.outputDir);
    const fs::path outDir(o.outputDir);
    std::unique_ptr<AuditLogger> audit;
    if (o.audit) {
        audit = std::make_unique<AuditLogger>((outDir / "audit.log").string());
    }
    MetricsCollector metrics(threads, names, labels.get());
    AlertQueue alertQueue;
    RiskyCardTracker riskyCards;
    ConsoleAlertListener console(std::cout, o.consoleAlerts);

    PipelineOptions pipelineOptions;
    pipelineOptions.threads = threads;
    pipelineOptions.queueCapacity = config.engine.queueCapacity;
    pipelineOptions.batchSize = config.engine.batchSize;
    Pipeline pipeline(engine, metrics, pipelineOptions);
    pipeline.setAuditLogger(audit.get());
    pipeline.addListener(&alertQueue);
    pipeline.addListener(&riskyCards);
    pipeline.addListener(&console);

    // --- Run.
    const Money initialTotal = accounts.totalBalance();
    std::cout << "Processing " << transactionsPath << " with " << threads
              << " worker thread(s)...\n";
    CsvTransactionSource source(transactionsPath);
    const PipelineResult result = pipeline.run(source);
    if (audit) {
        audit->flush();
    }
    if (console.suppressed() != 0) {
        std::cout << "... " << withThousands(console.suppressed())
                  << " more fraud alerts (see audit.log / alerts.csv)\n";
    }

    // --- Reports.
    RunReport report;
    report.threads = threads;
    report.accounts = accounts.size();
    report.pipeline = result;
    report.transactionStats = source.stats();
    report.accountStats = accountStats;
    report.metrics = metrics.summarize();
    report.balance.initialTotal = initialTotal;
    report.balance.finalTotal = accounts.totalBalance();
    report.balance.approvedSpend = report.metrics.approvedSpend;
    report.balance.overdrawnAccounts = accounts.overdrawnCount();
    report.topCards = riskyCards.topK(o.topK);
    report.alertsQueued = alertQueue.size();

    {
        std::ofstream alertsFile(outDir / "alerts.csv");
        writeAlertsCsv(alertsFile, alertQueue.drainSorted());
    }
    {
        std::ofstream metricsFile(outDir / "metrics.txt");
        writeMetricsReport(metricsFile, report);
    }

    std::cout << '\n';
    writeMetricsReport(std::cout, report);
    std::cout << "\nWrote " << (o.audit ? "audit.log, " : "") << "alerts.csv and metrics.txt to "
              << outDir.string() << '\n';
    return report.balance.passed() ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    int exitCode = 0;
    const auto options = parseArgs(argc, argv, exitCode);
    if (!options) {
        return exitCode;
    }
    try {
        return run(*options);
    } catch (const ConfigError& e) {
        std::cerr << "config error: " << e.what() << '\n';
        return 2;
    } catch (const IoError& e) {
        std::cerr << "i/o error: " << e.what() << '\n';
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << '\n';
        return 1;
    }
}
