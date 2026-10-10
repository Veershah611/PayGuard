#pragma once

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "payguard/CityTable.h"
#include "payguard/rules/FraudRule.h"

namespace payguard {

// Shared reference data that rules may need.
struct RuleContext {
    std::shared_ptr<const std::unordered_set<std::string>> blockedMerchants;
    std::shared_ptr<const CityTable> cities;
};

// Read-only view of one rule's JSON entry with typed, validated accessors.
// It remembers which keys were read so the factory can reject typos such as
// "max_txn" instead of silently falling back to a default.
class RuleParams {
public:
    RuleParams(const nlohmann::json& entry, std::string type)
        : entry_(entry), type_(std::move(type)) {}

    double number(const char* key, double fallback) const;
    long long integer(const char* key, long long fallback) const;
    std::string text(const char* key, const std::string& fallback) const;
    const std::string& type() const { return type_; }
    const std::set<std::string>& usedKeys() const { return used_; }

private:
    const nlohmann::json& entry_;
    std::string type_;
    mutable std::set<std::string> used_;
};

// Builds the rule list from rules.json. Each rule type maps to a creator, so
// adding a rule means writing the class and registering one creator here.
// The engine itself never changes.
//
// rules.json entry: { "type": "velocity", "enabled": true, "score": 45, ...params }
class RuleFactory {
public:
    using Creator =
        std::function<std::unique_ptr<FraudRule>(const RuleParams&, const RuleContext&)>;

    RuleFactory();  // registers the built-in rules

    void registerRule(const std::string& type, Creator creator);
    std::vector<std::string> registeredTypes() const;

    // Throws ConfigError for unknown types, unknown keys or bad parameters.
    std::unique_ptr<FraudRule> create(const nlohmann::json& entry, const RuleContext& context) const;

    // Creates every enabled rule, in config order.
    std::vector<std::unique_ptr<FraudRule>> createAll(const nlohmann::json& rules,
                                                      const RuleContext& context) const;

private:
    std::map<std::string, Creator> creators_;
};

}  // namespace payguard
