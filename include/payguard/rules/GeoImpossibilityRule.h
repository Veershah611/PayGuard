#pragma once

#include <memory>

#include "payguard/CityTable.h"
#include "payguard/rules/FraudRule.h"

namespace payguard {

// F3: the card was last used in a city too far away to have travelled from
// in the time since (implied speed above maxSpeedKmh). Hops shorter than
// minDistanceKm are ignored so neighbouring cities do not trip the rule.
// Unknown cities are skipped here; the engine handles them as a hard check.
class GeoImpossibilityRule : public FraudRule {
public:
    GeoImpossibilityRule(std::shared_ptr<const CityTable> cities, double maxSpeedKmh,
                         double minDistanceKm, double score);

    RuleResult evaluate(const Transaction& txn, const CardProfile& profile) const override;
    std::string name() const override { return "GEO_IMPOSSIBLE"; }

private:
    std::shared_ptr<const CityTable> cities_;
    double maxSpeedKmh_;
    double minDistanceKm_;
    double score_;
};

}  // namespace payguard
