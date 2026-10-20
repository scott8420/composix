#pragma once
// core/Zip -- reading entries out of a zip held in memory (s235; the
// swatch libraries' reader from s202 moved here for ODT import). Stored and
// deflated entries; sizes from the central directory (data descriptors).
#include <string>
#include <vector>

namespace composix::core::zip {

// The entry's bytes, "" when it isn't there (or can't be read). `ok`, when
// given, tells an empty entry from a missing one.
std::string entry(const std::string& zip, const std::string& name, bool* ok = nullptr);
// Its entries' names, in the central directory's order.
std::vector<std::string> names(const std::string& zip);
// s235: a zip of the given entries, stored (for tests and small writers).
std::string make_stored(const std::vector<std::pair<std::string, std::string>>& files);

}  // namespace composix::core::zip
