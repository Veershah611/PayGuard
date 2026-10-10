#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace payguard {

// Money is stored as a whole number of paise (1 rupee = 100 paise). Integer
// arithmetic keeps balances exact; doubles would accumulate rounding error.
using Money = std::int64_t;

constexpr Money kPaisePerRupee = 100;

// Largest single amount accepted (1 billion rupees). Keeps sums of millions of
// amounts far away from int64 overflow.
constexpr Money kMaxAmount = 1'000'000'000LL * kPaisePerRupee;

// Parses a decimal rupee amount such as "250", "250.5" or "-10.00" into paise.
// Rejects more than two decimal places, stray characters and amounts whose
// magnitude exceeds kMaxAmount.
std::optional<Money> parseMoney(std::string_view text);

// Formats paise as a rupee string with exactly two decimals, e.g. "4800.00".
std::string formatMoney(Money paise);

constexpr Money rupees(std::int64_t whole) { return whole * kPaisePerRupee; }

constexpr double toRupees(Money paise) {
    return static_cast<double>(paise) / static_cast<double>(kPaisePerRupee);
}

}  // namespace payguard
