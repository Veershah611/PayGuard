#include "payguard/rules/GeoImpossibilityRule.h"

#include <stdexcept>

namespace payguard {

GeoImpossibilityRule::GeoImpossibilityRule(std::shared_ptr<const CityTable> cities,
                                           double maxSpeedKmh, double minDistanceKm, double score)
    : cities_(std::move(cities)),
      maxSpeedKmh_(maxSpeedKmh),
      minDistanceKm_(minDistanceKm),
      score_(score) {
    if (!cities_) {
        throw std::invalid_argument("geo_impossibility: a city table is required");
    }
    if (maxSpeedKmh <= 0.0) {
        throw std::invalid_argument("geo_impossibility: max_speed_kmh must be positive");
    }
    if (minDistanceKm < 0.0) {
        throw std::invalid_argument("geo_impossibility: min_distance_km must be >= 0");
    }
}

RuleResult GeoImpossibilityRule::evaluate(const Transaction& txn,
                                          const CardProfile& profile) const {
    if (!profile.hasLastLocation() || profile.lastCity() == txn.city) {
        return RuleResult::pass();
    }
    const GeoPoint* from = cities_->find(profile.lastCity());
    const GeoPoint* to = cities_->find(txn.city);
    if (from == nullptr || to == nullptr) {
        return RuleResult::pass();
    }
    const double distanceKm = haversineKm(*from, *to);
    if (distanceKm < minDistanceKm_) {
        return RuleResult::pass();
    }
    const auto elapsedSeconds = txn.timestamp - profile.lastTimestamp();
    if (elapsedSeconds <= 0) {
        return RuleResult::hit(score_, name());  // two places at the same instant
    }
    const double speedKmh = distanceKm / (static_cast<double>(elapsedSeconds) / 3600.0);
    if (speedKmh > maxSpeedKmh_) {
        return RuleResult::hit(score_, name());
    }
    return RuleResult::pass();
}

}  // namespace payguard
