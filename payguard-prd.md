# PayGuard: Real-Time Payment Authorization & Fraud Detection Engine

**Product Requirements Document (PRD)**
**Author:** Veer Shah
**Language:** C++17
**Status:** Draft v1.0
**Target timeline:** 4 weeks

---

## 1. Overview

PayGuard is a high-performance C++ engine that processes a stream of card transactions and, for each one, decides in real time whether to **APPROVE**, **DECLINE**, or **FLAG** it for review.

It checks each transaction against account rules (balance, card limits, blocked cards) and a set of configurable fraud rules (velocity, amount anomaly, geo-impossibility, blocked merchants). It records every decision in an append-only audit log and reports performance and accuracy metrics.

**One-line pitch:** *A system that approves or declines card payments in microseconds and catches suspicious ones, similar to what runs behind every card swipe at Visa, Mastercard and JPMorgan.*

---

## 2. Problem Statement

Every card payment must be authorized within milliseconds. In that window, the system must:

1. Confirm the card is valid and has sufficient funds or credit.
2. Detect fraud patterns before money moves.
3. Keep account balances correct even when many transactions hit the same account at once.
4. Leave a complete, tamper-evident record of every decision for audit and compliance.

PayGuard is a simplified, self-contained version of this system, built to demonstrate correct, fast and well-designed C++.

---

## 3. Goals and Non-Goals

### Goals
- Authorize transactions with **low, measurable latency** (target: p99 under 50 µs per decision).
- Sustain **high throughput** (target: 100K+ transactions per second on a laptop).
- Detect fraud using **pluggable, configurable rules**, where adding a rule requires no change to the engine.
- Guarantee **correctness under concurrency**: no lost updates, no negative balances, no deadlocks.
- Produce **measurable KPIs**: throughput, latency percentiles, detection rate, false positive rate.
- Clean **OOP design** and well-chosen **data structures**, each justified by complexity.

### Non-Goals
- Real card network integration (ISO 8583, Visa/Mastercard rails).
- Real cryptography, PCI-DSS compliance, or handling real card numbers.
- A graphical UI (output is console plus report files).
- Distributed deployment across machines.
- A trained deep learning model (an ML scoring rule is a stretch goal only).

---

## 4. Users / Personas

| Persona | Need |
|---|---|
| **Payment network** (simulated) | Sends transactions, needs a fast approve or decline decision |
| **Fraud analyst** | Reviews flagged transactions, riskiest first |
| **Auditor / compliance** | Needs a complete, ordered log of every decision and the reason |
| **Operations / config owner** | Turns fraud rules on or off and tunes thresholds without recompiling |

---

## 5. Functional Requirements

### 5.1 Input

**Accounts file** (`data/accounts.csv`)
```
card_id,balance,credit_limit,daily_limit,home_city,status
C1001,5000.00,10000.00,2000.00,Ahmedabad,ACTIVE
C1002,120.50,0.00,500.00,Mumbai,BLOCKED
```

**Transactions file** (`data/transactions.csv`)
```
txn_id,card_id,merchant_id,amount,city,timestamp
T000001,C1001,M501,250.00,Ahmedabad,1728550000
T000002,C1001,M502,4800.00,London,1728550300
```

**Blocklists** (`data/blocked_merchants.txt`) and a **city coordinates table** (`data/cities.csv`) for the geo rule.

**Config file** (`config/rules.json`): which rules are enabled, plus their thresholds.

### 5.2 Authorization checks (hard rules: immediate DECLINE)

| ID | Check | Decline reason |
|---|---|---|
| A1 | Card exists | `UNKNOWN_CARD` |
| A2 | Card status is ACTIVE | `CARD_BLOCKED` |
| A3 | Amount is positive and valid | `INVALID_AMOUNT` |
| A4 | Balance + credit limit covers the amount | `INSUFFICIENT_FUNDS` |
| A5 | Today's total spend + amount within daily limit | `DAILY_LIMIT_EXCEEDED` |

### 5.3 Fraud rules (soft rules: contribute to a risk score)

| ID | Rule | Logic | Default threshold |
|---|---|---|---|
| F1 | **Velocity** | Too many transactions on one card in a short window | > 5 txns in 60 seconds |
| F2 | **Amount anomaly** | Amount far above this card's normal spending | > mean + 3 × std dev (after ≥ 10 txns of history) |
| F3 | **Geo-impossibility** | Two transactions in cities too far apart for the time between them | Implied speed > 900 km/h |
| F4 | **Blocked merchant** | Merchant is on the blocklist | Exact match |
| F5 | **Round-amount burst** *(optional)* | Several large round amounts in a row (common in card testing) | ≥ 3 txns that are multiples of 1000 within 10 min |

