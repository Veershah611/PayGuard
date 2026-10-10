# PayGuard

**Real-time payment authorization and fraud detection engine in C++17.**

PayGuard reads a stream of card transactions and, for each one, decides in real time whether to
**APPROVE**, **DECLINE** or **FLAG** it for analyst review. It checks account rules (card status,
balance, credit and daily limits), runs a set of pluggable fraud rules (velocity, amount anomaly,
geo-impossibility, blocked merchants, round-amount bursts), debits the account atomically, and
records every decision in an append-only audit log.

```
T000002 | C1001 | 4800.00 | FLAG | score=45 | GEO_IMPOSSIBLE | 3.8us
T000015 | C1005 | 75.00 | DECLINE | score=90 | FRAUD_SUSPECTED,VELOCITY,GEO_IMPOSSIBLE | 0.4us
```

---

## Quick start

Requirements: CMake ≥ 3.20 and a C++17 compiler (GCC 9+, Clang 10+, MSVC 2019+). GoogleTest,
Google Benchmark and nlohmann/json are fetched automatically by CMake.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Run on the hand-written sample data (`data/`):

```bash
./build/payguard --threads 4
```

Generate a labelled synthetic data set and measure accuracy:

```bash
./build/generate_data --accounts 10000 --transactions 1000000 --malformed 20
./build/payguard --data-dir data/generated
./build/evaluate --audit output/audit.log --labels data/generated/labels.csv
```

Each run writes three files to `output/`:

| File | Contents |
|---|---|
| `audit.log` | One line per decision: id, card, amount, verdict, score, reasons, latency |
| `alerts.csv` | FLAGged transactions, riskiest first (the analyst queue) |
| `metrics.txt` | Throughput, latency percentiles, decision counts, rule hits, accuracy, balance check, top risky cards |

### Command-line options

```
payguard [--data-dir DIR] [--accounts F] [--transactions F] [--blocklist F] [--cities F]
         [--labels F] [--config F] [--threads N] [--output DIR] [--console-alerts N]
         [--top-k K] [--no-audit]
generate_data [--accounts N] [--transactions M] [--fraud-rate F] [--days D]
              [--malformed K] [--seed S] [--out DIR]
evaluate --audit FILE --labels FILE [--thresholds 20,40,70]
```

---

## Architecture

```
 ┌────────────────┐   hash(card) % N   ┌──────────────────┐     ┌─────────────────────────┐
 │ TransactionSrc │ ─────────────────▶ │ bounded queue ×N │ ──▶ │ worker thread ×N        │
 │ (CSV stream /  │    (batches of 64) │ (mutex + 2 CVs)  │     │  AuthorizationEngine    │
 │  vector)       │                    └──────────────────┘     │   ├─ hard checks A1–A5  │
 └────────────────┘                                             │   ├─ FraudRule[] F1–F5  │
                                                                │   └─ thresholds         │
                                                                └───────────┬─────────────┘
                       ┌────────────────────────────────────────────────────┼──────────────────┐
                       ▼                                ▼                   ▼                  ▼
              ┌─────────────────┐          ┌──────────────────────┐  ┌──────────────┐  ┌───────────────┐
              │ AccountStore    │          │ AuditLogger          │  │ AlertListener│  │ Metrics       │
              │ unordered_map + │          │ own writer thread,   │  │ observers:   │  │ per-thread    │
              │ per-account lock│          │ chunked, back-pressure│ │ heap, top-K, │  │ buffers,      │
              │ + CardProfile   │          └──────────────────────┘  │ console      │  │ merged at end │
              └─────────────────┘                                    └──────────────┘  └───────────────┘
```

**One transaction:** reader routes by card → worker pops a batch → `AccountStore` lookup →
take the account lock → A1–A5 → each enabled `FraudRule` → sum scores → verdict → debit (still
under the lock) → update card profile → release → audit line, observers, metrics.

### Decision logic

