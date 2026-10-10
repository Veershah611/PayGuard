#include "payguard/TransactionSource.h"

#include "payguard/StringUtil.h"

namespace payguard {

CsvTransactionSource::CsvTransactionSource(const std::string& path) : in_(path) {
    if (!in_) {
        throw IoError("cannot open '" + path + "'");
    }
}

bool CsvTransactionSource::next(Transaction& out) {
    while (std::getline(in_, line_)) {
        ++lineNumber_;
        const std::string_view view = trim(line_);
        if (view.empty() || view.front() == '#') {
            continue;
        }
        splitFields(view, ',', fields_);
        if (firstRow_) {
            firstRow_ = false;
            if (equalsIgnoreCase(fields_.front(), "txn_id")) {
                continue;
            }
        }
        ++stats_.rows;
        if (!parseTransactionRow(fields_, out, error_)) {
            stats_.reject(lineNumber_, error_);
            continue;
        }
        ++stats_.loaded;
        return true;
    }
    return false;
}

bool VectorTransactionSource::next(Transaction& out) {
    if (position_ >= transactions_.size()) {
        return false;
    }
    out = transactions_[position_++];
    return true;
}

}  // namespace payguard
