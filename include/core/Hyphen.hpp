#pragma once
// core::Hyphen (s038) -- where a word may break, by Liang's algorithm over
// the TeX hyphenation patterns (third_party/hyphen, embedded at build time).
// GTK-free and Pango-free: the flow asks, the selftest checks.
// s190 (G4): in several languages -- US and British English, German (1996
// spelling), French, Spanish, Italian, Dutch, Portuguese -- over UTF-8
// letters (the accented ones included), each with its own minimums.
#include <string>
#include <string_view>
#include <vector>

namespace composix::core {

// A language the patterns cover: its code (a style's `lang`), its name, and
// TeX's minimums -- letters before / after a break.
struct HyphenLanguage {
    const char* code;    // "en-US", "de", ...
    const char* name;    // "English (US)", "German"
    int         left, right;
};
const std::vector<HyphenLanguage>& hyphen_languages();
// The language for a code -- an unknown or empty code: US English.
const HyphenLanguage& hyphen_language(std::string_view code);

// The places `word` may be hyphenated: byte offsets into it (a break before
// that byte), ascending. Letters only -- ASCII or accented (any other
// character: no places); an apostrophe belongs to a French word. At least
// `left` letters before a break and `right` after it; words shorter than
// left + right don't break. The language's own minimums when left / right
// are < 0.
std::vector<int> hyphen_points(std::string_view word, int left = -1, int right = -1, std::string_view lang = "en-US");

// The word with its breaks shown ("hy-phen-ation"), for tests and logs.
std::string hyphenated(std::string_view word, std::string_view lang = "en-US");

}  // namespace composix::core
