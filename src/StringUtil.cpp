#include "payguard/StringUtil.h"

#include <cctype>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>

namespace payguard {

namespace {
bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}
}  // namespace

std::string_view trim(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && isSpace(text[begin])) {
        ++begin;
    }
    while (end > begin && isSpace(text[end - 1])) {
        --end;
    }
    return text.substr(begin, end - begin);
}

void splitFields(std::string_view line, char delim, std::vector<std::string_view>& out) {
    out.clear();
    std::size_t start = 0;
    while (true) {
        const std::size_t pos = line.find(delim, start);
        if (pos == std::string_view::npos) {
            out.push_back(trim(line.substr(start)));
            return;
        }
        out.push_back(trim(line.substr(start, pos - start)));
        start = pos + 1;
    }
}

std::optional<std::int64_t> parseInt64(std::string_view text) {
    text = trim(text);
    if (!text.empty() && text[0] == '+') {
        text.remove_prefix(1);
    }
    if (text.empty()) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc() || ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<double> parseDouble(std::string_view text) {
    text = trim(text);
    if (text.empty() || text.size() > 64) {
        return std::nullopt;
    }
    // strtod needs a terminated buffer; floating-point from_chars is not
    // available on every standard library this project targets.
    char buf[65];
    text.copy(buf, text.size());
    buf[text.size()] = '\0';
    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(buf, &end);
    if (end != buf + text.size() || errno == ERANGE || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

bool equalsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::string withThousands(std::uint64_t value) {
    std::string digits = std::to_string(value);
    std::string out;
    out.reserve(digits.size() + digits.size() / 3);
    const std::size_t lead = digits.size() % 3;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i != 0 && (i + 3 - lead) % 3 == 0) {
            out.push_back(',');
        }
        out.push_back(digits[i]);
    }
    return out;
}

}  // namespace payguard
