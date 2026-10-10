#include "payguard/rules/RuleFactory.h"

#include <stdexcept>

#include "payguard/Config.h"
#include "payguard/rules/AmountAnomalyRule.h"
#include "payguard/rules/BlocklistRule.h"
#include "payguard/rules/GeoImpossibilityRule.h"
#include "payguard/rules/RoundAmountBurstRule.h"
#include "payguard/rules/VelocityRule.h"

namespace payguard {

namespace {

using nlohmann::json;

std::string where(const std::string& type, const char* key) {
    return "rule '" + type + "', parameter '" + key + "'";
}

}  // namespace

double RuleParams::number(const char* key, double fallback) const {
    used_.insert(key);
    const auto it = entry_.find(key);
    if (it == entry_.end()) {
        return fallback;
    }
    if (!it->is_number()) {
        throw ConfigError(where(type_, key) + " must be a number");
    }
    return it->get<double>();
}

long long RuleParams::integer(const char* key, long long fallback) const {
    used_.insert(key);
    const auto it = entry_.find(key);
    if (it == entry_.end()) {
        return fallback;
    }
    if (!it->is_number_integer()) {
        throw ConfigError(where(type_, key) + " must be an integer");
    }
    return it->get<long long>();
}

std::string RuleParams::text(const char* key, const std::string& fallback) const {
    used_.insert(key);
    const auto it = entry_.find(key);
    if (it == entry_.end()) {
        return fallback;
    }
    if (!it->is_string()) {
        throw ConfigError(where(type_, key) + " must be a string");
    }
    return it->get<std::string>();
}

RuleFactory::RuleFactory() {
    // Default scores are chosen so any single rule reaches FLAG (>= 40), two
    // rules together reach DECLINE (>= 70), and a blocklisted merchant is
    // declined on its own.
    registerRule("velocity", [](const RuleParams& p, const RuleContext&) {
        return std::make_unique<VelocityRule>(static_cast<int>(p.integer("max_txns", 5)),
                                              p.integer("window_seconds", 60),
                                              p.number("score", 45));
    });
    registerRule("amount_anomaly", [](const RuleParams& p, const RuleContext&) {
        const long long minHistory = p.integer("min_history", 10);
        if (minHistory < 0) {
            throw ConfigError(where(p.type(), "min_history") + " must be >= 0");
        }
        const std::string scale = p.text("scale", "log");
        if (scale != "log" && scale != "linear") {
            throw ConfigError(where(p.type(), "scale") + " must be \"log\" or \"linear\"");
        }
        return std::make_unique<AmountAnomalyRule>(
            static_cast<std::uint64_t>(minHistory), p.number("std_devs", 3.0),
            p.number("score", 40),
            scale == "log" ? AmountAnomalyRule::Scale::Log : AmountAnomalyRule::Scale::Linear);
    });
    registerRule("geo_impossibility", [](const RuleParams& p, const RuleContext& ctx) {
        if (!ctx.cities || ctx.cities->empty()) {
            throw ConfigError("rule 'geo_impossibility' is enabled but no city table was loaded");
        }
        return std::make_unique<GeoImpossibilityRule>(ctx.cities, p.number("max_speed_kmh", 900),
                                                      p.number("min_distance_km", 100),
                                                      p.number("score", 45));
    });
    registerRule("blocked_merchant", [](const RuleParams& p, const RuleContext& ctx) {
        auto blocked = ctx.blockedMerchants
                           ? ctx.blockedMerchants
                           : std::make_shared<const std::unordered_set<std::string>>();
        return std::make_unique<BlocklistRule>(std::move(blocked), p.number("score", 80));
    });
    registerRule("round_amount_burst", [](const RuleParams& p, const RuleContext&) {
        return std::make_unique<RoundAmountBurstRule>(
            static_cast<int>(p.integer("min_count", 3)), p.integer("window_seconds", 600),
            rupees(p.integer("multiple", 1000)), p.number("score", 40));
    });
}

void RuleFactory::registerRule(const std::string& type, Creator creator) {
    if (!creators_.emplace(type, std::move(creator)).second) {
        throw std::logic_error("rule type '" + type + "' registered twice");
    }
}

std::vector<std::string> RuleFactory::registeredTypes() const {
    std::vector<std::string> types;
    for (const auto& entry : creators_) {
        types.push_back(entry.first);
    }
    return types;
}

std::unique_ptr<FraudRule> RuleFactory::create(const json& entry,
                                               const RuleContext& context) const {
    if (!entry.is_object()) {
        throw ConfigError("each entry in 'rules' must be an object");
    }
    const auto typeIt = entry.find("type");
    if (typeIt == entry.end() || !typeIt->is_string()) {
        throw ConfigError("rule entry is missing a string 'type'");
    }
    const std::string type = typeIt->get<std::string>();
    const auto creator = creators_.find(type);
    if (creator == creators_.end()) {
        std::string known;
        for (const auto& name : registeredTypes()) {
            known += (known.empty() ? "" : ", ") + name;
        }
        throw ConfigError("unknown rule type '" + type + "' (known: " + known + ")");
    }

    RuleParams params(entry, type);
    std::unique_ptr<FraudRule> rule;
    try {
        rule = creator->second(params, context);
    } catch (const std::invalid_argument& e) {
        throw ConfigError(e.what());  // constructor validation -> config error
    }

    for (const auto& item : entry.items()) {
        const std::string& key = item.key();
        if (key != "type" && key != "enabled" && params.usedKeys().count(key) == 0) {
            throw ConfigError("rule '" + type + "' has unknown parameter '" + key + "'");
        }
    }
    return rule;
}

std::vector<std::unique_ptr<FraudRule>> RuleFactory::createAll(const json& rules,
                                                               const RuleContext& context) const {
    if (!rules.is_array()) {
        throw ConfigError("'rules' must be an array");
    }
    std::vector<std::unique_ptr<FraudRule>> created;
    for (const auto& entry : rules) {
        bool enabled = true;
        if (entry.is_object()) {
            const auto it = entry.find("enabled");
            if (it != entry.end()) {
                if (!it->is_boolean()) {
                    throw ConfigError("rule 'enabled' must be true or false");
                }
                enabled = it->get<bool>();
            }
        }
        if (enabled) {
            created.push_back(create(entry, context));
        }
    }
    return created;
}

}  // namespace payguard
