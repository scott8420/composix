#pragma once
// core/Settings -- the app's own small choices between runs (s052): a flat
// map of keys to strings ("colors.view" -> "list"), and its JSON pump.
// As Recents: the core owns the shape, the UI resolves the XDG path and
// hands it in; load is first-run tolerant and never throws.
//
// On disk: { "colors.view": "list", ... }. Non-string values are ignored.
#include <map>
#include <string>

namespace composix::core {

using AppSettings = std::map<std::string, std::string>;

AppSettings load_settings(const std::string& file);
bool     save_settings(const std::string& file, const AppSettings& s);

// s057: a whole number from the settings, kept within [lo, hi]; `fallback`
// when it's missing or isn't a number.
int setting_int(const AppSettings& s, const std::string& key, int fallback, int lo, int hi);

}  // namespace composix::core
