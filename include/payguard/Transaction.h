#pragma once

#include <cstdint>
#include <string>

#include "payguard/Money.h"

namespace payguard {

// One card authorization request. Plain data; validation happens in the engine.
struct Transaction {
    std::string id;
    std::string cardId;
    std::string merchantId;
    Money amount = 0;  // paise; anything <= 0 is declined as INVALID_AMOUNT
    std::string city;
    std::int64_t timestamp = 0;  // Unix seconds
};

}  // namespace payguard
