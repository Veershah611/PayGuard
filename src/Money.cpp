#include "payguard/Money.h"

#include <cstdio>

#include "payguard/StringUtil.h"

namespace payguard {

namespace {
bool isDigit(char c) { return c >= '0' && c <= '9'; }
}  // namespace

std::optional<Money> parseMoney(std::string_view text) {
    text = trim(text);
    if (text.empty()) {
        return std::nullopt;
    }

    std::size_t i = 0;
    bool negative = false;
    if (text[0] == '-' || text[0] == '+') {
        negative = text[0] == '-';
        i = 1;
    }

    constexpr Money kMaxWholeRupees = kMaxAmount / kPaisePerRupee;
    Money whole = 0;
    std::size_t wholeDigits = 0;
    for (; i < text.size() && isDigit(text[i]); ++i) {
        whole = whole * 10 + (text[i] - '0');
        if (whole > kMaxWholeRupees) {
            return std::nullopt;
        }
        ++wholeDigits;
    }

    Money fraction = 0;
    std::size_t fractionDigits = 0;
    if (i < text.size() && text[i] == '.') {
        ++i;
        for (; i < text.size() && isDigit(text[i]); ++i) {
            if (++fractionDigits > 2) {
                return std::nullopt;  // sub-paisa precision is not a valid amount
            }
            fraction = fraction * 10 + (text[i] - '0');
        }
    }

    if (i != text.size() || (wholeDigits == 0 && fractionDigits == 0)) {
        return std::nullopt;
    }
    if (fractionDigits == 1) {
        fraction *= 10;  // "250.5" means 50 paise, not 5
    }

    const Money paise = whole * kPaisePerRupee + fraction;
    if (paise > kMaxAmount) {
        return std::nullopt;
    }
    return negative ? -paise : paise;
}

std::string formatMoney(Money paise) {
    const bool negative = paise < 0;
    // Work in unsigned space so the most negative value cannot overflow.
    const auto magnitude = negative ? 0ULL - static_cast<unsigned long long>(paise)
                                    : static_cast<unsigned long long>(paise);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s%llu.%02llu", negative ? "-" : "",
                  magnitude / static_cast<unsigned long long>(kPaisePerRupee),
                  magnitude % static_cast<unsigned long long>(kPaisePerRupee));
    return buf;
}

}  // namespace payguard