Each rule returns a `RuleResult { bool triggered; double score; std::string reason; }`.

### 5.4 Decision logic

1. Run hard checks A1–A5. If any fails, the decision is **DECLINE** with that reason.
2. Run all enabled fraud rules and sum the scores into a **risk score (0–100)**.
3. Decide:
   - risk score **≥ 70**: **DECLINE** (`FRAUD_SUSPECTED`)
   - **40 ≤** risk score **< 70**: **FLAG** (approve but send to the analyst queue)
   - risk score **< 40**: **APPROVE**
4. On APPROVE or FLAG, debit the account atomically.
5. Write the decision to the audit log.

Thresholds are configurable in `rules.json`.

### 5.5 Output

- **Audit log** (`output/audit.log`), append-only, one line per transaction:
  `T000002 | C1001 | 4800.00 | FLAG | score=55 | GEO_IMPOSSIBLE,AMOUNT_ANOMALY | 12.4us`
- **Alert queue report** (`output/alerts.csv`): flagged transactions sorted by risk score, highest first.
- **Metrics report** (`output/metrics.txt`): throughput, latency percentiles, decision counts, detection accuracy (see Section 9).

### 5.6 Synthetic data generator

A separate tool (`tools/generate_data`) that creates N accounts and M transactions, **with known injected fraud** (velocity bursts, geo jumps, unusually large amounts) and a ground truth label per transaction. This is what makes detection rate and false positive rate measurable.

Optional: also evaluate on the public Kaggle *Credit Card Fraud Detection* dataset, mapped to the input format.

---

## 6. Non-Functional Requirements

| Requirement | Target |
|---|---|
| Throughput | ≥ 100K transactions/sec (multithreaded) |
| Latency | p50 < 10 µs, p99 < 50 µs per decision (excluding I/O) |
| Scalability | Throughput stays roughly flat from 10K to 1M cards (O(1) lookups) |
| Correctness | Zero balance inconsistencies under concurrent load |
| Memory | Bounded per card (no unbounded transaction history) |
| Extensibility | New fraud rule = one new class + one config entry |
| Portability | Builds on Linux, macOS and Windows with CMake |

---

## 7. System Architecture

```
 ┌────────────────┐     ┌──────────────────┐     ┌─────────────────────────┐
 │ TransactionSrc │ ──▶ │ Thread-safe Queue│ ──▶ │ Worker Threads (N)      │
 │ (CSV reader /  │     │ (bounded)        │     │  AuthorizationEngine    │
 │  generator)    │     └──────────────────┘     │   ├─ hard checks (A1–A5)│
 └────────────────┘                              │   ├─ FraudRule[] (F1–F5)│
                                                 │   └─ DecisionMaker      │
                                                 └───────────┬─────────────┘
                                                             │
                          ┌──────────────────────────────────┼─────────────────┐
                          ▼                                  ▼                 ▼
                 ┌─────────────────┐             ┌────────────────────┐ ┌─────────────┐
                 │ AccountStore    │             │ AuditLogger        │ │ AlertQueue  │
                 │ (hash map,      │             │ (append-only file) │ │ (max-heap)  │
                 │  per-acct lock) │             └────────────────────┘ └─────────────┘
                 └─────────────────┘
                          ▲
                 ┌─────────────────┐
                 │ MetricsCollector│  (latency samples, counters)
                 └─────────────────┘
```

**Flow:** reader → queue → worker pulls transaction → lookup account → hard checks → fraud rules → decision → debit → audit log → alert queue if flagged → metrics recorded.

---

## 8. Detailed Design

### 8.1 Core classes

| Class | Responsibility |
|---|---|
| `Transaction` | Plain data: id, card, merchant, amount, city, timestamp |
| `Account` | Encapsulates balance, limits, status and per-card stats; thread-safe `debit()` |
| `CardProfile` | Per-card fraud state: recent timestamps (deque), running mean/variance, last city and time |
| `AccountStore` | Owns all accounts; O(1) lookup by card ID |
| `FraudRule` (abstract) | Interface: `evaluate(const Transaction&, const CardProfile&)` |
| `VelocityRule`, `AmountAnomalyRule`, `GeoImpossibilityRule`, `BlocklistRule` | Concrete rules |
| `RuleFactory` | Builds the rule list from `rules.json` |
| `AuthorizationEngine` | Runs hard checks, rules and decision logic for one transaction |
| `TransactionQueue` | Bounded, thread-safe producer-consumer queue |
| `AuditLogger` | Append-only, thread-safe log writer (RAII file handle) |
| `AlertQueue` | Max-heap of flagged transactions by risk score |
| `AlertListener` (interface) | Observer notified on FLAG/DECLINE (console, file, heap) |
| `MetricsCollector` | Records latencies and counts; computes percentiles |
| `Decision` | Enum: `APPROVE`, `DECLINE`, `FLAG`, plus reasons and score |

