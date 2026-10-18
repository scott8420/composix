#pragma once
// layout::preflight_issues -- what would trouble a printer, checked live
// (s024). One list for the header's Preflight popover and Export's report.
//
// Each issue names where it is (page, frame) so a click can go there. Its
// level: Error (it will print wrong or not at all), Warning (it may), Info
// (worth knowing -- a spot plate).
#include "core/Doc.hpp"
#include "layout/Flow.hpp"

#include <functional>
#include <string>
#include <vector>

namespace composix::layout {

struct Issue {
    enum class Level { Error, Warning, Info };
    Level       level = Level::Warning;
    std::string kind;    // "overset", "missing", "modified", "lowres", "empty", "spot", "rgb", "transparency", "profile", "shift", s129 "xref"
    std::string text;    // "Overset text: Chapters"
    std::string page;    // where (a page; "" = nowhere in particular)
    std::string frame;
    std::string item;    // s089: a record it's about, when not a frame (a list: "stale_list")
    bool operator==(const Issue&) const = default;
};

struct PreflightOpts {
    int  standard = 0;          // 0 none, 1 PDF/X-1a, 2 PDF/X-4 (the export's setting)
    bool profile = false;       // an output profile is chosen
    std::string missing_profile;   // s032: the document's profile, not found on this computer
    // s053: the profile's file and whether RGB will be converted to it (Convert,
    // or PDF/X-1a) -- then an RGB colour that shifts noticeably is a warning.
    std::string profile_path;
    bool        convert = false;
    bool        perceptual = false;   // s085: the export's intent
    // A picture's link (the shell knows the folder); unset: links not checked.
    std::function<core::LinkStatus(const core::Asset&)> link;
};

std::vector<Issue> preflight_issues(const core::Doc& d, FlowCache& flows, const PreflightOpts& o);

// "3 problems" / "No problems" -- errors and warnings counted (not info).
int problem_count(const std::vector<Issue>& v);

}  // namespace composix::layout
