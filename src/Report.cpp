#include "payguard/Report.h"

#include <cstdio>

#include "payguard/StringUtil.h"

namespace payguard {

namespace {

std::string fixed(double value, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, value);
    return buf;
}

std::string count(std::uint64_t value) { return withThousands(value); }

void heading(std::ostream& out, const char* title) { out << "\n--- " << title << " ---\n"; }

}  // namespace

std::string formatPercent(double fraction) { return fixed(fraction * 100.0, 2) + "%"; }

void writeMetricsReport(std::ostream& out, const RunReport& r) {
    const auto& m = r.metrics;
    const auto& lat = m.latency;

    out << "===== PayGuard Run Summary =====\n";
    out << "Transactions processed : " << count(m.total) << '\n';
    out << "Malformed rows skipped : " << count(r.transactionStats.malformed) << '\n';
    out << "Accounts loaded        : " << count(r.accounts) << '\n';
    out << "Worker threads         : " << r.threads << '\n';
    out << "Wall time              : " << fixed(r.pipeline.wallSeconds, 3) << " s\n";
    out << "Throughput             : " << count(static_cast<std::uint64_t>(r.pipeline.throughput()))
        << " txns/sec\n";
    out << "Latency p50 / p95 / p99: " << fixed(lat.p50, 2) << " / " << fixed(lat.p95, 2)
        << " / " << fixed(lat.p99, 2) << " us\n";
    out << "Latency p99.9 / max    : " << fixed(lat.p999, 2) << " / " << fixed(lat.max, 2)
        << " us (mean " << fixed(lat.mean, 2) << ")\n";
    out << "Decisions              : APPROVE " << count(m.approved) << " | FLAG "
        << count(m.flagged) << " | DECLINE " << count(m.declined) << '\n';
    if (m.hasLabels) {
        out << "Fraud recall           : " << formatPercent(m.accuracy.recall()) << '\n';
        out << "False positive rate    : " << formatPercent(m.accuracy.falsePositiveRate())
            << '\n';
        out << "Precision              : " << formatPercent(m.accuracy.precision()) << '\n';
    }
    out << "Balance check          : " << (r.balance.passed() ? "PASSED" : "FAILED") << " ("
        << (r.balance.discrepancy() == 0 ? 0 : 1) << " discrepancies, "
        << r.balance.overdrawnAccounts << " overdrawn accounts)\n";

    heading(out, "Decline reasons");
    for (std::size_t i = 1; i < kDeclineReasonCount; ++i) {
        if (m.declineReasons[i] != 0) {
            char line[96];
            std::snprintf(line, sizeof(line), "%-22s : ",
                          std::string(toString(static_cast<DeclineReason>(i))).c_str());
            out << line << count(m.declineReasons[i]) << '\n';
        }
    }

    heading(out, "Rule hits");
    for (std::size_t i = 0; i < m.ruleNames.size(); ++i) {
        char line[96];
        std::snprintf(line, sizeof(line), "%-22s : ", m.ruleNames[i].c_str());
        out << line << count(m.ruleHits[i]) << '\n';
    }

    if (m.hasLabels) {
        const auto& a = m.accuracy;
        heading(out, "Detection accuracy (vs ground truth)");
        out << "Fraud transactions     : " << count(a.fraudTotal) << '\n';
        out << "  caught (flag/decline): " << count(a.fraudCaught) << "  recall "
            << formatPercent(a.recall()) << '\n';
        out << "  raised by fraud rules: " << count(a.fraudAlerted) << "  rule recall "
            << formatPercent(a.ruleRecall()) << '\n';
        out << "Legit transactions     : " << count(a.legitTotal) << '\n';
        out << "  false fraud alerts   : " << count(a.legitAlerted) << "  FPR "
            << formatPercent(a.falsePositiveRate()) << '\n';
        out << "Precision              : " << formatPercent(a.precision()) << '\n';
        if (a.unlabeled != 0) {
            out << "Unlabeled transactions : " << count(a.unlabeled) << '\n';
        }
        if (!m.byFraudType.empty()) {
            out << "Recall by fraud type:\n";
            for (const auto& [type, counts] : m.byFraudType) {
                char line[128];
                std::snprintf(line, sizeof(line), "  %-20s : %s / %s  ", type.c_str(),
                              count(counts.caught).c_str(), count(counts.total).c_str());
                out << line
                    << formatPercent(counts.total == 0
                                         ? 0.0
                                         : static_cast<double>(counts.caught) /
                                               static_cast<double>(counts.total))
                    << '\n';
            }
        }
    }

    heading(out, "Balance consistency");
    out << "Initial total balance  : " << formatMoney(r.balance.initialTotal) << '\n';
    out << "Approved spend         : " << formatMoney(r.balance.approvedSpend) << '\n';
    out << "Final total balance    : " << formatMoney(r.balance.finalTotal) << '\n';
    out << "Discrepancy            : " << formatMoney(r.balance.discrepancy()) << '\n';

    heading(out, "Analyst queue");
    out << "Flagged for review     : " << count(r.alertsQueued) << " (see alerts.csv)\n";

    if (!r.topCards.empty()) {
        heading(out, "Top risky cards");
        for (std::size_t i = 0; i < r.topCards.size(); ++i) {
            const auto& c = r.topCards[i];
            char line[160];
            std::snprintf(line, sizeof(line), "%2zu. %-12s alerts=%-4llu total_score=%-7.0f max=%.0f\n",
                          i + 1, c.cardId.c_str(), static_cast<unsigned long long>(c.alerts),
                          c.totalScore, c.maxScore);
            out << line;
        }
    }

    if (!r.transactionStats.sampleErrors.empty() || !r.accountStats.sampleErrors.empty()) {
        heading(out, "Input problems (first few)");
        for (const auto& e : r.accountStats.sampleErrors) {
            out << "accounts.csv     " << e << '\n';
        }
        for (const auto& e : r.transactionStats.sampleErrors) {
            out << "transactions.csv " << e << '\n';
        }
    }
}

void writeAlertsCsv(std::ostream& out, const std::vector<Alert>& alerts) {
    out << "rank,txn_id,card_id,merchant_id,amount,city,timestamp,risk_score,reasons\n";
    std::size_t rank = 0;
    for (const auto& a : alerts) {
        out << ++rank << ',' << a.txnId << ',' << a.cardId << ',' << a.merchantId << ','
            << formatMoney(a.amount) << ',' << a.city << ',' << a.timestamp << ','
            << fixed(a.riskScore, 0) << ',';
        for (std::size_t i = 0; i < a.reasons.size(); ++i) {
            out << (i == 0 ? "" : ";") << a.reasons[i];
        }
        out << '\n';
    }
}

}  // namespace payguard
