#include "payguard/AccountStore.h"

namespace payguard {

Account* AccountStore::add(const std::string& cardId, Money balance, Money creditLimit,
                           Money dailyLimit, const std::string& homeCity, CardStatus status) {
    auto [it, inserted] = accounts_.try_emplace(cardId, cardId, balance, creditLimit, dailyLimit,
                                                homeCity, status);
    return inserted ? &it->second : nullptr;
}

Account* AccountStore::find(const std::string& cardId) {
    const auto it = accounts_.find(cardId);
    return it == accounts_.end() ? nullptr : &it->second;
}

const Account* AccountStore::find(const std::string& cardId) const {
    const auto it = accounts_.find(cardId);
    return it == accounts_.end() ? nullptr : &it->second;
}

Money AccountStore::totalBalance() const {
    Money total = 0;
    for (const auto& entry : accounts_) {
        total += entry.second.balance();
    }
    return total;
}

std::size_t AccountStore::overdrawnCount() const {
    std::size_t count = 0;
    for (const auto& entry : accounts_) {
        if (entry.second.balance() < -entry.second.creditLimit()) {
            ++count;
        }
    }
    return count;
}

}  // namespace payguard
