#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>

#include "payguard/Account.h"

namespace payguard {

// Owns every account, keyed by card ID for O(1) average lookup. Ordering is
// never needed, so a hash map beats std::map's O(log n).
//
// Populate the store before processing starts. After that the map itself is
// read-only (lookups are safe from any thread) and each Account guards its own
// mutable state.
class AccountStore {
public:
    // Returns nullptr if the card ID already exists.
    Account* add(const std::string& cardId, Money balance, Money creditLimit, Money dailyLimit,
                 const std::string& homeCity, CardStatus status);

    Account* find(const std::string& cardId);
    const Account* find(const std::string& cardId) const;

    std::size_t size() const { return accounts_.size(); }
    void reserve(std::size_t count) { accounts_.reserve(count); }

    // Sum of all balances, locking each account in turn. Only a consistent
    // snapshot when no transactions are in flight.
    Money totalBalance() const;

    // Accounts whose balance has fallen below -creditLimit (must always be 0).
    std::size_t overdrawnCount() const;

    template <typename Fn>
    void forEach(Fn&& fn) const {
        for (const auto& entry : accounts_) {
            fn(entry.second);
        }
    }

private:
    std::unordered_map<std::string, Account> accounts_;
};

}  // namespace payguard
