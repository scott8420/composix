#pragma once
// The selftest's shared harness (s196): the selftest was one 10,000-line main()
// -- the slowest file of the build by far, and serial. Its blocks now live in
// src/selftest/partN.cpp, compiled in parallel; main() (src/selftest.cpp)
// calls them in their old order, so a block may still lean on files an
// earlier one left behind. A new block goes at the end of the last part (or
// a new part: call it from main() in selftest.cpp, list it in CMakeLists.txt).
#include "core/Doc.hpp"
#include "layout/ThumbStore.hpp"
#include "layout/Flow.hpp"
#include "layout/Preflight.hpp"
#include "layout/ColorReport.hpp"
#include <cstring>
#include <plate/plate.hpp>
#include "layout/Export.hpp"
#include "layout/Psd.hpp"
#include <zlib.h>
#include "layout/BigBook.hpp"
#include "core/Outline.hpp"
#include "core/Recents.hpp"
#include "core/Settings.hpp"
#include "core/PlaceQueue.hpp"
#include "core/Shortcuts.hpp"
#include "core/Stones.hpp"
#include "core/Undo.hpp"
#include "core/TextMap.hpp"
#include "core/Hyphen.hpp"
#include "core/Swatches.hpp"
#include "core/Pathfinder.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace core = composix::core;

namespace cxtest {
// The sample without s018's wrapped picture (and s029's drop caps, s026's
// indents): the text tests written before those measure plain text.
void load_plain(composix::core::Doc& d);
// One verdict: PASS / FAIL with an optional detail; the counts add up in main.
void check(const std::string& name, bool ok, const std::string& detail = "");
int passes();
int failures();

// The FORWARD mapping, mirroring what a viewer's selection() would do: a
// (al,ac)-(cl,cc) mark -> V byte endpoints -> codepoint range + quote.
struct Fwd { int cp_start, cp_end, bstart, bend; std::string quote, V; };
Fwd forward(const std::vector<core::FlatLine>& flat, int al, int ac, int cl, int cc);
// Assert map_range inverts forward().
void roundtrip(const std::vector<core::FlatLine>& flat,
               int al, int ac, int cl, int cc, const std::string& what);

void part1();
void part2();
void part3();
void part4();
void part5();
void part6();
void part7();
void part8();
}  // namespace cxtest
using namespace cxtest;
