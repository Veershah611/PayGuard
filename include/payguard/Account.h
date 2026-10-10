#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include "payguard/CardProfile.h"
#include "payguard/Money.h"

namespace payguard {

enum class CardStatus : std::uint8_t { Active, Blocked };

enum class FundsCheck : std::uint8_t { Ok, InsufficientFunds, DailyLimitExceeded };

enum class TransferResult : std::uint8_t { Ok, SameAccount, InvalidAmount, InsufficientFunds };

// A card account. Balance, status and daily spend are private and change only
// through methods that hold the account's own mutex, so different cards never
// contend with each other.
//
// The balance may go negative down to -creditLimit (credit in use); it can
// never go below that.
class Account {
public:
    Account(std::string cardId, Money balance, Money creditLimit, Money dailyLimit,
            std::string homeCity, CardStatus status);

    Account(const Account&) = delete;
    Account& operator=(const Account&) = delete;

    // RAII handle that holds the account lock for its lifetime. The engine uses
    // one of these to run checks, fraud rules and the debit as a single atomic
    // step, so two threads can never both spend the same last rupee.
    class Locked {
    public:
        explicit Locked(Account& account) : account_(account), guard_(account.mutex_) {}
        Locked(const Locked&) = delete;
        Locked& operator=(const Locked&) = delete;

        CardStatus status() const { return account_.status_; }
        Money balance() const { return account_.balance_; }
        Money spentToday(std::int64_t timestamp) const;
        FundsCheck checkFunds(Money amount, std::int64_t timestamp) const;
        FundsCheck debit(Money amount, std::int64_t timestamp);
        CardProfile& profile() { return account_.profile_; }
        const CardProfile& profile() const { return account_.profile_; }
        const Account& account() const { return account_; }

    private:
        Account& account_;
        std::lock_guard<std::mutex> guard_;
    };

    // C++17 guaranteed copy elision lets this return a non-movable handle.
    Locked lock() { return Locked(*this); }

    // Thread-safe single-step operations (each takes the lock once).
    FundsCheck debit(Money amount, std::int64_t timestamp);
    void credit(Money amount);
    Money balance() const;
    CardStatus status() const;
    void setStatus(CardStatus status);

    // Moves money between two accounts atomically. Both locks are taken with
    // std::scoped_lock, which orders acquisition to avoid deadlock.
    static TransferResult transfer(Account& from, Account& to, Money amount);

    // Immutable after construction; safe to read without the lock.
    const std::string& cardId() const { return cardId_; }
    const std::string& homeCity() const { return homeCity_; }
    Money creditLimit() const { return creditLimit_; }
    Money dailyLimit() const { return dailyLimit_; }

private:
    static std::int64_t dayOf(std::int64_t timestamp);
    Money spentTodayLocked(std::int64_t day) const;
    FundsCheck checkFundsLocked(Money amount, std::int64_t timestamp) const;
    FundsCheck debitLocked(Money amount, std::int64_t timestamp);

    const std::string cardId_;
    const Money creditLimit_;
    const Money dailyLimit_;
    const std::string homeCity_;

    mutable std::mutex mutex_;
    Money balance_;
    CardStatus status_;
    std::int64_t spendDay_ = INT64_MIN;  // UTC day index that spentToday_ refers to
    Money spentToday_ = 0;
    CardProfile profile_;
};

}  // namespace payguard