### 8.2 Data structures

| Component | Data structure | Complexity | Why |
|---|---|---|---|
| Account lookup | `std::unordered_map<std::string, Account>` | O(1) avg | Every transaction starts with a lookup; ordering not needed |
| Velocity rule | `std::deque<int64_t>` per card (sliding window) | Amortized O(1) | Push new timestamp, pop expired ones from the front |
| Amount anomaly | Running mean/variance (Welford's algorithm) | O(1) time and memory | No need to store full spending history |
| Geo rule | Last city + timestamp in `CardProfile`; `unordered_map<city, coords>` | O(1) | Only the previous transaction matters |
| Blocklists | `std::unordered_set<std::string>` | O(1) | Membership test |
| Transaction pipeline | Bounded queue (mutex + condition variable) | O(1) push/pop | Producer-consumer between reader and workers |
| Alert ranking | `std::priority_queue` (max-heap by score) | O(log n) insert, O(1) top | Analysts see riskiest first |
| Top-K risky cards | Min-heap of size K | O(n log K) | End-of-day report |
| Latency percentiles | `std::vector<double>` + `std::nth_element` | O(n) | p50/p95/p99 without fully sorting |
| Audit log | Append-only file / vector | O(1) append | Immutable, ordered record |

**Key claim to verify by benchmark:** every transaction is evaluated in **O(1) amortized time**, independent of the number of cards or past transactions.

### 8.3 OOP design

- **Abstraction:** `FraudRule` is a pure virtual interface; the engine never knows a rule's internals.
- **Inheritance:** each fraud rule derives from `FraudRule`.
- **Polymorphism:** the engine holds `std::vector<std::unique_ptr<FraudRule>>` and calls `evaluate()` on each.
- **Encapsulation:** `Account::balance_` is private; it changes only through `debit()` and `credit()`, which validate and lock.

```cpp
class FraudRule {
public:
    virtual ~FraudRule() = default;
    virtual RuleResult evaluate(const Transaction& txn,
                                const CardProfile& profile) const = 0;
    virtual std::string name() const = 0;
};

class VelocityRule : public FraudRule {
public:
    VelocityRule(int maxTxns, int windowSeconds);
    RuleResult evaluate(const Transaction& txn,
                        const CardProfile& profile) const override;
    std::string name() const override { return "VELOCITY"; }
private:
    int maxTxns_;
    int windowSeconds_;
};
```

**Design patterns used**

| Pattern | Where |
|---|---|
| Strategy | Each `FraudRule` is an interchangeable strategy |
| Factory | `RuleFactory` creates rules from config |
| Observer | `AlertListener`s notified on flagged/declined transactions |
| Producer-consumer | `TransactionQueue` between reader and workers |
| RAII | `std::lock_guard`, file handles in `AuditLogger` |

**SOLID highlights**
- *Single responsibility:* logging, metrics, rules and storage are separate classes.
- *Open/closed:* add rules without editing `AuthorizationEngine`.
- *Dependency inversion:* the engine depends on the `FraudRule` and `AlertListener` interfaces, not concrete classes.

### 8.4 Concurrency design

- **N worker threads** (configurable, default = hardware threads) pull from the shared queue.
- **Per-account locking:** each `Account` has its own `std::mutex`, so transactions on different cards never block each other.
- **Atomic debit:** check-and-debit happens under one lock, so two threads cannot both spend the last ₹500.
- **Per-card ordering:** to keep velocity and geo rules correct, transactions are routed by `hash(card_id) % N` so a card is always handled by the same worker (sharding). This also removes most lock contention.
- **Audit logger:** single writer thread with its own queue, so workers never block on disk I/O.
- **Metrics:** per-thread latency buffers, merged at the end, to avoid contention.

### 8.5 Error handling

- Malformed CSV rows are skipped, counted and reported (never crash the engine).
- Unknown card or city leads to a decline with a reason, not an exception.
- Config errors fail fast at startup with a clear message.

---

## 9. KPIs and Success Metrics

| Category | KPI | How measured | Target |
|---|---|---|---|
| Performance | Throughput (txns/sec) | Total txns / wall-clock time | ≥ 100K |
| Performance | Latency p50 / p95 / p99 | `std::chrono::steady_clock` per decision | p99 < 50 µs |
| Performance | Speedup vs single thread | 1 thread vs N threads | ≥ 3× on 4+ cores |
| Scalability | Throughput at 10K / 100K / 1M cards | Benchmark runs | Within ~10% of each other |
| Fraud | Detection rate (recall) | Flagged-or-declined fraud / total injected fraud | ≥ 90% |
| Fraud | False positive rate | Legit txns flagged / total legit txns | ≤ 2% |
| Fraud | Precision | True fraud / total flagged | Report value |
| Fraud | Per-rule hit counts | Counters per rule | Report value |
| Correctness | Balance consistency | Sum of balances before − approved spend = sum after | 0 discrepancies |
| Correctness | Concurrency stress test | 1M random transfers across 8 threads | 0 negative balances, 0 deadlocks |
| Quality | Unit test coverage | gcov / lcov | ≥ 80% |
| Robustness | Malformed rows handled | Inject bad rows | 100% skipped, 0 crashes |

**Sample metrics report**
```
===== PayGuard Run Summary =====
Transactions processed : 1,000,000
Wall time              : 6.8 s
Throughput             : 147,058 txns/sec
Latency p50 / p95 / p99: 4.1 / 11.7 / 28.3 us
Decisions              : APPROVE 962,104 | FLAG 21,388 | DECLINE 16,508
Fraud recall           : 93.4%
False positive rate    : 1.6%
Balance check          : PASSED (0 discrepancies)
```
*(Numbers above are illustrative placeholders. Replace them with your real measured results.)*

---

## 10. Testing Strategy

| Type | What | Tool |
|---|---|---|
| Unit | Each fraud rule in isolation (triggers / doesn't trigger at boundaries) | GoogleTest |
| Unit | `Account::debit` (insufficient funds, limits, blocked card) | GoogleTest |
| Unit | Sliding window correctness, Welford mean/variance against brute force | GoogleTest |
| Integration | End-to-end on small hand-written CSV with known expected decisions | GoogleTest |
| Concurrency | Many threads hammering the same few accounts; assert totals match | GoogleTest + ThreadSanitizer |
| Memory | No leaks or invalid accesses | AddressSanitizer / Valgrind |
| Performance | Throughput and latency at different scales and thread counts | Google Benchmark |
| Accuracy | Detection rate and FPR on synthetic labelled data | Custom evaluator |

---

## 11. Tech Stack

- **Language:** C++17 (`std::optional`, structured bindings, `std::string_view`)
- **Build:** CMake
- **Testing:** GoogleTest
- **Benchmarking:** Google Benchmark
- **Config parsing:** nlohmann/json (header-only)
- **Tooling:** clang-format, ThreadSanitizer, AddressSanitizer
- **CI (optional):** GitHub Actions running build + tests on every push

---

## 12. Project Structure

```
payguard/
├── CMakeLists.txt
├── README.md
├── config/
│   └── rules.json
├── data/
│   ├── accounts.csv
│   ├── transactions.csv
│   ├── blocked_merchants.txt
│   └── cities.csv
├── include/payguard/
│   ├── Transaction.h
│   ├── Account.h
│   ├── CardProfile.h
│   ├── AccountStore.h
│   ├── AuthorizationEngine.h
│   ├── TransactionQueue.h
│   ├── AuditLogger.h
│   ├── AlertQueue.h
│   ├── MetricsCollector.h
│   └── rules/
│       ├── FraudRule.h
│       ├── VelocityRule.h
│       ├── AmountAnomalyRule.h
│       ├── GeoImpossibilityRule.h
│       ├── BlocklistRule.h
│       └── RuleFactory.h
├── src/
│   ├── main.cpp
│   └── ... (matching .cpp files)
├── tools/
│   ├── generate_data.cpp
│   └── evaluate.cpp
├── tests/
│   ├── test_rules.cpp
│   ├── test_account.cpp
│   ├── test_engine.cpp
│   └── test_concurrency.cpp
├── benchmarks/
│   └── bench_engine.cpp
└── output/
```

---

## 13. Milestones (4 weeks)

### Week 1: Core single-threaded engine
- [ ] CMake project, folder structure, GoogleTest wired up
- [ ] `Transaction`, `Account`, `AccountStore`, CSV loaders
- [ ] Hard checks A1–A5 and decision logic
- [ ] `AuditLogger` (simple, synchronous)
- [ ] Unit tests for accounts and hard checks
- **Deliverable:** reads CSVs, approves/declines, writes audit log

### Week 2: Fraud rules and OOP design
- [ ] `FraudRule` interface + Velocity, AmountAnomaly, GeoImpossibility, Blocklist rules
- [ ] `CardProfile` with deque window and Welford stats
- [ ] `RuleFactory` + `rules.json` config
- [ ] Risk scoring, FLAG decision, `AlertQueue` (heap), Observer listeners
- [ ] Synthetic data generator with labelled fraud
- [ ] Unit tests for every rule
- **Deliverable:** full fraud detection, single-threaded, with accuracy report

### Week 3: Concurrency and correctness
- [ ] Bounded `TransactionQueue`, worker thread pool
- [ ] Per-account locking and card-based sharding
- [ ] Async audit logger
- [ ] Concurrency stress tests under ThreadSanitizer
- [ ] Balance-consistency check
- **Deliverable:** multithreaded engine with zero inconsistencies

### Week 4: Measurement and polish
- [ ] `MetricsCollector` with p50/p95/p99
- [ ] Google Benchmark: throughput vs threads, vs number of cards
- [ ] Evaluate on Kaggle dataset (optional)
- [ ] README with architecture diagram, design decisions, results table
- [ ] Clean up, clang-format, GitHub Actions CI
- **Deliverable:** finished repo with measured KPIs ready for the resume

---

## 14. Stretch Goals

1. **ML scoring rule:** train a logistic regression in Python on the Kaggle dataset, export weights, and implement `MLScoreRule` in C++ that computes the score at runtime. Ties into your ML background.
2. **Custom bloom filter** for blocklists; compare memory and speed against `unordered_set`.
3. **Lock-free ring buffer** replacing the mutex queue; benchmark the difference.
4. **TCP server mode:** clients send transactions over a socket and get decisions back.
5. **Crash recovery:** replay the audit log to rebuild account balances after a restart.
6. **Hot reload** of `rules.json` without restarting the engine.

---

## 15. Interview Talking Points

- **"Walk me through a transaction."** Queue → worker → account lookup → hard checks → rules → score → decision → debit → log.
- **"Why `unordered_map` over `map`?"** O(1) average lookup; no ordering needed.
- **"How does the velocity rule work?"** Sliding window with a deque; amortized O(1).
- **"How do you compute a card's average without storing history?"** Welford's online algorithm.
- **"How do you add a new rule?"** One class implementing `FraudRule` plus a config entry; the engine is unchanged (open/closed principle).
- **"How do you prevent double spending across threads?"** Check-and-debit under a per-account lock; sharding by card keeps ordering.
- **"Why a virtual destructor in `FraudRule`?"** Deleting a derived rule through a base pointer would otherwise be undefined behaviour.
- **"`unique_ptr` vs `shared_ptr`?"** The engine solely owns its rules.
- **"How did you measure latency?"** `steady_clock` around each decision, per-thread buffers, percentiles via `nth_element`.
- **"What's the tradeoff between detection rate and false positives?"** Lowering the threshold catches more fraud but annoys more genuine customers; show your threshold sweep results.

---

## 16. Resume Entry (fill with real numbers after benchmarking)

**PayGuard: Real-Time Payment Authorization & Fraud Detection Engine** | C++17, CMake, GoogleTest, Google Benchmark
- Built a multithreaded transaction authorization engine validating balance, credit and daily limits, sustaining **__K transactions/sec** with **p99 latency of __ µs** across **__M** transactions.
- Designed an extensible fraud rule engine (velocity, amount-anomaly, geo-impossibility, blocklist) using polymorphic Strategy and Factory patterns, achieving **__% fraud recall** at a **__% false positive rate** on **__K** labelled transactions.
- Guaranteed correctness under concurrency via per-account locking and card-based sharding, verified by stress tests with **zero balance inconsistencies** across **__M** concurrent operations.

---

## 17. Open Questions

- Fixed-point integer (paise) vs `double` for money? **Recommendation:** store amounts as `int64_t` paise to avoid floating-point rounding errors. This is a good interview point in itself.
- Should FLAG transactions be debited immediately or held? (Default: debit, as real networks approve and review later.)
- Single combined risk score vs per-rule hard thresholds? (Default: combined score, configurable.)
