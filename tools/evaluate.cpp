// Offline evaluator: joins an audit log with ground-truth labels and reports
// detection accuracy, per-fraud-type recall and a risk-threshold sweep.
//
// The sweep re-thresholds the scores recorded in the audit log. Card state
// (velocity windows, trusted locations, spending baselines) evolved under the
// thresholds actually used for the run, so the sweep is a close approximation,
// not an exact re-run.

#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "payguard/Loaders.h"
#include "payguard/StringUtil.h"

using namespace payguard;

namespace {

struct Row {
    bool fraud = false;
    std::string type;
    double score = 0.0;
    bool declined = false;
    bool hardDecline = false;  // declined by A1–A5 / unknown city, not by score
    bool flagged = false;
};

void splitAudit(std::string_view line, std::vector<std::string_view>& out) {
    out.clear();
    constexpr std::string_view sep = " | ";
    std::size_t start = 0;
    while (true) {
        const std::size_t pos = line.find(sep, start);
        if (pos == std::string_view::npos) {
            out.push_back(trim(line.substr(start)));
            return;
        }
        out.push_back(trim(line.substr(start, pos - start)));
        start = pos + sep.size();
    }
}

std::string pct(std::uint64_t num, std::uint64_t den) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%6.2f%%",
                  den == 0 ? 0.0 : 100.0 * static_cast<double>(num) / static_cast<double>(den));
    return buf;
}

void usage() {
    std::cout << "Usage: evaluate --audit output/audit.log --labels data/generated/labels.csv\n"
                 "                [--thresholds 20,30,40,50,60,70,80]\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string auditPath = "output/audit.log";
    std::string labelsPath;
    std::vector<double> thresholds = {20, 30, 40, 45, 50, 60, 70, 80, 90};
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            usage();
            return 0;
        }
        if (i + 1 >= argc) {
            usage();
            return 2;
        }
        const std::string value = argv[++i];
        if (arg == "--audit") {
            auditPath = value;
        } else if (arg == "--labels") {
            labelsPath = value;
        } else if (arg == "--thresholds") {
            thresholds.clear();
            std::vector<std::string_view> parts;
            splitFields(value, ',', parts);
            for (auto p : parts) {
                const auto t = parseDouble(p);
                if (!t) {
                    std::cerr << "error: bad threshold '" << std::string(p) << "'\n";
                    return 2;
                }
                thresholds.push_back(*t);
            }
        } else {
            usage();
            return 2;
        }
    }
    if (labelsPath.empty()) {
        usage();
        return 2;
    }

    LabelMap labels;
    try {
        labels = loadLabels(labelsPath);
    } catch (const IoError& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 2;
    }
    std::ifstream audit(auditPath);
    if (!audit) {
        std::cerr << "error: cannot open '" << auditPath << "'\n";
        return 2;
    }

    std::vector<Row> rows;
    rows.reserve(labels.size());
    std::size_t unlabeled = 0;
    std::size_t badLines = 0;
    std::string line;
    std::vector<std::string_view> fields;
    while (std::getline(audit, line)) {
        if (trim(line).empty()) {
            continue;
        }
        splitAudit(line, fields);
        if (fields.size() != 7 || fields[4].substr(0, 6) != "score=") {
            ++badLines;
            continue;
        }
        const auto label = labels.find(std::string(fields[0]));
        if (label == labels.end()) {
            ++unlabeled;
            continue;
        }
        Row row;
        row.fraud = label->second.fraud;
        row.type = label->second.type;
        row.score = parseDouble(fields[4].substr(6)).value_or(0.0);
        row.declined = fields[3] == "DECLINE";
        row.flagged = fields[3] == "FLAG";
        row.hardDecline = row.declined && fields[5].substr(0, 15) != "FRAUD_SUSPECTED";
        rows.push_back(std::move(row));
    }

    // --- Actual decisions.
    std::uint64_t fraud = 0, legit = 0, fraudCaught = 0, fraudAlert = 0, legitAlert = 0;
    std::uint64_t legitHardDecline = 0;
    std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> byType;  // caught, total
    for (const Row& r : rows) {
        const bool alert = r.flagged || (r.declined && !r.hardDecline);
        if (r.fraud) {
            ++fraud;
            fraudCaught += (r.flagged || r.declined) ? 1 : 0;
            fraudAlert += alert ? 1 : 0;
            auto& t = byType[r.type.empty() ? "UNSPECIFIED" : r.type];
            t.first += (r.flagged || r.declined) ? 1 : 0;
            ++t.second;
        } else {
            ++legit;
            legitAlert += alert ? 1 : 0;
            legitHardDecline += r.hardDecline ? 1 : 0;
        }
    }

    std::cout << "===== PayGuard Evaluation =====\n"
              << "Audit lines matched    : " << withThousands(rows.size()) << '\n'
              << "Unlabeled / bad lines  : " << unlabeled << " / " << badLines << "\n\n"
              << "                 actual fraud   actual legit\n";
    std::printf("fraud alert      %12s   %12s\n", withThousands(fraudAlert).c_str(),
                withThousands(legitAlert).c_str());
    std::printf("hard decline     %12s   %12s\n", withThousands(fraudCaught - fraudAlert).c_str(),
                withThousands(legitHardDecline).c_str());
    std::printf("approved         %12s   %12s\n", withThousands(fraud - fraudCaught).c_str(),
                withThousands(legit - legitAlert - legitHardDecline).c_str());
    std::cout << "\nRecall (flag or decline) : " << pct(fraudCaught, fraud) << '\n'
              << "Rule recall              : " << pct(fraudAlert, fraud) << '\n'
              << "False positive rate      : " << pct(legitAlert, legit) << '\n'
              << "Precision                : " << pct(fraudAlert, fraudAlert + legitAlert) << "\n\n"
              << "Recall by fraud type:\n";
    for (const auto& [type, counts] : byType) {
        std::printf("  %-18s %8s / %-8s %s\n", type.c_str(), withThousands(counts.first).c_str(),
                    withThousands(counts.second).c_str(), pct(counts.first, counts.second).c_str());
    }

    // --- Threshold sweep: alert when score >= t.
    std::cout << "\nThreshold sweep (alert if score >= t; hard declines always count as caught)\n"
              << "     t     recall      FPR   precision\n";
    for (const double t : thresholds) {
        std::uint64_t caught = 0, fAlert = 0, lAlert = 0;
        for (const Row& r : rows) {
            const bool alert = r.score >= t;
            if (r.fraud) {
                caught += (alert || r.hardDecline) ? 1 : 0;
                fAlert += alert ? 1 : 0;
            } else {
                lAlert += alert ? 1 : 0;
            }
        }
        std::printf("  %4.0f   %s  %s   %s\n", t, pct(caught, fraud).c_str(), pct(lAlert, legit).c_str(),
                    pct(fAlert, fAlert + lAlert).c_str());
    }
    return 0;
}
