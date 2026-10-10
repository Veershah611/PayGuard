#include "payguard/Account.h"

#include <stdexcept>

namespace payguard {

namespace {
constexpr std::int64_t kSecondsPerDay = 86'400;
}

Account::Account(std::string cardId, Money balance, Money creditLimit, Money dailyLimit,
                 std::string homeCity, CardStatus status)
    : cardId_(std::move(cardId)),
      creditLimit_(creditLimit),
      dailyLimit_(dailyLimit),
      homeCity_(std::move(homeCity)),
      balance_(balance),
      status_(status) {
    if (creditLimit < 0 || dailyLimit < 0) {
        throw std::invalid_argument("account " + cardId_ + ": limits must be non-negative");
    }
}

std::int64_t Account::dayOf(std::int64_t timestamp) {
    // Floor division so negative timestamps land on the correct day.
    std::int64_t day = timestamp / kSecondsPerDay;
    if (timestamp % kSecondsPerDay < 0) {
        --day;
    }
    return day;
}

Money Account::spentTodayLocked(std::int64_t day) const {
    // A new day starts with nothing spent. An out-of-order timestamp from an
    // earlier day is conservatively measured against the current day's total.
    return day > spendDay_ ? 0 : spentToday_;
}

FundsCheck Account::checkFundsLocked(Money amount, std::int64_t timestamp) const {
    if (amount > balance_ + creditLimit_) {
        return FundsCheck::InsufficientFunds;
    }
    if (spentTodayLocked(dayOf(timestamp)) + amount > dailyLimit_) {
        return FundsCheck::DailyLimitExceeded;
    }
    return FundsCheck::Ok;
}

FundsCheck Account::debitLocked(Money amount, std::int64_t timestamp) {
    if (amount <= 0) {
        throw std::invalid_argument("debit amount must be positive");
    }
    const FundsCheck check = checkFundsLocked(amount, timestamp);
    if (check != FundsCheck::Ok) {
        return check;
    }
    const std::int64_t day = dayOf(timestamp);
    if (day > spendDay_) {
        spendDay_ = day;
        spentToday_ = 0;
    }
    spentToday_ += amount;
    balance_ -= amount;
    return FundsCheck::Ok;
}

Money Account::Locked::spentToday(std::int64_t timestamp) const {
    return account_.spentTodayLocked(dayOf(timestamp));
}

FundsCheck Account::Locked::checkFunds(Money amount, std::int64_t timestamp) const {
    return account_.checkFundsLocked(amount, timestamp);
}

FundsCheck Account::Locked::debit(Money amount, std::int64_t timestamp) {
    return account_.debitLocked(amount, timestamp);
}

FundsCheck Account::debit(Money amount, std::int64_t timestamp) {
    std::lock_guard<std::mutex> guard(mutex_);
    return debitLocked(amount, timestamp);
}

void Account::credit(Money amount) {
    if (amount <= 0) {
        throw std::invalid_argument("credit amount must be positive");
    }
    std::lock_guard<std::mutex> guard(mutex_);
    balance_ += amount;
}

Money Account::balance() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return balance_;
}

CardStatus Account::status() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return status_;
}

void Account::setStatus(CardStatus status) {
    std::lock_guard<std::mutex> guard(mutex_);
    status_ = status;
}

TransferResult Account::transfer(Account& from, Account& to, Money amount) {
    if (&from == &to) {
        return TransferResult::SameAccount;
    }
    if (amount <= 0) {
        return TransferResult::InvalidAmount;
    }
    std::scoped_lock guard(from.mutex_, to.mutex_);
    if (amount > from.balance_ + from.creditLimit_) {
        return TransferResult::InsufficientFunds;
    }
    from.balance_ -= amount;
    to.balance_ += amount;
    return TransferResult::Ok;
}

}  // namespace payguard
