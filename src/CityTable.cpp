#include "payguard/CityTable.h"

#include <algorithm>
#include <cmath>

namespace payguard {

namespace {
constexpr double kEarthRadiusKm = 6371.0;
constexpr double kPi = 3.14159265358979323846;

double toRadians(double degrees) { return degrees * kPi / 180.0; }
}  // namespace

double haversineKm(const GeoPoint& a, const GeoPoint& b) {
    const double dLat = toRadians(b.latitude - a.latitude);
    const double dLon = toRadians(b.longitude - a.longitude);
    const double sinLat = std::sin(dLat / 2.0);
    const double sinLon = std::sin(dLon / 2.0);
    const double h = sinLat * sinLat + std::cos(toRadians(a.latitude)) *
                                           std::cos(toRadians(b.latitude)) * sinLon * sinLon;
    return 2.0 * kEarthRadiusKm * std::asin(std::min(1.0, std::sqrt(h)));
}

bool CityTable::add(const std::string& name, GeoPoint point) {
    return cities_.emplace(name, point).second;
}

const GeoPoint* CityTable::find(const std::string& name) const {
    const auto it = cities_.find(name);
    return it == cities_.end() ? nullptr : &it->second;
}

}  // namespace payguard
