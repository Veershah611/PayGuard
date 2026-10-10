// Synthetic data generator: N accounts, ~M transactions with injected fraud
// and a ground-truth label per transaction, so detection rate and false
// positive rate can be measured.
//
// Legitimate behaviour
//   - each card has a typical spend (log-normal across cards) and an activity
//     level; amounts vary log-normally around the typical spend
//   - ~1% of purchases are unusually large (3–6x) and some are round amounts,
//     so the anomaly and round-amount rules see realistic false-positive bait
//   - ~25% of cards take one trip, with a realistic travel gap before the first
//     purchase at the destination and after returning
//
// Injected fraud (each anchored just after one of the victim's real purchases)
//   VELOCITY          card-testing burst of 8–15 small charges a few seconds apart
//   GEO               cloned card used 2,000+ km away 10–90 minutes later
//   AMOUNT            one charge 8–20x the card's typical spend
//   BLOCKED_MERCHANT  charge at a merchant on the blocklist
//   ROUND_AMOUNT      3–5 round charges (multiples of 1000) within minutes
//
// Every transaction in a fraud scenario is labelled fraud, including the first
// few of a burst that a velocity rule cannot see coming.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "payguard/CityTable.h"
#include "payguard/Money.h"
#include "payguard/StringUtil.h"

namespace fs = std::filesystem;
using payguard::Money;

namespace {

struct City {
    const char* name;
    double lat;
    double lon;
    bool domestic;
};

constexpr City kCities[] = {
    {"Ahmedabad", 23.0225, 72.5714, true},    {"Mumbai", 19.0760, 72.8777, true},
    {"Delhi", 28.6139, 77.2090, true},        {"Bengaluru", 12.9716, 77.5946, true},
    {"Chennai", 13.0827, 80.2707, true},      {"Kolkata", 22.5726, 88.3639, true},
    {"Hyderabad", 17.3850, 78.4867, true},    {"Pune", 18.5204, 73.8567, true},
    {"Jaipur", 26.9124, 75.7873, true},       {"Surat", 21.1702, 72.8311, true},
    {"Lucknow", 26.8467, 80.9462, true},      {"Kochi", 9.9312, 76.2673, true},
    {"Chandigarh", 30.7333, 76.7794, true},   {"Indore", 22.7196, 75.8577, true},
    {"Bhopal", 23.2599, 77.4126, true},       {"Panaji", 15.4909, 73.8278, true},
    {"Vadodara", 22.3072, 73.1812, true},     {"Nagpur", 21.1458, 79.0882, true},
    {"Patna", 25.5941, 85.1376, true},        {"Guwahati", 26.1445, 91.7362, true},
    {"London", 51.5074, -0.1278, false},      {"New York", 40.7128, -74.0060, false},
    {"Dubai", 25.2048, 55.2708, false},       {"Singapore", 1.3521, 103.8198, false},
    {"Tokyo", 35.6762, 139.6503, false},      {"Sydney", -33.8688, 151.2093, false},
    {"Paris", 48.8566, 2.3522, false},        {"Frankfurt", 50.1109, 8.6821, false},
    {"Hong Kong", 22.3193, 114.1694, false},  {"San Francisco", 37.7749, -122.4194, false},
    {"Toronto", 43.6532, -79.3832, false},    {"Lagos", 6.5244, 3.3792, false},
    {"Moscow", 55.7558, 37.6173, false},      {"Sao Paulo", -23.5505, -46.6333, false},
    {"Bangkok", 13.7563, 100.5018, false},    {"Johannesburg", -26.2041, 28.0473, false},
};
constexpr int kCityCount = static_cast<int>(sizeof(kCities) / sizeof(kCities[0]));
constexpr int kDomesticCities = 20;

constexpr int kLegitMerchants = 4999;    // M0001..M4999
constexpr int kBlockedMerchantBase = 9000;  // M9000..M9049
constexpr int kBlockedMerchants = 50;

enum FraudType : std::uint8_t { kLegit, kVelocity, kGeo, kAmount, kBlocked, kRound, kTypeCount };
constexpr const char* kTypeNames[kTypeCount] = {"",       "VELOCITY",         "GEO",
                                                "AMOUNT", "BLOCKED_MERCHANT", "ROUND_AMOUNT"};

struct Options {
    std::size_t accounts = 10'000;
    std::size_t transactions = 1'000'000;
    double fraudRate = 0.02;
    int days = 30;
    std::uint64_t seed = 42;
    std::size_t malformed = 0;
    std::string outDir = "data/generated";
    std::int64_t start = 1'728'000'000;  // 2024-10-04 UTC
};

struct Card {
    std::string id;
    int home = 0;
    double typical = 0.0;  // paise
    Money balance = 0;
    Money credit = 0;
    Money daily = 0;
    bool active = true;
};

struct Event {
    std::int64_t ts;
    std::uint32_t card;
    std::uint16_t merchant;
    std::uint8_t city;
    std::uint8_t type;
    Money amount;
};

double distanceKm(int a, int b) {
    return payguard::haversineKm({kCities[a].lat, kCities[a].lon}, {kCities[b].lat, kCities[b].lon});
}

class Generator {
public:
    explicit Generator(Options options) : o_(std::move(options)), rng_(o_.seed) {}