| Step | Outcome |
|---|---|
| A1 card exists | else `DECLINE UNKNOWN_CARD` |
| A2 card is ACTIVE | else `DECLINE CARD_BLOCKED` |
| A3 amount is positive and well-formed | else `DECLINE INVALID_AMOUNT` |
| city is in `cities.csv` (configurable) | else `DECLINE UNKNOWN_CITY` |
| A4 balance + credit limit covers amount | else `DECLINE INSUFFICIENT_FUNDS` |
| A5 today's spend + amount ≤ daily limit | else `DECLINE DAILY_LIMIT_EXCEEDED` |
| risk score ≥ 70 | `DECLINE FRAUD_SUSPECTED` |
| 40 ≤ risk score < 70 | `FLAG` (approved, debited, queued for an analyst) |
| risk score < 40 | `APPROVE` |

### Fraud rules (`config/rules.json`)

| Rule | Triggers when | Default score |
|---|---|---|
| `velocity` | more than 5 attempts on the card within 60 s | 45 |
| `amount_anomaly` | amount above mean + 3σ of the card's approved spending (≥ 10 txns of history), measured on log(amount) | 40 |
| `geo_impossibility` | implied travel speed from the last trusted location > 900 km/h (hops < 100 km ignored) | 45 |
| `blocked_merchant` | merchant is on the blocklist | 80 |
| `round_amount_burst` | ≥ 3 multiples of ₹1000 within 10 minutes | 40 |

Any single rule reaches FLAG, two rules together reach DECLINE, and a blocklisted merchant is
declined on its own. Thresholds, scores and every parameter are set in `rules.json`. Unknown rule
types, misspelt parameters and out-of-range values stop the program at startup with a clear
message.

---

## Design decisions

### Data structures

| Component | Structure | Cost | Why |
|---|---|---|---|
| Account lookup | `std::unordered_map<std::string, Account>` | O(1) avg | Every transaction starts with a lookup and nothing needs ordering. Read-only during processing, so lookups need no lock. |
| Velocity / burst window | `std::deque<Attempt>` per card | amortized O(1) | Push the new attempt, pop expired ones from the front. Capped at 256 entries and 1 hour, so memory per card is bounded. |
| Spending baseline | Welford running mean/variance (`RunningStats`) | O(1) time and memory | No spending history is stored. |
| Geo rule | last trusted city + timestamp, `unordered_map<city, coords>` | O(1) | Only the previous trusted location matters. |
| Blocklist | `std::unordered_set<std::string>` | O(1) | Membership test. |
| Pipeline | bounded queue per worker (mutex + 2 condition variables) | O(1) | Back-pressure on the reader; batches of 64 keep synchronisation off the hot path. |
| Analyst queue | `std::priority_queue` max-heap by score | O(log n) push | Riskiest first; ties broken by arrival order. |
| Top-K risky cards | min-heap of size K | O(n log K) | No need to sort every card. |
| Latency percentiles | `std::vector<double>` + `std::nth_element` | O(n) | p50/p95/p99/p99.9 without a full sort. |

### OOP and patterns

- **Strategy:** each fraud rule implements the pure-virtual `FraudRule` interface. The engine holds
  `std::vector<std::unique_ptr<FraudRule>>` and never sees concrete types. It solely owns its rules,
  hence `unique_ptr`. The base has a virtual destructor so deleting through a base pointer is
  well-defined.
- **Factory:** `RuleFactory` maps rule `type` strings to creators and builds the rule list from
  JSON.
- **Observer:** `AlertListener` subscribers (`AlertQueue`, `RiskyCardTracker`,
  `ConsoleAlertListener`) receive every FLAG and DECLINE.
- **Producer–consumer:** `TransactionQueue<T>` sits between the reader and the workers.
- **RAII:** `Account::Locked` holds the account mutex for one whole decision. `AuditLogger` owns
  its file and its writer thread, and the destructor drains and closes both.
- **Encapsulation:** `Account` balance, status and daily spend are private and change only through
  methods that hold the lock.

**Adding a rule:** write a class deriving from `FraudRule`, register one creator in the
`RuleFactory` constructor, and add an entry to `rules.json`. `AuthorizationEngine` does not change.

### Concurrency

