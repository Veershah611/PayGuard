#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace payguard {

// Removes leading and trailing whitespace (including '\r' from CRLF files).
std::string_view trim(std::string_view text);

// Splits `line` on `delim` into trimmed views that point into `line`.
// `out` is cleared first so callers can reuse one vector per row.
void splitFields(std::string_view line, char delim, std::vector<std::string_view>& out);

std::optional<std::int64_t> parseInt64(std::string_view text);
std::optional<double> parseDouble(std::string_view text);

bool equalsIgnoreCase(std::string_view a, std::string_view b);

// "1234567" -> "1,234,567"
std::string withThousands(std::uint64_t value);

}  // namespace payguard
