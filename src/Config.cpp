#include "payguard/Config.h"

#include <fstream>

namespace payguard {

namespace {

using nlohmann::json;

template <typename T>
T readOr(const json& object, const char* section, const char* key, T fallback) {
    const auto it = object.find(key);
    if (it == object.end()) {
        return fallback;
    }
    try {
        return it->get<T>();
    } catch (const json::exception&) {
        throw ConfigError(std::string("'") + section + "." + key + "' has the wrong type");
    }
}

std::size_t readCount(const json& object, const char* section, const char* key,
                      std::size_t fallback, std::size_t minimum) {
    const auto value = readOr<long long>(object, section, key, static_cast<long long>(fallback));
    if (value < static_cast<long long>(minimum)) {
        throw ConfigError(std::string("'") + section + "." + key + "' must be >= " +
                          std::to_string(minimum));
    }
    return static_cast<std::size_t>(value);
}

}  // namespace

Config Config::fromJson(const json& root) {
    if (!root.is_object()) {
        throw ConfigError("config root must be a JSON object");
    }
    Config config;

    if (const auto it = root.find("decision"); it != root.end()) {
        if (!it->is_object()) {
            throw ConfigError("'decision' must be an object");
        }
        config.thresholds.flag = readOr(*it, "decision", "flag_threshold", config.thresholds.flag);
        config.thresholds.decline =
            readOr(*it, "decision", "decline_threshold", config.thresholds.decline);
    }
    const auto& t = config.thresholds;
    if (!(t.flag >= 0.0 && t.flag <= t.decline && t.decline <= 100.0)) {
        throw ConfigError("thresholds must satisfy 0 <= flag_threshold <= decline_threshold <= 100");
    }

    if (const auto it = root.find("engine"); it != root.end()) {
        if (!it->is_object()) {
            throw ConfigError("'engine' must be an object");
        }
        auto& e = config.engine;
        e.threads = readCount(*it, "engine", "threads", e.threads, 0);
        e.queueCapacity = readCount(*it, "engine", "queue_capacity", e.queueCapacity, 1);
        e.batchSize = readCount(*it, "engine", "batch_size", e.batchSize, 1);
        e.declineUnknownCity =
            readOr(*it, "engine", "decline_unknown_city", e.declineUnknownCity);
    }

    if (const auto it = root.find("rules"); it != root.end()) {
        if (!it->is_array()) {
            throw ConfigError("'rules' must be an array");
        }
        config.rules = *it;
    }
    return config;
}

Config Config::loadFile(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw ConfigError("cannot open config file '" + path + "'");
    }
    json root;
    try {
        in >> root;
    } catch (const json::parse_error& e) {
        throw ConfigError("invalid JSON in '" + path + "': " + e.what());
    }
    return fromJson(root);
}

}  // namespace payguard