- **Per-card sharding.** The reader routes each transaction to worker `hash(card_id) % N`, so a
  card is always handled by the same thread, in file order. Velocity and geo rules need that
  ordering, and it means two workers almost never contend for the same account.
- **Per-account locking.** Each `Account` has its own mutex. The funds check, the fraud rules that
  read the card's profile, and the debit all run under that one lock. Two threads therefore cannot
  both spend the last ₹500, even when sharding is bypassed (see
  `Concurrency.EngineOnSharedCardsHasNoLostUpdates`).
- **Deadlock-free transfers.** `Account::transfer` takes both locks with `std::scoped_lock`, which
  orders acquisition. The test suite runs 1M random transfers in both directions across 8 threads.
- **Asynchronous audit log.** Workers format their lines into a per-batch chunk and hand it to a
  single writer thread with one lock acquisition. A byte cap applies back-pressure if the disk
  falls behind.
- **Lock-free metrics.** Each worker records into its own cache-line-aligned buffer. Buffers are
  merged once, after the workers join.

### Money and correctness

- All amounts are `int64_t` **paise**, never `double`, so balances are exact. Parsing rejects
  sub-paisa precision and absurd magnitudes.
- **Balance check:** after every run, sum of balances before − approved spend must equal the sum
  after, and no balance may be below −credit_limit. The CLI exits non-zero if this fails.
- **Bad input never crashes the engine.** Malformed CSV rows are skipped, counted and sampled in the
  report. Unparseable or non-positive amounts are kept and declined as `INVALID_AMOUNT`, so they
  still appear in the audit trail.

### Choices beyond (or different from) the PRD

1. **Amount anomaly in log space by default.** Card spending is roughly log-normal, so "mean + 3σ"
   on raw amounts fires on 2–3% of perfectly normal purchases. On the 1M-transaction synthetic set,
   the linear test gave **3.25% FPR / 29% precision**; the same Welford z-score on `log(amount)`
   gave **0.76% FPR / 64% precision**, while recall on amount fraud only moved from 99.9% to 98.6%. Set
   `"scale": "linear"` to get the PRD's literal formula.
2. **Only clean approvals update the card's baseline.** Location and spending statistics are
   updated on APPROVE only, not on FLAG or DECLINE. If a flagged charge in London moved the card's
   "last location", the cardholder's next genuine purchase at home would also look
   geo-impossible. Every attempt, including declines, still counts towards velocity, because card
   testing shows up as a run of declines.
3. **Geo rule ignores hops shorter than 100 km** (`min_distance_km`), so neighbouring cities do not
   trip it.
4. **Unknown cities** are a hard decline (`UNKNOWN_CITY`) when a city table is loaded, per PRD §8.5.
   Turn this off with `"decline_unknown_city": false`.
5. **FLAG is debited immediately** (PRD open question: approve now, review later).

---

## Results

Measured on a Windows 11 laptop (18 logical cores, MSVC 19.51, Release). Data: synthetic, 10,000
cards and 1,000,008 transactions, 2% injected fraud (`generate_data --malformed 20`, seed 42).

| KPI | Target | Measured |
|---|---|---|
| Throughput, end to end (CSV parse → decision → audit log) | ≥ 100K txns/s | **1.2–1.6M txns/s** (4–8 workers) |
| Throughput, engine only via pipeline (in-memory source) | — | **2.06M txns/s** (8 workers) |
| Latency p50 / p95 / p99 per decision | p99 < 50 µs | **0.4 / 0.8 / 1.1 µs** |
| Speedup, 1 → 8 threads (pipeline benchmark) | ≥ 3× | **4.1×** (0.51M → 2.06M txns/s) |
| Single-thread decision cost at 10K / 100K / 1M cards | within ~10% | **0.80 / 1.32 / 1.49 µs** (see note) |
| Fraud recall (flagged or declined) | ≥ 90% | **75.4%** (see breakdown) |
| False positive rate (legit with a fraud alert) | ≤ 2% | **0.76%** |
| Precision | report | **63.7%** |
| Balance consistency | 0 discrepancies | **PASSED, 0** |
| Concurrency stress (1M transfers, 8 threads) | 0 negative, 0 deadlocks | **passed** |
| Malformed rows | 100% skipped, 0 crashes | **20 / 20 skipped** |