    void run() {
        fs::create_directories(o_.outDir);
        makeCards();
        makeLegit();
        makeFraud();
        std::stable_sort(events_.begin(), events_.end(),
                         [](const Event& a, const Event& b) { return a.ts < b.ts; });
        writeAll();
    }

private:
    double uniform(double lo, double hi) { return std::uniform_real_distribution<double>(lo, hi)(rng_); }
    std::int64_t uniformInt(std::int64_t lo, std::int64_t hi) {
        return std::uniform_int_distribution<std::int64_t>(lo, hi)(rng_);
    }
    bool chance(double p) { return std::bernoulli_distribution(p)(rng_); }
    std::int64_t end() const { return o_.start + std::int64_t{o_.days} * 86'400; }

    static Money toPaise(double paise) { return std::max<Money>(100, std::llround(paise)); }

    void makeCards() {
        std::lognormal_distribution<double> typical(std::log(800.0 * 100), 0.8);
        std::lognormal_distribution<double> activity(0.0, 0.5);
        cards_.resize(o_.accounts);
        weights_.resize(o_.accounts);
        for (std::size_t i = 0; i < o_.accounts; ++i) {
            Card& c = cards_[i];
            char id[16];
            std::snprintf(id, sizeof(id), "C%07zu", i + 1);
            c.id = id;
            c.home = chance(0.97) ? static_cast<int>(uniformInt(0, kDomesticCities - 1))
                                  : static_cast<int>(uniformInt(kDomesticCities, kCityCount - 1));
            c.typical = std::clamp(typical(rng_), 50.0 * 100, 50'000.0 * 100);
            c.active = !chance(0.01);
            weights_[i] = activity(rng_);
        }
    }

    void makeLegit() {
        const std::size_t fraudTarget =
            static_cast<std::size_t>(std::llround(static_cast<double>(o_.transactions) * o_.fraudRate));
        fraudTarget_ = std::min(fraudTarget, o_.transactions);
        const std::size_t legitCount = o_.transactions - fraudTarget_;

        std::vector<std::size_t> counts(o_.accounts, 0);
        std::discrete_distribution<std::size_t> pick(weights_.begin(), weights_.end());
        for (std::size_t i = 0; i < legitCount; ++i) {
            ++counts[pick(rng_)];
        }

        events_.reserve(o_.transactions + 64);
        cardEvents_.resize(o_.accounts);
        std::lognormal_distribution<double> spread(0.0, 0.45);
        std::vector<Event> local;

        for (std::size_t ci = 0; ci < o_.accounts; ++ci) {
            Card& card = cards_[ci];
            const std::size_t n = counts[ci];

            // Optional trip, with no purchases while travelling.
            bool trip = n >= 5 && chance(0.25);
            int dest = card.home;
            std::int64_t tripStart = 0, tripEnd = 0, gap = 0;
            if (trip) {
                do {
                    dest = chance(0.7) ? static_cast<int>(uniformInt(0, kDomesticCities - 1))
                                       : static_cast<int>(uniformInt(kDomesticCities, kCityCount - 1));
                } while (dest == card.home);
                tripStart = uniformInt(o_.start + 86'400, end() - 3 * 86'400);
                tripEnd = tripStart + uniformInt(1, 5) * 86'400;
                gap = static_cast<std::int64_t>((distanceKm(card.home, dest) / 700.0 + 2.0) * 3600.0);
            }
            auto inGap = [&](std::int64_t ts) {
                return trip && ((ts >= tripStart - gap && ts < tripStart) ||
                                (ts >= tripEnd && ts < tripEnd + gap));
            };

            local.clear();
            for (std::size_t k = 0; k < n; ++k) {
                std::int64_t ts = uniformInt(o_.start, end() - 1);
                for (int attempt = 0; attempt < 20 && inGap(ts); ++attempt) {
                    ts = uniformInt(o_.start, end() - 1);
                }
                if (inGap(ts)) {
                    ts = tripStart + 3600;
                }
                const bool away = trip && ts >= tripStart && ts < tripEnd;

                double amount = card.typical * spread(rng_);
                if (chance(0.01)) {
                    amount *= uniform(3.0, 6.0);  // the occasional big purchase
                }
                if (amount >= 1000.0 * 100 && chance(0.005)) {
                    amount = std::round(amount / (1000.0 * 100)) * 1000.0 * 100;
                }
                Event e{};
                e.ts = ts;
                e.card = static_cast<std::uint32_t>(ci);
                e.merchant = static_cast<std::uint16_t>(uniformInt(1, kLegitMerchants));
                e.city = static_cast<std::uint8_t>(away ? dest : card.home);
                e.type = kLegit;
                e.amount = toPaise(amount);
                local.push_back(e);
            }
            std::sort(local.begin(), local.end(),
                      [](const Event& a, const Event& b) { return a.ts < b.ts; });

            // Fund the account for its normal month, with a few thin accounts.
            const double spend = std::max(1.0, static_cast<double>(n)) * card.typical * 1.15;
            card.balance = chance(0.02) ? toPaise(card.typical * uniform(5.0, 20.0))
                                        : toPaise(spend * uniform(1.5, 3.0));
            card.credit = chance(0.5) ? toPaise(card.typical * uniform(20.0, 100.0)) : 0;
            const double perDay = static_cast<double>(n) / o_.days;
            card.daily = toPaise(card.typical * std::max(15.0, 3.0 * perDay) * uniform(1.0, 2.0));

            cardEvents_[ci].first = events_.size();
            events_.insert(events_.end(), local.begin(), local.end());
            cardEvents_[ci].second = events_.size();
        }
    }

    // Picks an active victim with at least `minHistory` legit events.
    bool pickVictim(std::size_t minHistory, std::uint32_t& card, std::size_t& anchor) {
        for (int attempt = 0; attempt < 1000; ++attempt) {
            const auto ci = static_cast<std::uint32_t>(uniformInt(0, static_cast<std::int64_t>(o_.accounts) - 1));
            const auto [first, last] = cardEvents_[ci];
            if (!cards_[ci].active || last - first < std::max<std::size_t>(minHistory, 1)) {
                continue;
            }
            card = ci;
            anchor = first + static_cast<std::size_t>(uniformInt(
                             static_cast<std::int64_t>(minHistory == 0 ? 0 : minHistory - 1),
                             static_cast<std::int64_t>(last - first - 1)));
            return true;
        }
        return false;
    }

    void addFraud(const Event& anchor, std::int64_t ts, int city, int merchant, Money amount,
                  FraudType type) {
        Event e{};
        e.ts = std::min(ts, end() + 86'400);
        e.card = anchor.card;
        e.merchant = static_cast<std::uint16_t>(merchant);
        e.city = static_cast<std::uint8_t>(city);
        e.type = type;
        e.amount = amount;
        fraud_.push_back(e);
        ++fraudByType_[type];
    }

    void makeFraud() {
        // Scenario mix (per scenario, not per transaction).
        std::discrete_distribution<int> scenario({0.0, 0.10, 0.28, 0.25, 0.20, 0.17});
        const auto legitMerchant = [&] { return static_cast<int>(uniformInt(1, kLegitMerchants)); };

        while (fraud_.size() < fraudTarget_) {
            const auto type = static_cast<FraudType>(scenario(rng_));
            std::uint32_t ci = 0;
            std::size_t anchorIndex = 0;
            if (!pickVictim(type == kAmount ? 15 : 1, ci, anchorIndex)) {
                break;  // no suitable victims (tiny data sets)
            }
            const Event anchor = events_[anchorIndex];
            const Card& card = cards_[ci];

            switch (type) {
                case kVelocity: {
                    std::int64_t ts = anchor.ts + uniformInt(600, 6 * 3600);
                    const auto burst = uniformInt(8, 15);
                    for (std::int64_t k = 0; k < burst; ++k) {
                        addFraud(anchor, ts, anchor.city, legitMerchant(),
                                 toPaise(uniform(1.0, 200.0) * 100), kVelocity);
                        ts += uniformInt(2, 8);
                    }
                    break;
                }
                case kGeo: {
                    int city = 0;
                    do {
                        city = static_cast<int>(uniformInt(0, kCityCount - 1));
                    } while (distanceKm(anchor.city, city) < 2000.0);
                    addFraud(anchor, anchor.ts + uniformInt(10 * 60, 90 * 60), city, legitMerchant(),
                             toPaise(card.typical * uniform(1.0, 2.5)), kGeo);
                    break;
                }
                case kAmount: {
                    const double amount = std::min(card.typical * uniform(8.0, 20.0),
                                                   static_cast<double>(card.daily) * 0.8);
                    addFraud(anchor, anchor.ts + uniformInt(3600, 6 * 3600), anchor.city,
                             legitMerchant(), toPaise(amount), kAmount);
                    break;
                }
                case kBlocked: {
                    addFraud(anchor, anchor.ts + uniformInt(5 * 60, 2 * 3600), anchor.city,
                             kBlockedMerchantBase + static_cast<int>(uniformInt(0, kBlockedMerchants - 1)),
                             toPaise(card.typical * uniform(0.5, 2.0)), kBlocked);
                    break;
                }
                case kRound: {
                    std::int64_t ts = anchor.ts + uniformInt(600, 6 * 3600);
                    const auto burst = uniformInt(3, 5);
                    for (std::int64_t k = 0; k < burst; ++k) {
                        addFraud(anchor, ts, anchor.city, legitMerchant(),
                                 payguard::rupees(1000 * uniformInt(1, 10)), kRound);
                        ts += uniformInt(60, 180);
                    }
                    break;
                }
                default:
                    break;
            }
        }
        events_.insert(events_.end(), fraud_.begin(), fraud_.end());
    }

    void writeAll() {
        const fs::path dir(o_.outDir);

        {
            std::ofstream out(dir / "cities.csv", std::ios::binary);
            out << "city,latitude,longitude\n";
            for (const auto& c : kCities) {
                out << c.name << ',' << c.lat << ',' << c.lon << '\n';
            }
        }
        {
            std::ofstream out(dir / "blocked_merchants.txt", std::ios::binary);
            out << "# generated blocklist\n";
            for (int i = 0; i < kBlockedMerchants; ++i) {
                char id[16];
                std::snprintf(id, sizeof(id), "M%04d", kBlockedMerchantBase + i);
                out << id << '\n';
            }
        }
        {
            std::ofstream out(dir / "accounts.csv", std::ios::binary);
            out << "card_id,balance,credit_limit,daily_limit,home_city,status\n";
            for (const auto& c : cards_) {
                out << c.id << ',' << payguard::formatMoney(c.balance) << ','
                    << payguard::formatMoney(c.credit) << ',' << payguard::formatMoney(c.daily) << ','
                    << kCities[c.home].name << ',' << (c.active ? "ACTIVE" : "BLOCKED") << '\n';
            }
        }

        // Malformed rows go at random positions in the transaction stream.
        std::vector<std::size_t> badPositions;
        for (std::size_t i = 0; i < o_.malformed; ++i) {
            badPositions.push_back(static_cast<std::size_t>(
                uniformInt(0, static_cast<std::int64_t>(events_.size()))));
        }
        std::sort(badPositions.begin(), badPositions.end());

        std::ofstream txns(dir / "transactions.csv", std::ios::binary);
        std::ofstream labels(dir / "labels.csv", std::ios::binary);
        txns << "txn_id,card_id,merchant_id,amount,city,timestamp\n";
        labels << "txn_id,is_fraud,fraud_type\n";

        std::string txnBuf;
        std::string labelBuf;
        txnBuf.reserve(1 << 20);
        labelBuf.reserve(1 << 20);
        std::size_t badIndex = 0;
        char line[256];
        for (std::size_t i = 0; i < events_.size(); ++i) {
            while (badIndex < badPositions.size() && badPositions[badIndex] == i) {
                txnBuf += malformedRow(badIndex++);
            }
            const Event& e = events_[i];
            char id[16];
            std::snprintf(id, sizeof(id), "T%08zu", i + 1);
            std::snprintf(line, sizeof(line), "%s,%s,M%04u,%s,%s,%lld\n", id,
                          cards_[e.card].id.c_str(), static_cast<unsigned>(e.merchant),
                          payguard::formatMoney(e.amount).c_str(), kCities[e.city].name,
                          static_cast<long long>(e.ts));
            txnBuf += line;
            std::snprintf(line, sizeof(line), "%s,%d,%s\n", id, e.type == kLegit ? 0 : 1,
                          kTypeNames[e.type]);
            labelBuf += line;
            if (txnBuf.size() > (1u << 20)) {
                txns << txnBuf;
                labels << labelBuf;
                txnBuf.clear();
                labelBuf.clear();
            }
        }
        while (badIndex < badPositions.size()) {
            txnBuf += malformedRow(badIndex++);
        }
        txns << txnBuf;
        labels << labelBuf;

        std::size_t fraudTotal = 0;
        for (int t = 1; t < kTypeCount; ++t) {
            fraudTotal += fraudByType_[t];
        }
        std::cout << "Wrote " << dir.string() << ":\n"
                  << "  accounts      " << payguard::withThousands(cards_.size()) << '\n'
                  << "  transactions  " << payguard::withThousands(events_.size()) << " ("
                  << payguard::withThousands(events_.size() - fraudTotal) << " legit, "
                  << payguard::withThousands(fraudTotal) << " fraud, " << o_.malformed
                  << " malformed rows)\n";
        for (int t = 1; t < kTypeCount; ++t) {
            std::cout << "    " << kTypeNames[t] << ": " << payguard::withThousands(fraudByType_[t])
                      << '\n';
        }
    }

    std::string malformedRow(std::size_t i) {
        switch (i % 4) {
            case 0: return "TBAD" + std::to_string(i) + ",C0000001,M0001,10.00\n";
            case 1: return "TBAD" + std::to_string(i) + ",C0000001,M0001,10.00,Mumbai,yesterday\n";
            case 2: return "TBAD" + std::to_string(i) + ",,M0001,10.00,Mumbai,1728000000\n";
            default: return "TBAD" + std::to_string(i) + ",C0000001,M0001,10.00,Mumbai,1728000000,extra\n";
        }
    }

    Options o_;
    std::mt19937_64 rng_;
    std::vector<Card> cards_;
    std::vector<double> weights_;
    std::vector<Event> events_;
    std::vector<Event> fraud_;
    std::vector<std::pair<std::size_t, std::size_t>> cardEvents_;  // [first, last) into events_
    std::size_t fraudTarget_ = 0;
    std::array<std::size_t, kTypeCount> fraudByType_{};
};

void usage() {
    std::cout << R"(Usage: generate_data [options]
  --accounts N       number of cards                       [10000]
  --transactions M   approximate number of transactions    [1000000]
  --fraud-rate F     fraction of transactions that are fraud [0.02]
  --days D           length of the simulated period        [30]
  --malformed K      malformed rows to inject              [0]
  --seed S           random seed                           [42]
  --out DIR          output directory                      [data/generated]
)";
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            usage();
            return 0;
        }
        if (i + 1 >= argc) {
            std::cerr << "error: " << arg << " needs a value\n";
            return 2;
        }
        const std::string value = argv[++i];
        const auto integer = payguard::parseInt64(value);
        const auto real = payguard::parseDouble(value);
        if (arg == "--out") {
            o.outDir = value;
        } else if (arg == "--fraud-rate" && real && *real >= 0.0 && *real < 1.0) {
            o.fraudRate = *real;
        } else if (integer && *integer >= 0 && arg == "--accounts" && *integer > 0) {
            o.accounts = static_cast<std::size_t>(*integer);
        } else if (integer && *integer > 0 && arg == "--transactions") {
            o.transactions = static_cast<std::size_t>(*integer);
        } else if (integer && *integer >= 4 && arg == "--days") {
            o.days = static_cast<int>(*integer);
        } else if (integer && *integer >= 0 && arg == "--malformed") {
            o.malformed = static_cast<std::size_t>(*integer);
        } else if (integer && arg == "--seed") {
            o.seed = static_cast<std::uint64_t>(*integer);
        } else {
            std::cerr << "error: bad option or value: " << arg << ' ' << value << '\n';
            usage();
            return 2;
        }
    }
    if (o.accounts > 9'999'999) {
        std::cerr << "error: at most 9,999,999 accounts\n";
        return 2;
    }
    try {
        Generator(o).run();
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
