#include <gtest/gtest.h>

#include "payguard/Account.h"
#include "payguard/AccountStore.h"

using namespace payguard;

namespace {

constexpr std::int64_t kDay = 86'400;
constexpr std::int64_t kT0 = 1'728'550'000;  // 2024-10-10, mid-day UTC

Account makeAccount(Money balance, Money credit = 0, Money daily = rupees(100'000),
                    CardStatus status = CardStatus::Active) {
    return Account("C1", balance, credit, daily, "Pune", status);
}

}  // namespace

TEST(Account, DebitReducesBalance) {
    Account a = makeAccount(rupees(500));
    EXPECT_EQ(a.debit(rupees(200), kT0), FundsCheck::Ok);
    EXPECT_EQ(a.balance(), rupees(300));
}

TEST(Account, InsufficientFundsLeavesBalanceUntouched) {
    Account a = makeAccount(rupees(500));
    EXPECT_EQ(a.debit(rupees(500) + 1, kT0), FundsCheck::InsufficientFunds);
    EXPECT_EQ(a.balance(), rupees(500));
    EXPECT_EQ(a.debit(rupees(500), kT0), FundsCheck::Ok);  // exactly the balance is fine
    EXPECT_EQ(a.balance(), 0);
}

TEST(Account, CreditLimitAllowsNegativeBalanceUpToLimit) {
    Account a = makeAccount(rupees(100), rupees(1000));
    EXPECT_EQ(a.debit(rupees(1100), kT0), FundsCheck::Ok);
    EXPECT_EQ(a.balance(), -rupees(1000));
    EXPECT_EQ(a.debit(1, kT0), FundsCheck::InsufficientFunds);
}

TEST(Account, DailyLimitAccumulatesWithinADay) {
    Account a = makeAccount(rupees(10'000), 0, rupees(2000));
    EXPECT_EQ(a.debit(rupees(1500), kT0), FundsCheck::Ok);
    EXPECT_EQ(a.debit(rupees(500), kT0 + 60), FundsCheck::Ok);  // exactly at the limit
    EXPECT_EQ(a.debit(1, kT0 + 120), FundsCheck::DailyLimitExceeded);
    EXPECT_EQ(a.balance(), rupees(8000));
}

TEST(Account, DailyLimitResetsOnNextDay) {
    Account a = makeAccount(rupees(10'000), 0, rupees(2000));
    EXPECT_EQ(a.debit(rupees(2000), kT0), FundsCheck::Ok);
    EXPECT_EQ(a.debit(rupees(1), kT0 + 10), FundsCheck::DailyLimitExceeded);
    EXPECT_EQ(a.debit(rupees(2000), kT0 + kDay), FundsCheck::Ok);
}

TEST(Account, DailyBoundaryIsUtcMidnight) {
    Account a = makeAccount(rupees(10'000), 0, rupees(1000));
    const std::int64_t midnight = (kT0 / kDay + 1) * kDay;
    EXPECT_EQ(a.debit(rupees(1000), midnight - 1), FundsCheck::Ok);
    EXPECT_EQ(a.debit(rupees(1000), midnight), FundsCheck::Ok);
}

TEST(Account, LateTimestampCountsAgainstCurrentDay) {
    Account a = makeAccount(rupees(10'000), 0, rupees(1000));
    EXPECT_EQ(a.debit(rupees(800), kT0 + kDay), FundsCheck::Ok);
    // An out-of-order transaction from the previous day may not bypass the limit.
    EXPECT_EQ(a.debit(rupees(300), kT0), FundsCheck::DailyLimitExceeded);
}

TEST(Account, CheckFundsDoesNotDebit) {
    Account a = makeAccount(rupees(500));
    {
        auto locked = a.lock();
        EXPECT_EQ(locked.checkFunds(rupees(400), kT0), FundsCheck::Ok);
        EXPECT_EQ(locked.checkFunds(rupees(600), kT0), FundsCheck::InsufficientFunds);
        EXPECT_EQ(locked.balance(), rupees(500));
        EXPECT_EQ(locked.debit(rupees(400), kT0), FundsCheck::Ok);
        EXPECT_EQ(locked.spentToday(kT0), rupees(400));
        EXPECT_EQ(locked.spentToday(kT0 + kDay), 0);
    }
    EXPECT_EQ(a.balance(), rupees(100));
}

TEST(Account, CreditAndInvalidAmounts) {
    Account a = makeAccount(rupees(10));
    a.credit(rupees(5));
    EXPECT_EQ(a.balance(), rupees(15));
    EXPECT_THROW(a.credit(0), std::invalid_argument);
    EXPECT_THROW(a.debit(0, kT0), std::invalid_argument);
    EXPECT_THROW(a.debit(-1, kT0), std::invalid_argument);
    EXPECT_THROW(Account("C", 0, -1, 0, "X", CardStatus::Active), std::invalid_argument);
}

TEST(Account, StatusCanChange) {
    Account a = makeAccount(rupees(10), 0, rupees(10), CardStatus::Blocked);
    EXPECT_EQ(a.status(), CardStatus::Blocked);
    a.setStatus(CardStatus::Active);
    EXPECT_EQ(a.status(), CardStatus::Active);
}

TEST(Account, TransferMovesMoneyAtomically) {
    Account from("A", rupees(100), 0, 0, "Pune", CardStatus::Active);
    Account to("B", rupees(5), 0, 0, "Pune", CardStatus::Active);
    EXPECT_EQ(Account::transfer(from, to, rupees(60)), TransferResult::Ok);
    EXPECT_EQ(from.balance(), rupees(40));
    EXPECT_EQ(to.balance(), rupees(65));
    EXPECT_EQ(Account::transfer(from, to, rupees(41)), TransferResult::InsufficientFunds);
    EXPECT_EQ(Account::transfer(from, from, rupees(1)), TransferResult::SameAccount);
    EXPECT_EQ(Account::transfer(from, to, 0), TransferResult::InvalidAmount);
    EXPECT_EQ(from.balance() + to.balance(), rupees(105));
}

TEST(AccountStore, AddFindAndTotals) {
    AccountStore store;
    ASSERT_NE(store.add("C1", rupees(100), 0, rupees(10), "Pune", CardStatus::Active), nullptr);
    ASSERT_NE(store.add("C2", -rupees(20), rupees(50), rupees(10), "Pune", CardStatus::Active),
              nullptr);
    EXPECT_EQ(store.add("C1", 0, 0, 0, "Pune", CardStatus::Active), nullptr);
    EXPECT_EQ(store.size(), 2u);
    EXPECT_EQ(store.find("C3"), nullptr);
    EXPECT_EQ(store.find("C2")->creditLimit(), rupees(50));
    EXPECT_EQ(store.totalBalance(), rupees(80));
    EXPECT_EQ(store.overdrawnCount(), 0u);

    const AccountStore& view = store;
    std::size_t visited = 0;
    view.forEach([&](const Account&) { ++visited; });
    EXPECT_EQ(visited, 2u);
}

TEST(AccountStore, DetectsOverdrawnAccounts) {
    AccountStore store;
    store.add("C1", -rupees(60), rupees(50), 0, "Pune", CardStatus::Active);
    EXPECT_EQ(store.overdrawnCount(), 1u);
}
