#include "core/Hyphen.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <sstream>
#include <string>
#include <unordered_map>

namespace composix::core {
namespace hyphen_data {
extern const char* const kPatterns;
extern const char* const kExceptions;
}  // namespace hyphen_data

namespace {
struct Tables {
    // letters ("ach") -> the digits between / around them (size letters + 1)
    std::unordered_map<std::string, std::vector<std::uint8_t>> patterns;
    std::unordered_map<std::string, std::vector<int>>          exceptions;   // "table" -> {2}
    std::size_t longest = 0;
};

const Tables& tables() {
    static const Tables t = [] {
        Tables t;
        std::istringstream in(hyphen_data::kPatterns);
        std::string tok;
        while (in >> tok) {
            std::string letters;
            std::vector<std::uint8_t> digits(1, 0);
            for (char c : tok) {
                if (c >= '0' && c <= '9') digits.back() = static_cast<std::uint8_t>(c - '0');
                else { letters += c; digits.push_back(0); }
            }
            t.longest = std::max(t.longest, letters.size());
            t.patterns.emplace(std::move(letters), std::move(digits));
        }
        std::istringstream ex(hyphen_data::kExceptions);
        while (ex >> tok) {
            std::string word;
            std::vector<int> at;
            for (char c : tok) {
                if (c == '-') at.push_back(static_cast<int>(word.size()));
                else word += c;
            }
            t.exceptions.emplace(std::move(word), std::move(at));
        }
        return t;
    }();
    return t;
}
}  // namespace

std::vector<int> hyphen_points(std::string_view word, int left, int right) {
    const int n = static_cast<int>(word.size());
    if (n < left + right) return {};
    std::string w;
    w.reserve(word.size());
    for (unsigned char c : word) {
        if (!std::isalpha(c) || c >= 0x80) return {};
        w += static_cast<char>(std::tolower(c));
    }
    const auto& t = tables();
    std::vector<int> out;
    if (const auto it = t.exceptions.find(w); it != t.exceptions.end()) {
        for (int k : it->second) if (k >= left && n - k >= right) out.push_back(k);
        return out;
    }
    const std::string dotted = "." + w + ".";
    const int m = static_cast<int>(dotted.size());
    std::vector<std::uint8_t> v(static_cast<std::size_t>(m) + 1, 0);
    for (int i = 0; i < m; ++i)
        for (int len = 1; len <= static_cast<int>(t.longest) && i + len <= m; ++len) {
            const auto it = t.patterns.find(dotted.substr(static_cast<std::size_t>(i), static_cast<std::size_t>(len)));
            if (it == t.patterns.end()) continue;
            for (int k = 0; k <= len; ++k)
                v[static_cast<std::size_t>(i + k)] = std::max(v[static_cast<std::size_t>(i + k)], it->second[static_cast<std::size_t>(k)]);
        }
    // v[k + 1]: between word[k - 1] and word[k] (the leading dot shifts by one).
    for (int k = left; k <= n - right; ++k)
        if (v[static_cast<std::size_t>(k + 1)] % 2) out.push_back(k);
    return out;
}

std::string hyphenated(std::string_view word) {
    std::string out;
    const auto at = hyphen_points(word);
    std::size_t j = 0;
    for (int k = 0; k < static_cast<int>(word.size()); ++k) {
        if (j < at.size() && at[j] == k) { out += '-'; ++j; }
        out += word[static_cast<std::size_t>(k)];
    }
    return out;
}

}  // namespace composix::core