Recall by injected fraud type:

| Type | Recall | Why |
|---|---|---|
| Blocked merchant | 100% | exact match |
| Geo-impossible | 100% | |
| Amount anomaly | 98.6% | the misses stay under the card's log-space threshold |
| Round-amount burst | 78.6% | the first 2 charges of each burst cannot trigger a "≥ 3" rule |
| Velocity (card testing) | 56.9% | the first 5 charges of each 8–15 charge burst cannot trigger a "> 5" rule |

Every charge in a burst is labelled fraud, including the ones no streaming rule can see coming.
That keeps the recall figure honest, but caps it. Ways to raise recall: lower `max_txns`, or add
retroactive alerting that, once velocity fires, flags the card's earlier charges in the window for
review. The threshold sweep from `evaluate` shows the trade-off between FPR and recall.

**Note on scaling:** lookups are O(1) and per-card state is bounded, so cost per decision does not
depend on how much history exists. The step from 10K to 100K cards is a cache effect: about 10K
accounts fit in L2/L3, 100K+ do not. From 100K to 1M the cost stays within 13%.

**Note on latency:** on Windows `std::chrono::steady_clock` ticks every 100 ns, so the sub-µs
percentiles are quantised to 0.1 µs.

Reproduce the benchmarks with `./build/bench_engine`.

---

## Testing

81 GoogleTest cases in `tests/`:

| File | Covers |
|---|---|
| `test_util.cpp` | money parsing/formatting, CSV parsing, loaders, Welford against brute force, percentiles against a sorted reference, the queue |
| `test_account.cpp` | debit, credit limit, daily limit and day rollover, transfers, the store |
| `test_rules.cpp` | every rule at and around its boundaries, the sliding window, factory and config validation |
| `test_engine.cpp` | threshold boundaries, hard-check order, the end-to-end sample data with exact expected decisions and balances, audit format, alert heap, top-K, metrics |
| `test_concurrency.cpp` | parallel debits, shared-card engine stress, 1M concurrent transfers, a multithreaded pipeline that must match a sequential run decision-for-decision, MPMC queue, concurrent audit logging |

Sanitizers and coverage (GCC/Clang):

```bash
cmake -S . -B build-tsan -DPAYGUARD_SANITIZER=thread -DPAYGUARD_BUILD_BENCHMARKS=OFF
cmake -S . -B build-asan -DPAYGUARD_SANITIZER=address,undefined -DPAYGUARD_BUILD_BENCHMARKS=OFF
cmake -S . -B build-cov  -DPAYGUARD_COVERAGE=ON -DCMAKE_BUILD_TYPE=Debug
```

GitHub Actions (`.github/workflows/ci.yml`) builds and tests on Linux, macOS and Windows, and runs
the suite under ThreadSanitizer, AddressSanitizer + UBSan, and lcov coverage.

---

## Project layout

```
config/rules.json            rule set, thresholds, engine settings
data/                        small hand-written sample (accounts, transactions, blocklist, cities)
include/payguard/            public headers (rules/ holds the FraudRule hierarchy)
src/                         implementation + main.cpp (CLI)
tools/generate_data.cpp      synthetic data with labelled, injected fraud
tools/evaluate.cpp           accuracy report and threshold sweep from audit.log + labels
tests/                       GoogleTest suite (tests/data = fixtures)
benchmarks/bench_engine.cpp  Google Benchmark: card-count scaling, thread scaling, rule cost
```

## Limitations and next steps

- One reader thread parses the CSV, which caps end-to-end throughput at about 1.6M txns/s. Parallel
  parsing, or reading from several sources, would lift that.
- Per-card ordering assumes each card's transactions arrive in timestamp order, which the generator
  and sharding guarantee.
- Stretch goals from the PRD not yet built: ML scoring rule, Bloom-filter blocklist, lock-free ring
  buffer, TCP server mode, crash recovery from the audit log, hot reload of `rules.json`.
