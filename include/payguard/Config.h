#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace payguard {

// Raised for any problem in rules.json. The engine refuses to start rather
// than run with a half-understood configuration.
class ConfigError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct DecisionThresholds {
    double flag = 40.0;     // score >= flag    -> FLAG (approve, send to analyst)
    double decline = 70.0;  // score >= decline -> DECLINE (FRAUD_SUSPECTED)
};

struct EngineSettings {
    std::size_t threads = 0;  // 0 = std::thread::hardware_concurrency()
    std::size_t queueCapacity = 1024;  // batches per worker queue
    std::size_t batchSize = 64;        // transactions per queue item
    bool declineUnknownCity = true;    // decline cities missing from cities.csv
};

struct Config {
    DecisionThresholds thresholds;
    EngineSettings engine;
    nlohmann::json rules = nlohmann::json::array();  // handed to RuleFactory

    static Config fromJson(const nlohmann::json& root);
    static Config loadFile(const std::string& path);
};

}  // namespace payguard
