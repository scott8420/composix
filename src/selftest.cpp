// composix_selftest -- the headless core harness.
//
// A seed is only trustworthy if its paradigms are EXERCISED, not just described
// (CANON: "a claim in the doc isn't a fact until a consumer exercises it"). This
// runs the GTK-free core with no display, so a future you can trust the pump and
// the mapping before wiring any widget on top:
//   * Recents  -- move-to-front + trim + the JSON round-trip with stale-prune;
//   * TextMap  -- forward(selection) -> inverse(map_range) round-trips byte-exact
//                 across single-line / multi-line / cross-paragraph / UTF-8, and
//                 chrome lines are never landed on.
//
//   cmake --build build --target composix_selftest && ./build/composix_selftest
//   s196: the tests themselves are in src/selftest/part1..N.cpp.

#include "selftest/Selftest.hpp"

#include <iostream>

namespace cxtest {
namespace { int g_pass = 0, g_fail = 0; }
int passes() { return g_pass; }
int failures() { return g_fail; }
// The sample without s018's wrapped picture: the text tests written before
// wraps measure the chapter's opening text set plain.
void load_plain(composix::core::Doc& d) {
    composix::core::load_sample(d);
    composix::core::Doc::Loading load(d);
    auto f = *d.as<composix::core::Frame>("frame.11");
    f.wrap = composix::core::Wrap::None;
    d.put("frame.11", f);
    // ... and without s029's drop caps (the chapters' first paragraphs in Body)
    {
        auto st = *d.as<composix::core::Story>("story.1");
        for (auto& p : st.paras) if (p.style == "style.10") p.style = "style.1";
        d.put("story.1", st);
    }
    // ... and without s026's indents (body first lines, hanging sources).
    for (const char* id : {"style.1", "style.4"}) {
        auto st = *d.as<composix::core::Style>(id);
        st.first_indent = st.left_indent = 0;
        d.put(id, st);
    }
}

void check(const std::string& name, bool ok, const std::string& detail) {
    if (ok) { ++g_pass; std::cout << "  PASS  " << name; }
    else    { ++g_fail; std::cout << "  FAIL  " << name; }
    if (!detail.empty()) std::cout << "  (" << detail << ")";
    std::cout << "\n";
}

// The FORWARD mapping, mirroring what a viewer's selection() would do: a
// (al,ac)-(cl,cc) mark -> V byte endpoints -> codepoint range + quote. Returns
// the byte endpoints too, so the round-trip through map_range can be exact.
Fwd forward(const std::vector<core::FlatLine>& flat, int al, int ac, int cl, int cc) {
    std::vector<int> off;
    std::string V = core::visible_text(flat, off);
    int bstart = off[static_cast<std::size_t>(al)] +
                 std::min(ac, static_cast<int>(flat[static_cast<std::size_t>(al)].text.size()));
    int bend   = off[static_cast<std::size_t>(cl)] +
                 std::min(cc, static_cast<int>(flat[static_cast<std::size_t>(cl)].text.size()));
    std::string quote = V.substr(static_cast<std::size_t>(bstart),
                                 static_cast<std::size_t>(bend - bstart));
    int cps = core::utf8_length(V.substr(0, static_cast<std::size_t>(bstart)));
    int cpe = cps + core::utf8_length(quote);
    return {cps, cpe, bstart, bend, quote, V};
}

// Assert map_range inverts forward(): endpoints mapped back through byte_off
// reproduce the forward byte endpoints and reconstruct the same quote.
void roundtrip(const std::vector<core::FlatLine>& flat,
               int al, int ac, int cl, int cc, const std::string& what) {
    Fwd f = forward(flat, al, ac, cl, cc);
    std::vector<int> off;
    (void)core::visible_text(flat, off);
    core::LineCol s, e;
    const bool ok = core::map_range(flat, f.cp_start, f.cp_end, s, e);
    check("textmap: " + what + " maps", ok);
    if (!ok) return;
    const int rb0 = off[static_cast<std::size_t>(s.line)] + s.col;
    const int rb1 = off[static_cast<std::size_t>(e.line)] + e.col;
    check("textmap: " + what + " round-trips byte-exact",
          rb0 == f.bstart && rb1 == f.bend,
          std::to_string(rb0) + "," + std::to_string(rb1) +
          " vs " + std::to_string(f.bstart) + "," + std::to_string(f.bend));
    check("textmap: " + what + " reconstructs quote",
          f.V.substr(static_cast<std::size_t>(rb0),
                     static_cast<std::size_t>(rb1 - rb0)) == f.quote);
}
}  // namespace cxtest

int main() {
    std::cout << "composix_selftest -- recents pump + textmap mapping\n";
    part1();
    part2();
    part3();
    part4();
    part5();
    part6();
    part7();
    part8();
    std::cout << "-----------------------------------------------\n";
    std::cout << passes() << " pass / " << failures() << " fail\n";
    return failures() == 0 ? 0 : 1;
}
