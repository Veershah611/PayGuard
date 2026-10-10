#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>

namespace payguard {

struct GeoPoint {
    double latitude = 0.0;   // degrees
    double longitude = 0.0;  // degrees
};

// Great-circle distance between two points on Earth, in kilometres.
double haversineKm(const GeoPoint& a, const GeoPoint& b);

// City name -> coordinates, used by the geo-impossibility rule.
class CityTable {
public:
    // Returns false if the city was already present (the first entry wins).
    bool add(const std::string& name, GeoPoint point);

    const GeoPoint* find(const std::string& name) const;
    bool contains(const std::string& name) const { return find(name) != nullptr; }

    std::size_t size() const { return cities_.size(); }
    bool empty() const { return cities_.empty(); }

private:
    std::unordered_map<std::string, GeoPoint> cities_;
};

}  // namespace payguard
