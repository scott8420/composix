#pragma once
// core::Hyphen (s038) -- where a word may break, by Liang's algorithm over
// the TeX US English patterns (third_party/hyphen, embedded at build time).
// GTK-free and Pango-free: the flow asks, the selftest checks.
#include <string>
#include <string_view>
#include <vector>

namespace composix::core {

// The places `word` may be hyphenated: byte offsets into it (a break before
// that byte), ascending. ASCII letters only (any other byte: no places).
// At least `left` letters before a break and `right` after it (TeX's
// American English minimums: 2 and 3); words shorter than left + right
// don't break.
std::vector<int> hyphen_points(std::string_view word, int left = 2, int right = 3);

// The word with its breaks shown ("hy-phen-ation"), for tests and logs.
std::string hyphenated(std::string_view word);

}  // namespace composix::core
