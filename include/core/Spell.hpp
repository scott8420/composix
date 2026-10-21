#pragma once
// core::spell -- spell check (s306). Hunspell's dictionaries when the build
// found Hunspell (COMPOSIX_HAVE_HUNSPELL); without it nothing loads and every
// word reads as fine. GTK-free; one dictionary for the process, loaded on
// demand. Not thread-safe -- the UI thread only.
//
// Words: letters (any script) with apostrophes inside them ("don't",
// "harbor’s"); a hyphen, a dash, a space, a mark (U+E000..) part them.
// Words with a digit are left alone, as are web / mail addresses.
#include "core/Doc.hpp"

#include <string>
#include <vector>

namespace composix::core::spell {

// Hunspell was built in.
bool built_in();
// Load `lang` ("en_US", "en_GB" ...) from the usual places (~/.local/share/
// hunspell, /usr/share/hunspell, /usr/share/myspell[/dicts]). `dirs` first
// when given (the selftest). False (and `err`) when its .aff / .dic aren't
// found; the one loaded before stays.
bool load(const std::string& lang, std::string* err = nullptr, const std::vector<std::string>& dirs = {});
bool loaded();
std::string language();   // "" none
// The languages whose dictionaries are installed, sorted.
std::vector<std::string> languages(const std::vector<std::string>& dirs = {});

// The user's own words: kept in `path` (one a line), read now, written as
// they're added. "" = kept for the session only.
void set_user_file(const std::string& path);
bool add_word(const std::string& word);      // into the user's words (and the file)
bool remove_word(const std::string& word);
std::vector<std::string> user_words();
// Ignored for the rest of the session (Ignore All).
void ignore(const std::string& word);
void clear_ignored();

// A word as typed: correct (or the user's, or ignored, or no dictionary)?
bool good(const std::string& word);
// What it might have been, best first (none without a dictionary).
std::vector<std::string> suggest(const std::string& word, int max = 8);

// A word in a paragraph: bytes [from, to).
struct Word {
    int para = 0, from = 0, to = 0;
    std::string text;
    bool operator==(const Word&) const = default;
};
// The words of a paragraph's text, in order (the ones a checker looks at).
std::vector<Word> words_of(const std::string& text, int para = 0);
// The misspelled words of a story, in order.
std::vector<Word> misses(const Story& s);
// The first misspelled word at or after `from` (a TextPos), wrapping to the
// story's start when `wrap`; para -1 none.
Word next_miss(const Story& s, TextPos from, bool wrap = true);

}  // namespace composix::core::spell
