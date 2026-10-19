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

#include "core/Doc.hpp"
#include "layout/ThumbStore.hpp"
#include "layout/Flow.hpp"
#include "layout/Preflight.hpp"
#include "layout/ColorReport.hpp"
#include <cstring>
#include <plate/plate.hpp>
#include "layout/Export.hpp"
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

namespace {
int g_pass = 0, g_fail = 0;
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

void check(const std::string& name, bool ok, const std::string& detail = "") {
    if (ok) { ++g_pass; std::cout << "  PASS  " << name; }
    else    { ++g_fail; std::cout << "  FAIL  " << name; }
    if (!detail.empty()) std::cout << "  (" << detail << ")";
    std::cout << "\n";
}

// The FORWARD mapping, mirroring what a viewer's selection() would do: a
// (al,ac)-(cl,cc) mark -> V byte endpoints -> codepoint range + quote. Returns
// the byte endpoints too, so the round-trip through map_range can be exact.
struct Fwd { int cp_start, cp_end, bstart, bend; std::string quote, V; };
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
}  // namespace

int main() {
    std::cout << "composix_selftest -- recents pump + textmap mapping\n";

    // ── Undo -- the one door, two pictures, gestures, merging (s046) ──────────
    {
        std::cout << "\n-- undo --\n";
        using core::Stones;
        using core::Stone;
        using Clock = core::StoneJournal::Clock;
        using SnapMap = std::vector<std::pair<std::string, Stone>>;
        const auto snap = [](const Stones& s) {
            SnapMap out;
            for (const auto& id : s.ids()) out.emplace_back(id, *s.get(id));
            return out;
        };
        const auto names = [](const Stones& s) {
            std::string out;
            for (const auto& id : s.ids()) out += s.get(id)->name + ",";
            return out;
        };
        // A fixture: three stones, LOADED (not a user choice -- not undoable).
        struct Fix {
            Stones s;
            core::StoneJournal j{s};
            std::vector<std::string> sel;
            Fix() {
                s.set_door([this] { return j.open(); });
                j.set_selection_source([this] { return sel; });
                Stones::Loading load(s);
                for (const char* n : {"Ruby", "Jade", "Onyx"}) {
                    const auto id = s.fresh_id();
                    s.put(id, Stone{n, 3, s.pos_after({})});
                }
            }
        };

        // THE GATE. Every verb the page offers, from a fresh fixture: nothing
        // written with the door shut; at least one step; undoing them all puts
        // the model back exactly; redoing them all reproduces the result.
        for (const auto& v : core::stones::verbs()) {
            Fix f;
            const auto first = f.s.ids().front();
            const auto before = snap(f.s);
            v.run(f.j, f.s, first);
            const auto after = snap(f.s);
            const auto steps = f.j.size();
            check("gate/" + v.name + ": through the one door", f.s.strays() == 0,
                  std::to_string(f.s.strays()) + " stray write(s)");
            check("gate/" + v.name + ": is a step", steps > 0 && !(after == before));
            for (std::size_t i = 0; i < steps; ++i) f.j.undo();
            check("gate/" + v.name + ": undo puts it back exactly", snap(f.s) == before);
            for (std::size_t i = 0; i < steps; ++i) f.j.redo();
            check("gate/" + v.name + ": redo reproduces it", snap(f.s) == after);
        }
        {   // ...and the gate bites: a write around the door is counted.
            Fix f;
            const auto id = f.s.ids().front();
            f.s.put(id, Stone{"sneaky", 3, 0.5});
            check("gate: a write around the door is a stray", f.s.strays() == 1);
        }

        {   // Delete comes back whole, in its place, under its id, selected.
            Fix f;
            const auto ids = f.s.ids();
            f.sel = {ids[1]};
            core::stones::remove(f.j, f.s, ids[1]);
            f.sel = {ids[2]};      // the UI selects the neighbour...
            f.j.reselect(f.sel);   // ...and tells the step
            check("delete: gone", names(f.s) == "Ruby,Onyx,", names(f.s));
            check("delete: the label names it", f.j.undo_label() == "Delete “Jade”", f.j.undo_label());
            const auto r = f.j.undo();
            check("delete: undo -- back in its place", names(f.s) == "Ruby,Jade,Onyx,", names(f.s));
            check("delete: undo -- the same id", f.s.ids()[1] == ids[1]);
            check("delete: undo hands back its selection", r.selection == std::vector<std::string>{ids[1]});
            const auto r2 = f.j.redo();
            check("delete: redo -- gone again, the neighbour selected",
                  names(f.s) == "Ruby,Onyx," && r2.selection == std::vector<std::string>{ids[2]});
        }

        {   // Created records: undo removes, redo brings back the same id.
            Fix f;
            const auto first = f.s.ids()[0];
            const auto made = core::stones::add(f.j, f.s, "Opal", first);
            check("add: just after the one given", names(f.s) == "Ruby,Opal,Jade,Onyx,", names(f.s));
            f.j.undo();
            check("add: undone", !f.s.get(made));
            f.j.redo();
            check("add: redone, same id, same place", f.s.get(made) && names(f.s) == "Ruby,Opal,Jade,Onyx,");
        }

        {   // A no-op is not a step.
            Fix f;
            const auto id = f.s.ids()[0];
            core::stones::rename(f.j, f.s, id, "Ruby");
            core::stones::remove(f.j, f.s, "nope");
            core::stones::move_up(f.j, f.s, id);   // already first
            check("no-op: nothing kept", f.j.size() == 0 && !f.j.can_undo());
        }

        {   // Merging: arrows on the slider fold into one step inside the window.
            Fix f;
            const auto id = f.s.ids()[0];
            const auto t0 = Clock::now();
            core::stones::resize(f.j, f.s, id, 4);
            f.j.run("Size", {id}, [&] { auto v = *f.s.get(id); v.size = 5; f.s.put(id, v); }, "size:" + id,
                    t0 + std::chrono::milliseconds(400));
            f.j.run("Size", {id}, [&] { auto v = *f.s.get(id); v.size = 6; f.s.put(id, v); }, "size:" + id,
                    t0 + std::chrono::milliseconds(800));
            check("merge: three presses, one step", f.j.size() == 1, std::to_string(f.j.size()));
            f.j.run("Size", {id}, [&] { auto v = *f.s.get(id); v.size = 7; f.s.put(id, v); }, "size:" + id,
                    t0 + std::chrono::seconds(10));
            check("merge: after the window, a new step", f.j.size() == 2);
            f.j.undo();
            f.j.undo();
            check("merge: two undos, back to the start", f.s.get(id)->size == 3);
            // Another stone's key never merges into this one's step.
            Fix g;
            const auto a = g.s.ids()[0], b = g.s.ids()[1];
            core::stones::resize(g.j, g.s, a, 5);
            core::stones::resize(g.j, g.s, b, 5);
            check("merge: a different key is a different step", g.j.size() == 2);
            // Nor into a step that was undone.
            core::stones::resize(g.j, g.s, b, 6);
            g.j.undo();
            core::stones::resize(g.j, g.s, b, 7);
            check("merge: never into an undone step", g.j.size() == 2 && g.j.can_undo() && !g.j.can_redo());
            // Back to where it started inside the window: the step goes away.
            Fix h;
            const auto c = h.s.ids()[0];
            core::stones::resize(h.j, h.s, c, 4);
            core::stones::resize(h.j, h.s, c, 3);
            check("merge: there and back again is no step", h.j.size() == 0);
        }

        {   // A gesture is one step, however many writes the drag made.
            Fix f;
            const auto id = f.s.ids()[0];
            core::stones::drag_begin(f.j, f.s, id);
            for (int n = 4; n <= 9; ++n) core::stones::drag_to(f.j, f.s, id, n);
            check("gesture: open while dragging", f.j.in_gesture() && f.j.size() == 0);
            core::stones::drag_end(f.j);
            check("gesture: a six-write drag is one step", f.j.size() == 1 && f.s.get(id)->size == 9);
            f.j.undo();
            check("gesture: one undo, back to the start", f.s.get(id)->size == 3);
            // Escape mid-drag: back, and nothing kept.
            core::stones::drag_begin(f.j, f.s, id);
            core::stones::drag_to(f.j, f.s, id, 7);
            f.j.cancel();
            check("gesture: cancel puts it back and keeps nothing",
                  f.s.get(id)->size == 3 && !f.j.in_gesture() && f.j.can_redo());
            // Ctrl+Z mid-drag: the drag becomes a step, then is undone.
            core::stones::drag_begin(f.j, f.s, id);
            core::stones::drag_to(f.j, f.s, id, 2);
            f.j.undo();
            check("gesture: undo mid-drag closes the drag, then undoes it",
                  !f.j.in_gesture() && f.s.get(id)->size == 3 && f.j.can_redo());
            check("gesture: no stray writes", f.s.strays() == 0);
        }

        {   // A new step forgets the redo branch; Move keeps both stones in one step.
            Fix f;
            const auto ids = f.s.ids();
            core::stones::move_down(f.j, f.s, ids[0]);
            check("move: one step for both stones", f.j.size() == 1 && names(f.s) == "Jade,Ruby,Onyx,", names(f.s));
            f.j.undo();
            check("move: undone", names(f.s) == "Ruby,Jade,Onyx,");
            core::stones::rename(f.j, f.s, ids[2], "Coal");
            check("redo branch: a new step forgets it", !f.j.can_redo() && f.j.size() == 1);
        }

        {   // The cap: the oldest step falls off; the newest still undoes.
            Fix f;
            const auto id = f.s.ids()[0];
            for (std::size_t i = 0; i < core::StoneJournal::kMax + 20; ++i)
                core::stones::rename(f.j, f.s, id, "n" + std::to_string(i));
            check("cap: kMax steps kept", f.j.size() == core::StoneJournal::kMax);
            f.j.undo();
            check("cap: the newest still undoes", f.s.get(id)->name == "n" + std::to_string(core::StoneJournal::kMax + 18));
        }

        {   // on_change: the UI hears every journal change (labels, greying).
            Fix f;
            int heard = 0;
            f.j.set_on_change([&] { ++heard; });
            core::stones::rename(f.j, f.s, f.s.ids()[0], "X");
            f.j.undo();
            f.j.redo();
            f.j.clear();
            check("on_change: step, undo, redo, clear -- four", heard == 4, std::to_string(heard));
        }
    }

    // ── s002 / s004: segments, the document model, the projection ────────────
    {
        std::cout << "\n-- segments --\n";
        std::set<int> seen;
        std::set<std::string> keys;
        for (const auto& s : core::segments()) {
            seen.insert(static_cast<int>(s.id));
            keys.insert(s.key);
        }
        check("segments: every segment listed once", seen.size() == core::segments().size() &&
                                                     seen.size() == 9 && keys.size() == 9);
    }
    {
        std::cout << "\n-- document: places, folios, sections --\n";
        core::Doc d;
        core::load_sample(d);
        const auto p = d.pages();
        check("sample: 16 pages, 3 sections", p.size() == 16 && d.ids_of<core::Section>().size() == 3);
        check("folio: front matter is roman", d.folio(p[0]) == "i" && d.folio(p[3]) == "iv");
        check("folio: body restarts at 1", d.folio(p[4]) == "1" && d.folio(p[13]) == "10");
        check("folio: back matter starts at 11", d.folio(p[14]) == "11" && d.folio(p[15]) == "12");
        check("side: first page right, then alternating",
              d.side_of(p[0]) == core::Side::Right && d.side_of(p[1]) == core::Side::Left);
        check("section_of: by start page", d.section_of(p[3]) == "section.1" && d.section_of(p[4]) == "section.2");
        check("format: roman upper / letters",
              core::format_folio(14, core::Numbering::RomanUpper) == "XIV" &&
              core::format_folio(27, core::Numbering::AlphaLower) == "aa");
        check("inches: trims zeros", core::inches(432) == "6" && core::inches(423) == "5.875");
        check("sample: loading is not a stray", d.strays() == 0);
    }
    {
        std::cout << "\n-- document verbs: the gate --\n";
        // Every verb on the sample: through the door (no strays), ONE step,
        // undo restores the document exactly, redo reproduces it exactly.
        for (const auto& v : core::doc::verbs()) {
            core::Doc d;
            core::DocJournal j{d};
            d.set_door([&j] { return j.open(); });
            core::load_sample(d);
            const std::string before = core::to_json(d);
            v.run(j, d);
            const std::string after = core::to_json(d);
            check("verb " + v.name + ": changes the document", after != before);
            check("verb " + v.name + ": one step", j.size() == 1, std::to_string(j.size()));
            check("verb " + v.name + ": through the door", d.strays() == 0, std::to_string(d.strays()));
            j.undo();
            check("verb " + v.name + ": undo restores exactly", core::to_json(d) == before);
            j.redo();
            check("verb " + v.name + ": redo reproduces exactly", core::to_json(d) == after);
        }
        {   // A write that skips the door is caught.
            core::Doc d;
            core::DocJournal j{d};
            d.set_door([&j] { return j.open(); });
            core::load_sample(d);
            auto pg = *d.as<core::Page>("page.2");
            pg.master = "";
            d.put("page.2", pg);
            check("gate: a write outside the door is a stray", d.strays() == 1);
        }
    }
    {
        std::cout << "\n-- document verbs: behaviour --\n";
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        auto p = d.pages();

        const auto made = core::doc::add_pages(j, d, p[5], 2, "master.2");
        auto q = d.pages();
        check("add: two pages after the anchor", made.size() == 2 && q[6] == made[0] && q[7] == made[1]);
        check("add: fresh ids", made[0] != made[1] && !d.as<core::Page>("page.17") == false);

        core::doc::remove_pages(j, d, {p[4]});   // the body's first page: the section moves on
        check("remove: section start moves to the next page",
              d.as<core::Section>("section.2")->start_page == p[5]);
        check("remove: its frames go with it", !d.as<core::Frame>("frame.1") && !d.as<core::Frame>("frame.4"));
        j.undo();
        check("remove + undo: page back in place", d.pages()[4] == p[4] &&
                                                    d.as<core::Section>("section.2")->start_page == p[4]);
        check("remove + undo: frames back", d.as<core::Frame>("frame.1") != nullptr);

        check("remove: never the last page", [&] {
            core::Doc e;
            core::DocJournal je{e};
            core::load_sample(e);
            return !core::doc::remove_pages(je, e, e.pages());
        }());

        core::doc::remove_pages(j, d, {p[14], p[15]});   // the back matter's whole run
        check("remove: a section whose run is gone goes too", !d.as<core::Section>("section.3"));
        j.undo();

        core::doc::move_pages(j, d, {p[1], p[2]}, p[6]);
        q = d.pages();
        const auto at = [&](const std::string& id) { return std::find(q.begin(), q.end(), id) - q.begin(); };
        check("move: pages land after the anchor, in order",
              at(p[6]) + 1 == at(p[1]) && at(p[1]) + 1 == at(p[2]));
        core::doc::move_pages(j, d, {p[9]}, "");
        check("move: to the front", d.pages().front() == p[9]);
        check("move: onto itself refused", !core::doc::move_pages(j, d, {p[3]}, p[3]));

        const auto n0 = j.size();
        core::doc::set_master(j, d, {p[8], p[9], p[10]}, "master.3");
        check("set_master: one step for three pages", j.size() == n0 + 1);
        check("set_master: applied", d.as<core::Page>(p[9])->master == "master.3");
        check("set_master: unknown master refused", !core::doc::set_master(j, d, {p[8]}, "master.99"));
        check("set_master: same again is no step", !core::doc::set_master(j, d, {p[8]}, "master.3"));

        const auto fold = core::doc::size_presets(d.settings())[2];
        core::doc::set_page_size(j, d, {p[12]}, fold);
        check("size: 3-panel foldout = two folds, two tucks",
              d.as<core::Page>(p[12])->folds.size() == 2 &&
              core::inches(d.as<core::Page>(p[12])->w) == "17.75");

        const auto sec = core::doc::add_section(j, d, p[12], "Appendix", core::Numbering::AlphaUpper, 1);
        check("add_section: starts a new run", !sec.empty() && d.folio(p[12]) == "A");
        check("add_section: one per start page",
              core::doc::add_section(j, d, p[12], "Again", core::Numbering::Arabic, 1).empty());
        core::doc::set_numbering(j, d, {sec}, core::Numbering::RomanUpper);
        check("set_numbering: folios follow", d.folio(p[12]) == "I");
        core::doc::remove_section(j, d, sec);
        check("remove_section: pages rejoin the run before", d.section_of(p[12]) == "section.2");
        check("everything went through the door", d.strays() == 0, std::to_string(d.strays()));

        check("neighbour: next survivor, else previous",
              core::doc::neighbour_page(d, {d.pages()[2]}) == d.pages()[3] &&
              core::doc::neighbour_page(d, {d.pages().back()}) == d.pages()[d.pages().size() - 2]);
    }
    {
        std::cout << "\n-- document.json + the project folder --\n";
        core::Doc d;
        core::load_sample(d);
        const std::string text = core::to_json(d);
        core::Doc e;
        std::string err;
        check("json: parses back", core::from_json(e, text, &err), err);
        check("json: round-trips exactly", core::to_json(e) == text);
        check("json: same records", e.records() == d.records());

        const auto dir = (std::filesystem::temp_directory_path() / "composix_selftest_project").string();
        std::filesystem::remove_all(dir);
        check("project: saves", core::save_project(d, dir, &err), err);
        check("project: has document.json + assets/",
              std::filesystem::exists(std::filesystem::path(dir) / "document.json") &&
              std::filesystem::is_directory(std::filesystem::path(dir) / "assets"));
        core::Doc f;
        check("project: loads", core::load_project(f, dir, &err), err);
        check("project: what was saved is what loads", f.records() == d.records());
        std::filesystem::remove_all(dir);

        core::Doc g;
        check("json: garbage refused", !core::from_json(g, "{nope", &err));
        check("json: newer format refused", !core::from_json(g, R"({"composix": 99, "records": []})", &err));
        check("json: unknown kind skipped",
              core::from_json(g, R"({"composix":1,"records":[{"id":"x.1","kind":"hologram"},
                                    {"id":"page.1","kind":"page","pos":1}]})", &err) &&
              g.size() == 1 && g.as<core::Page>("page.1"));
    }
    {
        std::cout << "\n-- projection (what the views read) --\n";
        core::Doc d;
        core::load_sample(d);
        const auto o = core::project(d, "");
        bool all_full = true;
        for (const auto& s : core::segments()) all_full = all_full && !o.in(s.id).empty();
        check("project: every segment has items", all_full);
        std::set<std::string> ids;
        for (const auto& it : o.items()) ids.insert(it.id);
        check("project: item ids unique, = record ids", ids.size() == o.size() && o.find("page.11"));

        const auto field = [](const core::Item* it, const std::string& l) {
            for (const auto& f : it->fields) if (f.label == l) return f.value;
            return std::string("<absent>");
        };
        const auto* fo = o.find("page.11");
        check("project: the foldout page", fo->badge == "Foldout" && fo->thumb.panels == 3 &&
                                           fo->title == "Page 7" && field(fo, "Size") == "17.75 × 9 in (3-panel)");
        check("project: front matter page", field(o.find("page.3"), "Folio") == "iii" &&
                                            field(o.find("page.3"), "Section") == "Front matter");
        check("project: section page range", field(o.find("section.1"), "Pages") == "i–iv" &&
                                             field(o.find("section.2"), "Pages") == "1–10");
        check("project: master used-by is counted", field(o.find("master.2"), "Used by") == "8 pages");
        check("project: story words + threading",
              field(o.find("story.1"), "Frames") == "3, threaded" && field(o.find("story.3"), "Words") == "7");
        check("project: missing link flagged", o.find("asset.2")->badge == "Missing" &&
                                               field(o.find("asset.2"), "Status") == "Missing");
        check("project: effective ppi from the placed width",
              field(o.find("asset.1"), "Effective ppi") == "423");
        check("project: vector asset", field(o.find("asset.3"), "Effective ppi") == "Vector" &&
                                       field(o.find("asset.3"), "Placed") == "Pages ii, 12");
        check("project: stale contents list", o.find("list.1")->badge == "Stale" &&
                                              field(o.find("list.1"), "Entries") == "2");
        check("project: current bibliography", field(o.find("list.2"), "Built") == "Current");
        check("project: style specimen weight", o.find("style.2")->thumb.bold && o.find("style.3")->thumb.italic);

        // Multi-selection: shared fields, Mixed, editability.
        const auto two = core::shared_fields({o.find("page.6"), o.find("page.8")});
        auto get = [&](const std::vector<core::Field>& fs, const std::string& l) -> const core::Field* {
            for (const auto& f : fs) if (f.label == l) return &f;
            return nullptr;
        };
        check("shared: agreeing field keeps its value", get(two, "Master") && get(two, "Master")->value == "B-Body");
        check("shared: differing field reads Mixed", get(two, "Folio")->value == core::kMixed);
        check("shared: editable stays editable", get(two, "Master")->edit == "master" &&
                                                 !get(two, "Master")->choices.empty());
        check("shared: read-only stays read-only", get(two, "Folio")->edit.empty());
        const auto cross = core::shared_fields({o.find("page.6"), o.find("master.2")});
        check("shared: across segments only common labels survive",
              cross.empty());   // s148: a master's "Size (in)" is its own row now -- nothing in common with a page's
        check("shared: empty selection -> nothing", core::shared_fields({}).empty());

        check("title: one item", core::selection_title({fo}) == "Page 7");
        check("title: several of a kind",
              core::selection_title({o.find("page.6"), o.find("page.8"), fo}) == "3 pages");
        check("title: mixed kinds", core::selection_title({o.find("page.6"), o.find("swatch.4")}) == "2 items");
        check("title: none", core::selection_title({}) == "Nothing selected");

        // A verb shows up in the projection (the views' only view of the model).
        core::DocJournal j{d};
        core::doc::set_master(j, d, {"page.6"}, "master.3");
        check("project: follows the model", field(core::project(d, "").find("page.6"), "Master") == "C-Chapter");
    }

    // ── s005: the canvas's arithmetic, layout, picking, frame verbs ──────────
    {
        std::cout << "\n-- geometry: viewport, handles, resize --\n";
        using core::Rect;
        core::Viewport v;
        v.set_widget(1000, 800);
        v.set_content({0, 0, 432, 648});
        v.fit();
        const Rect s = v.to_screen(v.content());
        check("fit: centred", std::abs(s.cx() - 500) < 0.01 && std::abs(s.cy() - 400) < 0.01);
        check("fit: 90% of the tighter axis", std::abs(s.h - 720) < 0.01, std::to_string(s.h));
        double sx, sy, dx, dy;
        v.to_screen(100, 200, sx, sy);
        v.to_doc(sx, sy, dx, dy);
        check("to_screen / to_doc invert", std::abs(dx - 100) < 1e-9 && std::abs(dy - 200) < 1e-9);
        v.to_doc(300, 250, dx, dy);
        v.zoom_toward(300, 250, 2.0);
        double ex, ey;
        v.to_doc(300, 250, ex, ey);
        check("zoom_toward: the point under the cursor stays put", std::abs(ex - dx) < 1e-9 && std::abs(ey - dy) < 1e-9);
        v.set_percent(100);
        check("100% = 96/72 px per pt", std::abs(v.scale() - 96.0 / 72.0) < 1e-12 && std::abs(v.percent() - 100) < 1e-9);
        v.pan_by(-100000, -100000);
        const Rect s2 = v.to_screen(v.content());
        check("clamp: content never leaves the screen", s2.r() >= 47.9 && s2.b() >= 47.9);
        v.zoom_toward(500, 400, 1e9);
        check("zoom is bounded", v.scale() == core::Viewport::kMax);

        const Rect r{100, 100, 200, 100};
        check("handles: corners win over edges",
              core::handle_hit(r, 103, 98) == core::Handle::NW && core::handle_hit(r, 200, 100) == core::Handle::N);
        check("handles: miss", core::handle_hit(r, 150, 150) == core::Handle::None);
        check("resize SE grows", core::resize_rect(r, core::Handle::SE, 10, 20, false, false) == Rect{100, 100, 210, 120});
        check("resize NW anchors the SE corner",
              core::resize_rect(r, core::Handle::NW, 10, 10, false, false) == Rect{110, 110, 190, 90});
        check("resize N leaves width", core::resize_rect(r, core::Handle::N, 50, -10, false, false) == Rect{100, 90, 200, 110});
        const Rect sh = core::resize_rect(r, core::Handle::SE, 100, 0, true, false);
        check("resize Shift keeps proportions", std::abs(sh.w / sh.h - 2.0) < 1e-9 && sh.x == 100 && sh.y == 100);
        const Rect al = core::resize_rect(r, core::Handle::E, 10, 0, false, true);
        check("resize Alt is about the centre", al == Rect{90, 100, 220, 100});
        check("resize never below the minimum",
              core::resize_rect(r, core::Handle::E, -500, 0, false, false).w == 1);
        check("map_rect keeps a member's place in the box",
              core::map_rect({150, 125, 50, 25}, r, {0, 0, 400, 200}) == Rect{100, 50, 100, 50});
    }
    {
        std::cout << "\n-- layout + picking --\n";
        core::Doc d;
        core::load_sample(d);
        const auto pl = core::layout_pages(d, {"page.5", "page.11", "page.6"});
        check("layout: left to right, gap between",
              pl.size() == 3 && pl[1].r.x == 432 + 36 && pl[2].r.x == pl[1].r.r() + 36 && pl[0].r.y == 0);
        check("layout: a master is flagged", core::layout_pages(d, {"master.3"}).front().master);
        check("master chain: based-on first",
              core::master_chain(d, "page.5") == std::vector<std::string>({"master.2", "master.3"}));
        const auto fr = core::frame_in_layout(d, pl, "frame.5");
        check("frame in layout: owner's offset applied", fr.x == pl[1].r.x + 18 && fr.y == 36);
        check("hit: a frame", core::hit_frame(d, pl, fr.cx(), fr.cy(), 2) == "frame.5");
        check("hit: nothing on bare paper", core::hit_frame(d, pl, pl[2].r.x + 5, pl[2].r.y + 5, 2).empty());
        {   // overlapping frames: the higher z wins
            core::Doc e;
            core::load_sample(e);
            core::Doc::Loading load(e);
            e.put("frame.90", core::Frame{"page.5", core::FrameKind::Shape, 54, 54, 100, 100, 0, 9});
            const auto pe = core::layout_pages(e, {"page.5"});
            check("hit: topmost z first", core::hit_frame(e, pe, 60, 60, 1) == "frame.90");
        }
        const auto box = core::frames_in(d, pl, {0, 0, 432, 200});
        check("marquee: touching selects",
              std::find(box.begin(), box.end(), "frame.1") != box.end() &&
              std::find(box.begin(), box.end(), "frame.4") != box.end());
        check("thread: middle of three", core::thread_place(d, "frame.2") == std::make_pair(2, 3));
        check("thread: not text", core::thread_place(d, "frame.4") == std::make_pair(0, 0));

        {   // s154: X / Y read from each page's own corner here (Page origin)
            core::Doc::Loading load(d);
            auto st = d.settings();
            st.ruler_origin = core::RulerOrigin::Page;
            d.put("doc", st);
        }
        const auto items = core::frame_items(d, {"frame.2", "frame.3"});
        const auto shared = core::shared_fields({&items[0], &items[1]});
        const auto val = [&](const std::string& l) {
            for (const auto& f : shared) if (f.label == l) return f.value;
            return std::string("<absent>");
        };
        check("frame items: shared story, mixed place",
              val("Story") == "Chapters" && val("Thread") == core::kMixed &&
              val("Bounds (in)") == "0.75\t0.75\t4.5\t7.5", val("Bounds (in)"));
        const auto mixed = core::frame_items(d, {"frame.1", "frame.2"});
        const auto sm = core::shared_fields({&mixed[0], &mixed[1]});
        std::string b;
        for (const auto& f : sm) if (f.label == "Bounds (in)") b = f.value;
        check("frame items: bounds Mixed per column", b == "0.75\tMixed\t4.5\tMixed", b);
        check("frame items: titles", core::frames_title(items) == "2 frames" &&
                                     items[0].title == "Text frame — Chapters");
    }
    {
        std::cout << "\n-- frame verbs --\n";
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto f0 = *d.as<core::Frame>("frame.5");

        core::doc::frames_begin(j, d, {"frame.5"}, "Move frame");
        for (int i = 1; i <= 5; ++i) core::doc::frames_set(j, d, "frame.5", {f0.x + i, f0.y, f0.w, f0.h});
        core::doc::frames_cancel(j);
        check("drag + Escape: back where it was, no step",
              *d.as<core::Frame>("frame.5") == f0 && j.size() == 0);

        core::doc::frames_begin(j, d, {"frame.5"}, "Move frame");
        check("a press without a move is no step", !core::doc::frames_end(j) && j.size() == 0);

        const auto t0 = std::chrono::steady_clock::now();
        (void)t0;
        core::doc::move_frames(j, d, {"frame.5"}, 1, 0, "nudge");
        core::doc::move_frames(j, d, {"frame.5"}, 1, 0, "nudge");
        core::doc::move_frames(j, d, {"frame.5"}, 0, 1, "nudge");
        check("nudges merge into one step", j.size() == 1 &&
                                            d.as<core::Frame>("frame.5")->x == f0.x + 2 &&
                                            d.as<core::Frame>("frame.5")->y == f0.y + 1);
        j.undo();
        check("one undo takes all the nudges back", *d.as<core::Frame>("frame.5") == f0);
        check("move: nothing to move is no step", !core::doc::move_frames(j, d, {"page.1"}, 5, 5));
        check("frames: through the door", d.strays() == 0);
    }

    // ── s006: guides + snapping ──────────────────────────────────────────────
    {
        std::cout << "\n-- guides + snapping --\n";
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto pl = core::layout_pages(d, {"page.6", "page.7"});
        const auto t = core::snap_targets(d, pl, {"frame.2"});
        const auto has = [](const std::vector<double>& v, double x) {
            return std::any_of(v.begin(), v.end(), [&](double y) { return std::abs(y - x) < 1e-9; });
        };
        check("targets: page edges + centre", has(t.xs, 0) && has(t.xs, 216) && has(t.xs, 432));
        check("targets: the master's margin guides show on the page", has(t.xs, 54) && has(t.ys, 594));
        check("targets: the second page's guides are offset", has(t.xs, pl[1].r.x + 54));
        {   // a frame's edges are targets -- unless it is the one being dragged
            core::Doc e;
            core::load_sample(e);
            core::Doc::Loading load(e);
            e.put("frame.91", core::Frame{"page.6", core::FrameKind::Shape, 111, 222, 33, 44, 0, 5});
            check("targets: other frames' edges", has(core::snap_targets(e, pl, {}).xs, 111));
            check("targets: not the dragged frame's", !has(core::snap_targets(e, pl, {"frame.91"}).xs, 111));
        }
        const auto h = core::snap_any({50, 150}, t.xs, 5);
        check("snap: nearest within tolerance", h.hit() && h.line == 54 && std::abs(h.delta - 4) < 1e-9);
        check("snap: nothing near is no snap", !core::snap_any({30}, {0, 54}, 5).hit());
        check("snap: the closer of two wins", core::snap_any({52, 100}, {54, 101}, 5).line == 101);

        check("page_at: over a page / the pasteboard",
              core::page_at(pl, 10, 10) == "page.6" && core::page_at(pl, 10, 700).empty());
        check("spread: a left page and its right page meet at the spine (s027)",
              pl[1].r.x == pl[0].r.w && core::page_at(pl, 440, 10) == "page.7");
        const auto g = core::doc::add_guide(j, d, "page.6", false, 300);
        const auto pl2 = core::layout_pages(d, {"page.6"});
        check("add_guide: one step, on its page", !g.empty() && j.size() == 1 && core::guides_on(d, "page.6").size() == 1);
        check("hit_guide: the page's own guide", core::hit_guide(d, pl2, 100, 302, 3) == g);
        check("hit_guide: a master's guide isn't picked on the page", core::hit_guide(d, pl2, 54, 100, 3).empty());
        core::doc::guide_begin(j, d, g);
        core::doc::guide_set(j, d, g, 320);
        core::doc::guide_set(j, d, g, -50);
        core::doc::guide_remove_in_gesture(j, d, g);
        core::doc::frames_end(j);
        check("drag a guide off its page: gone, one step", !d.as<core::Guide>(g) && j.size() == 2);
        check("...and the step says so", j.undo_label() == "Remove guide");
        j.undo();
        check("undo brings it back where it started", d.as<core::Guide>(g) && d.as<core::Guide>(g)->pos == 300);
        check("guides: through the door", d.strays() == 0);
        check("add_guide: not on the pasteboard", core::doc::add_guide(j, d, "", true, 5).empty());
        {   // s153: a dragged guide snaps to everything but itself
            const auto pl3 = core::layout_pages(d, {"page.6"});
            check("s153 targets: a guide is a target", has(core::snap_targets(d, pl3, {}).ys, 300));
            check("s153 targets: not the dragged guide", !has(core::snap_targets(d, pl3, {g}).ys, 300));
            const auto g2 = core::doc::add_guide(j, d, "page.6", true, 100);
            const auto t3 = core::snap_targets(d, pl3, {g2});
            const auto h3 = core::snap_any({57}, t3.xs, 6);
            check("s153 snap: a dragged guide meets the margin", h3.hit() && std::abs(h3.line - 54) < 1e-9);
            const auto h4 = core::snap_any({303}, t3.ys, 6);
            check("s153 snap: ... and another guide", h4.hit() && std::abs(h4.line - 300) < 1e-9);
        }
    }

    // ── s007: text flow ──────────────────────────────────────────────────────
    {
        std::cout << "\n-- text flow --\n";
        namespace L = composix::layout;
        core::Doc d;
        load_plain(d);
        check("thread order", L::thread_frames(d, "story.1") ==
                              std::vector<std::string>({"frame.1", "frame.2", "frame.3"}));
        const auto f = L::flow_story(d, "story.1");
        check("sample chapters: set, not overset", f.lines > 0 && !f.overset, std::to_string(f.lines));
        bool steady = true;
        for (const auto& ft : f.frames)
            for (std::size_t i = 1; i < ft.lines.size(); ++i)
                steady = steady && ft.lines[i].y > ft.lines[i - 1].y;
        check("lines go down the frame", steady);

        // Every byte of every paragraph is set exactly once, in order -- across
        // frames of different widths too.
        const auto covers = [&](const core::Doc& doc, const L::StoryFlow& fl, const std::string& story) {
            const auto& paras = doc.as<core::Story>(story)->paras;
            std::vector<int> at(paras.size(), 0);
            for (const auto& ft : fl.frames)
                for (const auto& ln : ft.lines) {
                    if (ln.start != at[static_cast<std::size_t>(ln.para)]) return false;
                    at[static_cast<std::size_t>(ln.para)] = ln.end;
                }
            for (std::size_t i = 0; i < paras.size(); ++i)
                if (at[i] != static_cast<int>(paras[i].text.size())) return false;
            return true;
        };
        check("every byte set once, in order", covers(d, f, "story.1"));

        {   // A short first frame: the text continues in the next, re-broken at its width.
            core::Doc e;
            load_plain(e);
            core::Doc::Loading load(e);
            auto f1 = *e.as<core::Frame>("frame.1");
            f1.h = 62;
            e.put("frame.1", f1);
            auto f2 = *e.as<core::Frame>("frame.2");
            f2.w = 150;   // much narrower
            e.put("frame.2", f2);
            const auto g = L::flow_story(e, "story.1");
            check("flows on into the next frame", !g.frames[1].lines.empty() && !g.overset);
            // 62 pt holds the 28 pt heading line, its 6 pt space after (s011)
            // and two 14 pt body lines (62); a third would end at 76.
            check("frame 1 holds exactly what fits (28 + 6 + 14 + 14)", g.frames[0].lines.size() == 3 &&
                                                                    g.frames[0].lines[2].y == 48,
                  std::to_string(g.frames[0].lines.size()));
            check("still every byte once, across widths", covers(e, g, "story.1"));
            check("narrower frame: more lines", g.lines > f.lines, std::to_string(g.lines) + " vs " + std::to_string(f.lines));
        }
        {   // Too little room anywhere: overset.
            core::Doc e;
            load_plain(e);
            core::Doc::Loading load(e);
            for (const char* id : {"frame.1", "frame.2", "frame.3"}) {
                auto fr = *e.as<core::Frame>(id);
                fr.h = 30;
                e.put(id, fr);
            }
            check("overset when the thread runs out", L::flow_story(e, "story.1").overset);
        }
        {   // A looping thread does not hang.
            core::Doc e;
            load_plain(e);
            core::Doc::Loading load(e);
            auto f3 = *e.as<core::Frame>("frame.3");
            f3.next = "frame.1";
            e.put("frame.3", f3);
            check("a looping thread stops", L::thread_frames(e, "story.1").size() <= 3);
        }
        check("the caption fits its frame", !L::flow_story(d, "story.2").overset);
        check("an unknown story flows nothing", L::flow_story(d, "story.99").lines == 0);

        L::FlowCache cache;
        const int n1 = cache.get(d, "story.1").lines;
        core::DocJournal j{d};
        auto fr = *d.as<core::Frame>("frame.1");
        core::doc::frames_set(j, d, "frame.1", {fr.x, fr.y, 120, fr.h});
        check("cache: recomputed after a change", cache.get(d, "story.1").lines > n1);

        std::vector<std::string> over{"story.2"};
        const auto o = core::project(d, "", &over);
        check("projection: overset badge + field", o.find("story.2")->badge == "Overset");
    }

    // ── s008: frame tools ────────────────────────────────────────────────────
    {
        std::cout << "\n-- frame tools --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);

        const auto t = core::doc::add_frame(j, d, "page.8", core::FrameKind::Text, {54, 54, 200, 100});
        const auto* tf = d.as<core::Frame>(t);
        check("text frame: its own story, one empty paragraph",
              tf && !tf->story.empty() && d.as<core::Story>(tf->story)->paras.size() == 1);
        check("text frame: one step (frame + story)", j.size() == 1);
        const std::string made_story = tf ? tf->story : "x";
        j.undo();
        check("undo: frame and story both gone", !d.as<core::Frame>(t) && !d.as<core::Story>(made_story));
        j.redo();
        check("new frame on top", d.as<core::Frame>(t)->z >= 1);
        check("no frame on the pasteboard", core::doc::add_frame(j, d, "", core::FrameKind::Shape, {0, 0, 9, 9}).empty());
        check("no zero-size frame", core::doc::add_frame(j, d, "page.8", core::FrameKind::Shape, {0, 0, 0, 9}).empty());

        core::doc::delete_frames(j, d, {"frame.2"});
        check("delete mid-thread: spliced out", d.as<core::Frame>("frame.1")->next == "frame.3");
        check("...the text still flows", L::thread_frames(d, "story.1") ==
                                         std::vector<std::string>({"frame.1", "frame.3"}));
        j.undo();
        check("undo: back in the thread", d.as<core::Frame>("frame.1")->next == "frame.2" &&
                                          d.as<core::Frame>("frame.2"));

        core::doc::thread_frames(j, d, "frame.3", "frame.7");   // dedication joins the chapters
        check("thread: appended to the end", d.as<core::Frame>("frame.3")->next == "frame.7" &&
                                             d.as<core::Frame>("frame.7")->story == "story.1");
        check("thread: the old story's text moved", d.as<core::Story>("story.1")->paras.back().text ==
                                                    "For everyone who ever finished a manuscript.");
        check("thread: the emptied story goes", !d.as<core::Story>("story.3"));
        check("thread: refused into a threaded frame", !core::doc::thread_frames(j, d, "frame.10", "frame.2"));
        check("thread: refused to itself / non-text",
              !core::doc::thread_frames(j, d, "frame.3", "frame.3") && !core::doc::thread_frames(j, d, "frame.3", "frame.4"));
        j.undo();
        check("undo thread: both stories as they were", d.as<core::Story>("story.3") &&
                                                        d.as<core::Frame>("frame.7")->story == "story.3");

        core::doc::unthread_after(j, d, "frame.1");
        check("unthread: the tail is empty", d.as<core::Frame>("frame.1")->next.empty() &&
                                             d.as<core::Frame>("frame.2")->story.empty() &&
                                             d.as<core::Frame>("frame.3")->story.empty());
        check("unthread: the head keeps the story", L::thread_frames(d, "story.1") == std::vector<std::string>({"frame.1"}));
        core::doc::thread_frames(j, d, "frame.1", "frame.2");
        check("re-thread an emptied frame", d.as<core::Frame>("frame.2")->story == "story.1");
        check("frame tools: through the door", d.strays() == 0);
    }

    // ── s010: text editing ──────────────────────────────────────────────────
    {
        std::cout << "\n-- text editing --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        load_plain(d);
        using P = core::TextPos;
        const auto para = [&](int i) { return d.as<core::Story>("story.1")->paras[static_cast<std::size_t>(i)].text; };
        const auto n0 = d.as<core::Story>("story.1")->paras.size();
        const std::string p1 = para(1);

        // A burst of typing is one step.
        P c{1, 0};
        for (const char* k : {"N", "e", "w", " "}) c = core::doc::edit_text(j, d, "story.1", c, c, k, "Typing", "type:1");
        check("typing: in place, caret after", para(1).rfind("New ", 0) == 0 && c == P{1, 4});
        check("typing: a burst is ONE step", j.size() == 1);
        j.undo();
        check("undo: the burst gone", para(1) == p1);

        c = core::doc::edit_text(j, d, "story.1", {1, 3}, {1, 3}, "\n");
        check("enter: splits the paragraph", d.as<core::Story>("story.1")->paras.size() == n0 + 1 &&
                                             para(1) == p1.substr(0, 3) && para(2) == p1.substr(3) && c == P{2, 0});
        check("enter: the new paragraph keeps the style",
              d.as<core::Story>("story.1")->paras[1].style == d.as<core::Story>("story.1")->paras[2].style);
        c = core::doc::edit_text(j, d, "story.1", core::prev_char(*d.as<core::Story>("story.1"), c), c, "", "Delete text");
        check("backspace at a paragraph's start: joins it back", para(1) == p1 && c == P{1, 3} &&
                                                                 d.as<core::Story>("story.1")->paras.size() == n0);
        c = core::doc::edit_text(j, d, "story.1", {0, 0}, {0, 0}, "a\r\nb\nc", "Paste");
        check("paste: three lines, three paragraphs", para(0) == "a" && para(1) == "b" &&
                                                      c == P{2, 1} && para(2).rfind("c", 0) == 0);
        j.undo();
        check("undo paste", d.as<core::Story>("story.1")->paras.size() == n0);
        check("no change, no step", core::doc::edit_text(j, d, "story.1", {1, 2}, {1, 2}, "") == P{1, 2} &&
                                    j.undo_label() != "Typing");

        core::Story u;
        u.paras = {{"", "caf\xC3\xA9 ok"}, {"", "two words"}};
        check("utf-8: next over a 2-byte char", core::next_char(u, {0, 3}) == P{0, 5});
        check("utf-8: prev back over it", core::prev_char(u, {0, 5}) == P{0, 3});
        check("utf-8: clamp off a continuation byte", core::clamp_pos(u, {0, 4}) == P{0, 3});
        check("char: across the paragraph end", core::next_char(u, {0, 8}) == P{1, 0} && core::prev_char(u, {1, 0}) == P{0, 8});
        check("word: next", core::next_word(u, {0, 0}) == P{0, 6} && core::next_word(u, {0, 6}) == P{1, 0});
        check("word: prev", core::prev_word(u, {1, 4}) == P{1, 0} && core::prev_word(u, {1, 0}) == P{0, 6});
        check("word at (double-click)", core::word_at(u, {1, 5}) == std::make_pair(P{1, 4}, P{1, 9}));
        check("text between: joined with \\n", core::text_between(u, {0, 6}, {1, 3}) == "ok\ntwo");

        // Geometry: the caret, a hit back, up / down, edges, selections. The
        // first frame made short, so the story runs on into the second.
        {
            const auto f1 = *d.as<core::Frame>("frame.1");
            core::doc::frames_begin(j, d, {"frame.1"}, "Resize frame");
            core::doc::frames_set(j, d, "frame.1", {f1.x, f1.y, f1.w, 62});
            core::doc::frames_end(j);
        }
        const auto flow = L::flow_story(d, "story.1");
        const auto ca = L::caret_at(d, "story.1", flow, {1, 10});
        check("caret: found, in the first frame", ca.ok && ca.frame == "frame.1" && ca.h > 0, ca.frame);
        const auto back = L::hit_text(d, "story.1", flow, ca.frame, ca.x + 0.1, ca.y + ca.h / 2);
        check("hit: the caret's own place comes back", back == P{1, 10},
              std::to_string(back.para) + "," + std::to_string(back.byte));
        const auto down = L::line_move(d, "story.1", flow, {1, 10}, +1, ca.x);
        const auto up = L::line_move(d, "story.1", flow, down, -1, ca.x);
        check("down then up: home again", up == P{1, 10} && down != up);
        const auto e = L::line_edge(d, "story.1", flow, {1, 10}, true);
        const auto h = L::line_edge(d, "story.1", flow, {1, 10}, false);
        check("home / end of the line", h.byte <= 10 && e.byte > 10 && h.para == 1 && e.para == 1);
        // The last line of frame.1 goes on into frame.2.
        const auto& last = flow.frames[0].lines.back();
        const auto next = L::line_move(d, "story.1", flow, {last.para, last.start}, +1, 0);
        check("down off a frame's last line: into the next frame",
              L::caret_at(d, "story.1", flow, next).frame == "frame.2");
        check("selection: one rect per line touched",
              L::selection_rects(d, "story.1", flow, "frame.1", {1, 0}, {1, 3}).size() == 1 &&
              L::selection_rects(d, "story.1", flow, "frame.1", {0, 0}, {1, 60}).size() == 3);
        check("text editing: through the door", d.strays() == 0);
    }

    // ── s015: the print PDF through Plate ───────────────────────────────────
    {
        std::cout << "\n-- print pdf (plate) --\n";
        namespace L = composix::layout;
        namespace fs = std::filesystem;
        core::Doc d;
        core::load_sample(d);
        const L::ImageSource src = [](const core::Asset& a, double, double) -> cairo_surface_t* {
            if (a.name == "harbor.psd") return nullptr;
            cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 4);
            cairo_t* c = cairo_create(s);
            cairo_set_source_rgba(c, 0.2, 0.4, 0.6, 0.5);
            cairo_paint(c);
            cairo_destroy(c);
            return s;
        };
        const auto base = fs::temp_directory_path() / "composix_selftest_plate";
        std::error_code ec;
        fs::create_directories(base, ec);
        const auto out = (base / "print.pdf").string();
        L::PdfOptions opt;
        opt.marks = true;
        opt.compress = false;
        L::PdfReport rep;
        std::string err;
        check("print: ok", L::export_print_pdf(d, out, opt, src, &rep, &err), err);
        std::string pdf;
        { std::ifstream f(out, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); pdf = ss.str(); }
        const auto has = [&](const std::string& s) { return pdf.find(s) != std::string::npos; };
        check("print: every page", rep.pages == 16 && has("/Count 16"));
        // Trim 432 x 648; the bleed 9 and the marks' slug 21 around it.
        check("print: a TrimBox at the trim, inside the slug", has("/TrimBox [30 30 462 678]"));
        check("print: a BleedBox the bleed beyond it", has("/BleedBox [21 21 471 687]"));
        check("print: fonts embedded, subset, with ToUnicode",
              (has("/FontFile2") || has("/FontFile3")) && has("/ToUnicode") &&
                  std::regex_search(pdf, std::regex("/BaseFont /[A-Z]{6}\\+")));   // a subset tag, whatever Serif is here
        check("print: body text in 100 K", has("0 0 0 1 k\n"));
        // s016: Guild Rust is a spot colour -- its own plate, PMS 7593 C.
        check("print: the heading on its spot plate", has("[/Separation /PMS#207593#20C /DeviceCMYK") && has("cs 1 scn"));
        check("print: the caption's rule on the same plate (stroke)", has("CS 1 SCN"));
        check("print: crop marks in registration", has("1 1 1 1 K\n"));
        check("print: folio page labels", has("/P (iv)"));
        check("print: one embedding per image file",
              [&] { int n = 0; for (std::size_t at = 0; (at = pdf.find("/Subtype /Image /Width 8", at)) != std::string::npos; ++at) ++n;
                    return n == 6; }());   // guild-map, lighthouse, fleuron (placed twice): 3 images + their 3 soft masks
        bool miss = false;
        for (const auto& w : rep.warnings) miss = miss || w == "Missing image: harbor.psd";
        check("print: the missing image reported", miss);
        // s016: with a press profile (GRACoL, when installed): RGB converted.
        const char* gracol = "/usr/share/color/icc/GRACoL2013_CRPC6.icc";
        if (fs::exists(gracol)) {
            L::PdfOptions co = opt;
            co.profile = gracol;
            co.convert = true;
            L::PdfReport crep;
            check("cmyk: export with a press profile", L::export_print_pdf(d, out, co, src, &crep, &err), err);
            std::string cp;
            { std::ifstream f(out, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); cp = ss.str(); }
            check("cmyk: images converted (DeviceCMYK), no RGB left", cp.find("/ColorSpace /DeviceCMYK") != std::string::npos &&
                                                                        cp.find(" rg\n") == std::string::npos);
            check("cmyk: the report names the profile", !crep.warnings.empty() &&
                                                        crep.warnings.front().rfind("Output profile: ", 0) == 0);
            co.convert = false;
            check("keep: RGB tagged sRGB", L::export_print_pdf(d, out, co, src, &crep, &err));
            { std::ifstream f(out, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); cp = ss.str(); }
            check("keep: ICCBased images", cp.find("[/ICCBased") != std::string::npos);
            co.profile = "/usr/share/color/icc/sRGB.icc";
            check("profile: an RGB profile refused, said why", !L::export_print_pdf(d, out, co, src, &crep, &err) &&
                                                               err.find("CMYK") != std::string::npos, err);
            // s017: PDF/X. X-1a converts and flattens, X-4 keeps ICC RGB.
            co.profile = gracol;
            co.convert = false;
            co.compress = false;
            co.standard = L::plate_standard::X1a;
            L::PdfReport xrep;
            check("pdfx: X-1a exports (convert forced)", L::export_print_pdf(d, out, co, src, &xrep, &err), err);
            { std::ifstream f(out, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); cp = ss.str(); }
            check("pdfx: X-1a is PDF 1.4, intent, keys, no RGB, no SMask",
                  cp.rfind("%PDF-1.4", 0) == 0 && cp.find("/GTS_PDFX ") != std::string::npos &&
                      cp.find("(PDF/X-1a:2003)") != std::string::npos && cp.find(" rg\n") == std::string::npos &&
                      cp.find("/SMask") == std::string::npos && cp.find("/ICCBased") == std::string::npos);
            check("pdfx: the report names the standard", !xrep.warnings.empty() &&
                                                         xrep.warnings.front().rfind("Standard: PDF/X-1a:2003", 0) == 0);
            co.standard = L::plate_standard::X4;
            check("pdfx: X-4 exports", L::export_print_pdf(d, out, co, src, &xrep, &err), err);
            { std::ifstream f(out, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); cp = ss.str(); }
            check("pdfx: X-4 is PDF 1.6 with XMP, ICC RGB images",
                  cp.rfind("%PDF-1.6", 0) == 0 && cp.find("<pdfxid:GTS_PDFXVersion>PDF/X-4") != std::string::npos &&
                      cp.find("[/ICCBased") != std::string::npos);
            co.profile.clear();
            check("pdfx: no profile refused, said why", !L::export_print_pdf(d, out, co, src, &xrep, &err) &&
                                                       err.find("profile") != std::string::npos, err);
        } else {
            std::cout << "  (skip cmyk: no GRACoL profile installed)\n";
        }
        const auto o = core::project(d, "");
        bool col = false;
        for (const auto& f : o.find("style.2")->fields) col = col || (f.edit == "color" && f.value == "Guild Rust");
        check("styles: Color shows the swatch, editable", col);
        if (std::getenv("KEEP_PDF")) std::cout << "kept " << out << "\n"; else fs::remove_all(base, ec);
    }

    // ── s014: margins + master columns ──────────────────────────────────────
    {
        std::cout << "\n-- margins --\n";
        const auto near = [](double a, double b) { return std::abs(a - b) < 0.01; };
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        // Find a right and a left Body page.
        std::string right, left;
        for (const auto& p : d.pages()) {
            if (d.as<core::Page>(p)->master != "master.2") continue;
            if (d.side_of(p) == core::Side::Right && right.empty()) right = p;
            if (d.side_of(p) == core::Side::Left && left.empty()) left = p;
        }
        auto r = core::margin_rect(d, right);
        check("margins: a right page, inside on the left", near(r.x, 54) && near(r.y, 54) && near(r.w, 432 - 108));
        core::doc::edit_masters(j, d, {"master.2"}, "Margins", [](core::Master& m) { m.margins.inside = 72; });
        r = core::margin_rect(d, right);
        const auto l = core::margin_rect(d, left);
        check("margins: inside by the spine -- mirrored on a left page", near(r.x, 72) && near(l.x, 54) && near(l.r(), 432 - 72));
        check("margins: the chapter master's deep top", near(core::margin_rect(d, "master.3").y, 180));
        const auto cols = core::page_columns(d, d.pages()[10]);   // the foldout
        check("columns: the foldout master's three", cols.size() == 3 && cols[0].x1 < cols[1].x0);
        check("columns: none without a master", core::page_columns(d, "nope").empty());
        core::doc::edit_masters(j, d, {"master.2"}, "Columns", [](core::Master& m) { m.columns = 2; m.gutter = 18; });
        const auto bc = core::page_columns(d, right);
        check("columns: two, the gutter between", bc.size() == 2 && near(bc[1].x0 - bc[0].x1, 18));
        const auto t = core::snap_targets(d, core::layout_pages(d, {right}), {});
        check("snap: to the margins and column edges",
              std::find_if(t.xs.begin(), t.xs.end(), [&](double x) { return near(x, bc[0].x1); }) != t.xs.end());
        core::Doc back;
        check("json: margins and gutter", core::from_json(back, core::to_json(d)) &&
                                          *back.as<core::Master>("master.2") == *d.as<core::Master>("master.2"));
        check("margins: through the door", d.strays() == 0);
    }

    // ── s013: PDF output ────────────────────────────────────────────────────
    {
        std::cout << "\n-- pdf --\n";
        namespace L = composix::layout;
        namespace fs = std::filesystem;
        core::Doc d;
        core::load_sample(d);
        // A fake image source: a flat plate for every asset but harbor.psd.
        int asked = 0;
        const L::ImageSource src = [&](const core::Asset& a, double, double) -> cairo_surface_t* {
            ++asked;
            if (a.name == "harbor.psd") return nullptr;
            cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 64, 32);
            cairo_t* c = cairo_create(s);
            cairo_set_source_rgb(c, 0.2, 0.4, 0.6);
            cairo_paint(c);
            cairo_destroy(c);
            return s;
        };
        const auto base = fs::temp_directory_path() / "composix_selftest_pdf";
        std::error_code ec;
        fs::create_directories(base, ec);
        const auto out = (base / "book.pdf").string();
        L::PdfReport rep;
        std::string err;
        check("export: ok", L::export_pdf(d, out, {true, true, ""}, src, &rep, &err), err);
        std::string pdf;
        { std::ifstream f(out, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); pdf = ss.str(); }
        check("export: a PDF", pdf.rfind("%PDF-", 0) == 0);
        int pages = 0;
        for (std::size_t at = 0; (at = pdf.find("/Type /Page", at)) != std::string::npos; ++at)
            if (pdf.compare(at, 12, "/Type /Pages") != 0) ++pages;
        check("export: every page", rep.pages == 16 && pages == 16, std::to_string(pages));
        check("export: fonts embedded", pdf.find("/FontFile") != std::string::npos);
        check("export: images asked for", asked >= 4, std::to_string(asked));
        check("export: the title", pdf.find("Sample Book") != std::string::npos);
        // Trim 432 x 648, bleed 9, slug 21 each side: 492 x 708; the foldout wider.
        check("export: page size = trim + bleed + slug", pdf.find("/MediaBox [ 0 0 492 708 ]") != std::string::npos);
        check("export: the foldout's own (wider) size",
              pdf.find("/MediaBox [ 0 0 " + std::to_string(static_cast<int>(d.as<core::Page>(d.pages()[10])->w) + 60)) != std::string::npos);
        check("export: page labels are the folios", pdf.find("(iv)") != std::string::npos);
        bool miss = false;
        for (const auto& w : rep.warnings) miss = miss || w == "Missing image: harbor.psd";
        check("export: the missing image reported", miss);
        check("export: no bleed, no marks = trim size", L::export_pdf(d, out, {false, false, ""}, src, &rep, &err));
        { std::ifstream f(out, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); pdf = ss.str(); }
        check("...432 x 648", pdf.find("/MediaBox [ 0 0 432 648 ]") != std::string::npos);
        check("export: refuses an unwritable path",
              !L::export_pdf(d, "/nonexistent-dir/x.pdf", {}, src, nullptr, &err) && !err.empty());
        fs::remove_all(base, ec);
    }

    // ── s012: text columns ──────────────────────────────────────────────────
    {
        std::cout << "\n-- columns --\n";
        namespace L = composix::layout;
        const auto near = [](double a, double b) { return std::abs(a - b) < 0.01; };
        core::Frame f;
        f.w = 300; f.columns = 3; f.gutter = 15;
        check("equal columns", near(core::column_width(f, 0), 90) && near(core::column_x(f, 2), 210));
        f.widths = {2, 1, 1};
        check("weighted 2:1:1", near(core::column_width(f, 0), 135) && near(core::column_width(f, 1), 67.5) &&
                                near(core::column_x(f, 1), 150));
        f.w = 600;
        check("weights keep their proportions on resize", near(core::column_width(f, 0), 285));
        f.widths = {1, 2};   // the wrong count: ignored, equal
        check("weights that don't fit: equal", near(core::column_width(f, 0), core::column_width(f, 2)));
        check("column at x: the nearest", core::column_at(f, 5) == 0 && core::column_at(f, 590) == 2 &&
                                          core::column_at(f, -50) == 0);

        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        load_plain(d);
        const auto one = L::flow_story(d, "story.1");
        check("columns: one step", core::doc::edit_frames(j, d, {"frame.1"}, "Columns", [](core::Frame& fr) {
                                       fr.columns = 2; fr.gutter = 18; fr.h = 140; }) && j.size() == 1);
        const auto two = L::flow_story(d, "story.1");
        const auto& ls = two.frames[0].lines;
        const bool both = std::any_of(ls.begin(), ls.end(), [](const L::Line& l) { return l.col == 0; }) &&
                          std::any_of(ls.begin(), ls.end(), [](const L::Line& l) { return l.col == 1; });
        check("text fills column 1, then column 2", both);
        const auto* f1 = d.as<core::Frame>("frame.1");
        check("column 2 starts after column 1 and the gutter",
              std::all_of(ls.begin(), ls.end(), [&](const L::Line& l) {
                  return l.col == 0 ? l.x == 0 : near(l.x, core::column_width(*f1, 0) + 18);
              }));
        check("narrower columns: more lines", two.lines > one.lines);
        // A place in column 2 round-trips through the caret and a click.
        const L::Line* c2 = nullptr;
        for (const auto& l : ls) if (l.col == 1) { c2 = &l; break; }
        if (c2) {
            const core::TextPos p{c2->para, c2->start + 2};
            const auto ca = L::caret_at(d, "story.1", two, p);
            check("caret in column 2: x past the gutter", ca.ok && ca.x > core::column_width(*f1, 0) + 18);
            check("click in column 2 comes back", L::hit_text(d, "story.1", two, "frame.1", ca.x + 0.1, ca.y + 1) == p);
            // Down from column 1's last line: column 2's first.
            const L::Line* last1 = nullptr;
            for (const auto& l : ls) if (l.col == 0) last1 = &l;
            const auto down = L::line_move(d, "story.1", two, {last1->para, last1->start}, +1, 0);
            check("down off column 1: into column 2", down.para == c2->para && down.byte >= c2->start &&
                                                      down.byte <= c2->end);
        } else {
            check("column 2 has text", false);
        }
        // Balance: where the story ends, the columns come out even. (s043: the
        // sample's Chapter Two starts a right-hand page; here it may follow on,
        // so the story ends in this frame.)
        core::doc::edit_styles(j, d, {"style.2"}, "Start", [](core::Style& s) { s.start = core::ParaStart::Anywhere; });
        core::doc::edit_frames(j, d, {"frame.1"}, "Columns", [](core::Frame& fr) { fr.h = 600; fr.balance = true; });
        const auto bal = L::flow_story(d, "story.1");
        int n0 = 0, n1 = 0;
        for (const auto& l : bal.frames[0].lines) (l.col == 0 ? n0 : n1)++;
        check("balanced: the columns within a couple of lines", n1 > 0 && std::abs(n0 - n1) <= 3 && !bal.overset,
              std::to_string(n0) + " / " + std::to_string(n1));
        core::Doc back;
        check("json: columns, gutter, balance, weights",
              core::doc::edit_frames(j, d, {"frame.1"}, "Widths", [](core::Frame& fr) { fr.widths = {3, 2}; }) &&
              core::from_json(back, core::to_json(d)) && *back.as<core::Frame>("frame.1") == *d.as<core::Frame>("frame.1"));
        check("columns: through the door", d.strays() == 0);
    }

    // ── s011: styles + fonts ────────────────────────────────────────────────
    {
        std::cout << "\n-- styles --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        load_plain(d);
        using P = core::TextPos;
        const auto runs = [&](const char* story, int p) { return d.as<core::Story>(story)->paras[static_cast<std::size_t>(p)].runs; };
        const auto text = [&](const char* story, int p) { return d.as<core::Story>(story)->paras[static_cast<std::size_t>(p)].text; };

        // story.4 para 0: "Almanac of the Outer Ports" (0-26) in Emphasis.
        auto c = core::doc::edit_text(j, d, "story.4", {0, 7}, {0, 7}, "XX");
        check("typing inside a run: the run grows", runs("story.4", 0).size() == 1 && runs("story.4", 0)[0].end == 28);
        c = core::doc::edit_text(j, d, "story.4", {0, 28}, {0, 28}, "!");
        check("typing at a run's end continues its style", runs("story.4", 0)[0].end == 29);
        c = core::doc::edit_text(j, d, "story.4", {0, 0}, {0, 0}, ">");
        check("typing before a run (at the start) takes the first character's style",
              runs("story.4", 0)[0].start == 0 && runs("story.4", 0)[0].end == 30);
        j.undo(); j.undo(); j.undo();
        core::doc::edit_text(j, d, "story.4", {0, 10}, {0, 10}, "\n");
        check("enter inside a run: both halves keep it", runs("story.4", 0) == std::vector<core::Run>{{0, 10, "style.5"}} &&
                                                       runs("story.4", 1) == std::vector<core::Run>{{0, 16, "style.5"}});
        j.undo();
        core::doc::edit_text(j, d, "story.4", {0, 26}, {0, 35}, "Z");   // replace from just after the run
        check("replacing a selection takes its first character's style", runs("story.4", 0) == std::vector<core::Run>{{0, 26, "style.5"}});
        j.undo();
        core::doc::edit_text(j, d, "story.4", {0, 0}, {0, 7}, "Book");
        check("...and inside a run, the run's", runs("story.4", 0) == std::vector<core::Run>{{0, 23, "style.5"}});
        j.undo();
        core::doc::edit_text(j, d, "story.4", {0, 3}, {0, 30}, "");
        check("deleting across a run's end trims it", runs("story.4", 0) == std::vector<core::Run>{{0, 3, "style.5"}} &&
                                                      text("story.4", 0) == "Almd ed.", text("story.4", 0));
        j.undo();

        check("char style over plain text", core::doc::set_char_style(j, d, "story.1", {1, 4}, {1, 10}, "style.5") &&
                                            runs("story.1", 1) == std::vector<core::Run>{{4, 10, "style.5"}});
        check("...touching runs of one style merge",
              core::doc::set_char_style(j, d, "story.1", {1, 10}, {1, 14}, "style.5") &&
              runs("story.1", 1) == std::vector<core::Run>{{4, 14, "style.5"}});
        check("clear part of a run splits it", core::doc::set_char_style(j, d, "story.1", {1, 6}, {1, 8}, "") &&
                                               runs("story.1", 1) == std::vector<core::Run>({{4, 6, "style.5"}, {8, 14, "style.5"}}));
        check("char style refuses a paragraph style", !core::doc::set_char_style(j, d, "story.1", {1, 0}, {1, 3}, "style.1"));
        check("para style across paragraphs", core::doc::set_para_style(j, d, "story.1", {1, 5}, {2, 1}, "style.6") &&
                                              d.as<core::Story>("story.1")->paras[1].style == "style.6" &&
                                              d.as<core::Story>("story.1")->paras[2].style == "style.6" &&
                                              d.as<core::Story>("story.1")->paras[3].style == "style.1");
        check("para style refuses a character style", !core::doc::set_para_style(j, d, "story.1", {1, 0}, {1, 0}, "style.5"));

        check("edit styles: one step for two", core::doc::edit_styles(j, d, {"style.1", "style.4"}, "Size",
                                                                      [](core::Style& s) { s.size = 13; }) &&
                                               d.as<core::Style>("style.4")->size == 13 && j.undo_label() == "Size");
        const auto dup = core::doc::duplicate_style(j, d, "style.1");
        check("duplicate: a copy, named so, just after", d.as<core::Style>(dup) &&
                                                         d.as<core::Style>(dup)->name == "Body copy" &&
                                                         d.ids_of<core::Style>()[1] == dup);
        check("uses: counted", core::doc::style_uses(d, "style.5") >= 1 && core::doc::style_uses(d, dup) == 0);
        check("delete: refuses a used style", !core::doc::delete_styles(j, d, {"style.1"}));
        check("delete: an unused one goes", core::doc::delete_styles(j, d, {dup}) && !d.as<core::Style>(dup));

        // The flow: spacing and a bold run.
        core::Doc e;
        load_plain(e);
        const auto f0 = L::flow_story(e, "story.1");
        check("space after a heading (6 pt)", f0.frames[0].lines[1].y == 28 + 6);
        int n_before = 0;
        for (const auto& l : f0.frames[0].lines) n_before += l.para == 1;
        {
            core::Doc::Loading load(e);
            auto st = *e.as<core::Story>("story.1");
            st.paras[1].runs = {{0, static_cast<int>(st.paras[1].text.size()), "style.7"}};
            e.put("story.1", st);
            e.put("style.7", core::Style{"Huge", core::StyleKind::Character, 7, "", 30, 0, true, false});
        }
        const auto f1 = L::flow_story(e, "story.1");
        int n_after = 0;
        for (const auto& l : f1.frames[0].lines) n_after += l.para == 1;
        check("a big bold run breaks into more lines", n_after > n_before,
              std::to_string(n_before) + " -> " + std::to_string(n_after));

        core::Doc back;
        check("json: runs and spacing round trip", core::from_json(back, core::to_json(d)) &&
                                                   *back.as<core::Story>("story.4") == *d.as<core::Story>("story.4") &&
                                                   *back.as<core::Style>("style.2") == *d.as<core::Style>("style.2"));
        const auto& fams = L::font_families();
        check("fonts: listed, the generics first", fams.size() >= 3 && fams[0] == "Serif" && fams[1] == "Sans",
              std::to_string(fams.size()));
        check("styles: through the door", d.strays() == 0);
    }

    // ── s009: images ─────────────────────────────────────────────────────────
    {
        std::cout << "\n-- images --\n";
        namespace fs = std::filesystem;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto near = [](double a, double b) { return std::abs(a - b) < 0.01; };

        const core::Rect fr{0, 0, 288, 144};   // 4 x 2 in
        const auto fill = core::fit_rect(fr, 1200, 1200, core::ImageFit::Fill);
        const auto fit  = core::fit_rect(fr, 1200, 1200, core::ImageFit::Fit);
        check("fit_rect: fill covers, centred", near(fill.w, 288) && near(fill.h, 288) && near(fill.y, -72));
        check("fit_rect: fit shows it whole", near(fit.w, 144) && near(fit.h, 144) && near(fit.x, 72));
        core::Asset a1;
        a1.px_w = 1200; a1.px_h = 1200;
        core::Frame f1;
        f1.w = 288; f1.h = 144;
        check("ppi: fill = pixels over the drawn width", near(core::effective_ppi(f1, a1), 300));
        f1.fit = core::ImageFit::Fit;
        check("ppi: fit draws it smaller, sharper", near(core::effective_ppi(f1, a1), 600));
        a1.px_w = 0;
        check("ppi: a vector has none", core::effective_ppi(f1, a1) == 0);
        const core::ImageFile big{"/x/big.png", "big.png", 3000, 2000, false, "RGB", 1};
        const auto [nw, nh] = core::natural_size(big, 0, 0);
        check("natural size: 300 ppi", near(nw, 720) && near(nh, 480));
        const auto [sw, sh] = core::natural_size(big, 360, 0);
        check("natural size: shrunk to fit, aspect kept", near(sw, 360) && near(sh, 240));

        const core::ImageFile cove{"/pics/cove.png", "cove.png", 2400, 1600, false, "RGB", 7};
        const auto before = d.ids_of<core::Asset>().size();
        check("place into a frame", core::doc::place_image(j, d, "frame.4", cove));
        const auto now = d.as<core::Frame>("frame.4")->asset;
        check("...a new linked asset", d.as<core::Asset>(now) && d.as<core::Asset>(now)->path == cove.path &&
                                       !d.as<core::Asset>(now)->embedded);
        check("...the old image, shown nowhere else, goes", !d.as<core::Asset>("asset.2") &&
                                                            d.ids_of<core::Asset>().size() == before);
        check("...one step", j.size() == 1);
        check("placing the same file again: no step", !core::doc::place_image(j, d, "frame.4", cove));
        j.undo();
        check("undo: the old image back", d.as<core::Frame>("frame.4")->asset == "asset.2" &&
                                          d.as<core::Asset>("asset.2") && !d.as<core::Asset>(now));
        j.redo();
        const auto nf = core::doc::place_image_new(j, d, "page.8", {54, 54, 288, 192}, cove);
        check("place new: a frame showing the SAME asset", d.as<core::Frame>(nf) &&
                                                           d.as<core::Frame>(nf)->asset == now &&
                                                           d.ids_of<core::Asset>().size() == before);
        check("place new: refused off a page", core::doc::place_image_new(j, d, "", {0, 0, 9, 9}, cove).empty());
        check("place: refused into a text frame", !core::doc::place_image(j, d, "frame.1", cove));

        core::doc::delete_frames(j, d, {"frame.8"});
        check("delete one of two placements: the image stays", d.as<core::Asset>("asset.3"));
        core::doc::delete_frames(j, d, {"frame.9"});
        check("delete the last placement: the image goes", !d.as<core::Asset>("asset.3"));
        core::doc::delete_frames(j, d, {"frame.6"});
        check("delete a story's last frame: the story goes", !d.as<core::Story>("story.2"));
        j.undo();
        check("undo: the story back with its frame", d.as<core::Story>("story.2") && d.as<core::Frame>("frame.6"));
        j.undo();
        j.undo();
        check("undo: the image back with its frames", d.as<core::Asset>("asset.3") && d.as<core::Frame>("frame.9"));
        core::doc::delete_frames(j, d, {"frame.2"});
        check("delete mid-thread: the story stays", d.as<core::Story>("story.1"));
        j.undo();

        core::doc::relink_assets(j, d, {{"asset.1", {"/pics/map.png", "map.png", 5000, 2500, false, "RGB", 3}}});
        const auto* r1 = d.as<core::Asset>("asset.1");
        check("relink: the file, its pixels, linked now", r1->path == "/pics/map.png" && r1->px_w == 5000 &&
                                                          !r1->embedded && r1->name == "map.png");
        check("relink: labelled", j.undo_label().find("Relink") != std::string::npos, j.undo_label());
        j.undo();
        check("embed: refused when already embedded", !core::doc::embed_assets(j, d, {{"asset.1", "assets/x"}}));
        core::doc::unembed_assets(j, d, {"asset.1", "asset.3"});
        check("unembed: linked to where it came from", d.as<core::Asset>("asset.1")->path == "Maps/guild-map.tif" &&
                                                        !d.as<core::Asset>("asset.1")->embedded);
        check("unembed: no source, left embedded", d.as<core::Asset>("asset.3")->embedded);
        core::doc::embed_assets(j, d, {{"asset.1", "assets/guild-map.tif"}});
        check("embed: reads from the project, remembers the source",
              d.as<core::Asset>("asset.1")->path == "assets/guild-map.tif" &&
              d.as<core::Asset>("asset.1")->source == "Maps/guild-map.tif");
        check("set fit", core::doc::set_fit(j, d, {"frame.5", "frame.1"}, core::ImageFit::Fit) &&
                         d.as<core::Frame>("frame.5")->fit == core::ImageFit::Fit);
        check("images: through the door", d.strays() == 0);

        // Link status against real files.
        const fs::path base = fs::temp_directory_path() / "composix_selftest_images";
        std::error_code ec;
        fs::remove_all(base, ec);
        fs::create_directories(base / "proj" / "assets", ec);
        { std::ofstream(base / "pic.png") << "x"; }
        { std::ofstream(base / "proj" / "assets" / "emb.png") << "x"; }
        core::Asset la;
        la.path = (base / "pic.png").string();
        la.stamp = core::file_stamp(la.path);
        check("status: OK", core::link_status(la, "") == core::LinkStatus::OK);
        la.stamp += 5;
        check("status: Modified when the file's time moved", core::link_status(la, "") == core::LinkStatus::Modified);
        la.path = (base / "gone.png").string();
        check("status: Missing", core::link_status(la, "") == core::LinkStatus::Missing);
        core::Asset ea;
        ea.embedded = true;
        ea.path = "assets/emb.png";
        check("status: embedded, in the project", core::link_status(ea, (base / "proj").string()) == core::LinkStatus::Embedded);
        ea.path = "assets/none.png";
        check("status: embedded, missing from the project",
              core::link_status(ea, (base / "proj").string()) == core::LinkStatus::EmbeddedMissing);

        // JSON keeps the new fields; Save As brings the embedded files.
        core::Doc e;
        e.put("asset.1", core::Asset{"emb.png", 1, "assets/emb.png", true, "RGB", 10, 10, "/orig/emb.png", 42});
        e.put("page.1", core::Page{});
        core::Frame ff;
        ff.owner = "page.1"; ff.kind = core::FrameKind::Image; ff.w = ff.h = 72; ff.asset = "asset.1";
        ff.fit = core::ImageFit::Fit;
        e.put("frame.1", ff);
        core::Doc back;
        check("json: round trip", core::from_json(back, core::to_json(e)) &&
                                  *back.as<core::Asset>("asset.1") == *e.as<core::Asset>("asset.1") &&
                                  back.as<core::Frame>("frame.1")->fit == core::ImageFit::Fit);
        std::string err;
        check("save as: ok", core::save_project(e, (base / "copy").string(), &err, (base / "proj").string()), err);
        check("save as: the embedded file came along", fs::exists(base / "copy" / "assets" / "emb.png"));

        // The projection.
        core::Doc pd;
        core::load_sample(pd);
        auto o = core::project(pd, "");
        const auto* harbor = o.find("asset.2");
        check("links: a missing link says so", harbor && harbor->badge == "Missing");
        bool acts = false;
        for (const auto& fl : harbor->fields) if (fl.buttons && fl.edit == "asset") acts = true;
        check("links: actions as buttons", acts);
        core::DocJournal pj{pd};
        pd.set_door([&pj] { return pj.open(); });
        core::doc::place_image(pj, pd, "frame.5", {"/pics/tiny.png", "tiny.png", 300, 100, false, "RGB", 1});
        o = core::project(pd, "");
        const auto* tiny = o.find(pd.as<core::Frame>("frame.5")->asset);
        std::string ppi;
        if (tiny) for (const auto& fl : tiny->fields) if (fl.label == "Effective ppi") ppi = fl.value;
        check("links: low res when stretched", ppi.find("(low)") != std::string::npos, ppi);
        const auto fi = core::frame_items(pd, {"frame.5"});
        bool fit_edit = false;
        for (const auto& fl : fi.front().fields) if (fl.edit == "fit") fit_edit = true;
        check("frame inspector: Fit is editable", fit_edit);
        fs::remove_all(base, ec);
    }

    std::cout << "-----------------------------------------------\n";

    // -- Recents: list ops + JSON round-trip with prune -----------------------
    {
        std::vector<std::string> l;
        core::recents_add(l, "/a");
        core::recents_add(l, "/b");
        core::recents_add(l, "/c");
        check("recents: newest first", l.size() == 3 && l.front() == "/c");
        core::recents_add(l, "/a");
        check("recents: re-add moves to front (no dup)",
              l.size() == 3 && l.front() == "/a");
        core::recents_add(l, "/x/");
        check("recents: trailing slash normalised",
              std::find(l.begin(), l.end(), "/x") != l.end() &&
              std::find(l.begin(), l.end(), "/x/") == l.end());
        std::vector<std::string> cap;
        for (int i = 0; i < 20; ++i)
            core::recents_add(cap, "/p" + std::to_string(i), 8);
        check("recents: trim to max (8)", cap.size() == 8 && cap.front() == "/p19");

        const std::string base =
            (std::filesystem::temp_directory_path() / "composix_selftest_recents").string();
        std::error_code ec;
        std::filesystem::remove_all(base, ec);
        const std::string live = base + "/live";
        std::filesystem::create_directories(live, ec);
        const std::string file = base + "/recent.json";

        std::vector<std::string> to_save = {live, base + "/GONE"};
        check("recents: save ok", core::save_recents(file, to_save));
        auto loaded = core::load_recents(file);
        check("recents: load prunes the missing path",
              loaded.size() == 1 && loaded.front() == live);
        check("recents: missing file -> empty",
              core::load_recents(base + "/nope.json").empty());
    }

    // -- TextMap: forward/inverse round-trip ----------------------------------
    {
        // A scene as a viewer flattens it:
        //   0 "The Cellar"     title  (non-prose)
        //   1 ""               gap    (non-prose)
        //   2 "She opened the" prose, para_start   <- para 1, wrap 1
        //   3 "old iron door." prose               <- para 1, wrap 2
        //   4 "Caf\u00e9 dust." prose, para_start  <- para 2 (UTF-8: é)
        std::vector<core::FlatLine> flat = {
            {"The Cellar",     false, false},
            {"",               false, false},
            {"She opened the", true,  true },
            {"old iron door.", true,  false},
            {"Caf\xC3\xA9 dust.", true, true },
        };
        roundtrip(flat, 2, 0, 2, 14, "single line");
        roundtrip(flat, 2, 4, 3, 8,  "multi-line (soft wrap)");
        roundtrip(flat, 2, 0, 4, 10, "cross-paragraph");
        roundtrip(flat, 4, 0, 4, 6,  "UTF-8 span (accented)");

        // Chrome is never landed on: a range covering all prose starts on line 2.
        std::vector<int> off;
        (void)core::visible_text(flat, off);
        core::LineCol s, e;
        core::map_range(flat, 0, 3, s, e);
        check("textmap: start snaps to prose, never chrome", s.line == 2);

        // Degenerate / empty inputs fail cleanly.
        check("textmap: empty range rejected", !core::map_range(flat, 5, 5, s, e));
        std::vector<core::FlatLine> no_prose = {{"Title", false, false}};
        check("textmap: no prose -> false", !core::map_range(no_prose, 0, 3, s, e));
    }
    // -- Shortcuts: the registry paradigm, exercised ---------------------------
    // A seed's claim isn't a fact until a consumer exercises it. The registry's
    // whole promise is that a key and its advertised row can't drift, because
    // both read one list -- so the guard that matters is pure and lives here:
    // no two chords may collide. (App wires from this same list; ShortcutsDialog
    // renders from it.)
    {
        namespace sc = composix::core;

        check("shortcuts: format_accel renders modifiers + key",
              sc::format_accel("<Ctrl><Shift>n") == "Ctrl+Shift+N",
              sc::format_accel("<Ctrl><Shift>n"));
        check("shortcuts: format_accel maps a named key to its glyph",
              sc::format_accel("<Ctrl>question") == "Ctrl+?",
              sc::format_accel("<Ctrl>question"));

        {
            sc::ShortcutSpec derived; derived.accels = {"<Ctrl>question", "<Ctrl>slash"};
            check("shortcuts: display_keys derives + joins accels",
                  derived.display_keys() == "Ctrl+?  /  Ctrl+/",
                  derived.display_keys());
            sc::ShortcutSpec doc; doc.keys = "Click a recent";
            check("shortcuts: display_keys prefers an explicit key string",
                  doc.display_keys() == "Click a recent");
        }

        check("shortcuts: registry is populated",
              !sc::shortcut_registry().empty(),
              std::to_string(sc::shortcut_registry().size()) + " specs");
        check("shortcuts: registry is accel-collision-free",
              sc::find_accel_collisions().empty(),
              sc::find_accel_collisions().empty()
                  ? "clean" : sc::find_accel_collisions().front());

        // Sections authored A-Z + contiguous: the dialog walks linearly and
        // starts a heading on change, so a stray out-of-order row would split a
        // section into two headings.
        {
            std::vector<std::string> secs;
            for (const auto& r : sc::shortcut_registry())
                if (secs.empty() || secs.back() != r.section) secs.push_back(r.section);
            std::vector<std::string> sorted = secs;
            std::sort(sorted.begin(), sorted.end());
            check("shortcuts: sections are contiguous and A-Z ordered", secs == sorted);
        }

        // The doc-only half of the paradigm: a gesture row documents without
        // binding (no accels), but must still show keys.
        {
            bool ok = true, saw_doc_only = false;
            for (const auto& r : sc::shortcut_registry())
                if (r.action.empty()) {
                    saw_doc_only = true;
                    if (!r.accels.empty() || r.keys.empty()) ok = false;
                }
            check("shortcuts: a doc-only row binds nothing but still displays keys",
                  ok && saw_doc_only);
        }
    }



    {
        std::cout << "\n-- text wrap --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto* pic = d.as<core::Frame>("frame.11");
        const auto* txt = d.as<core::Frame>("frame.1");
        // The wrap edge, in frame.2's coordinates: the picture plus 9 pt a side.
        const auto wr = core::wrap_rects(d, "frame.1");
        check("wrap: the picture pushes the opening text", wr.size() == 1 && !wr[0].jump &&
                                                          std::abs(wr[0].x0 - (pic->x - 9 - txt->x)) < 1e-9 &&
                                                          std::abs(wr[0].y1 - (pic->y + pic->h + 9 - txt->y)) < 1e-9);
        check("wrap: not its own frame, not other pages", core::wrap_rects(d, "frame.11").empty() &&
                                                             core::wrap_rects(d, "frame.2").empty());
        const auto flow = L::flow_story(d, "story.1");
        const L::FrameText* ft = nullptr;
        for (const auto& f : flow.frames) if (f.frame == "frame.1") ft = &f;
        bool clear = ft != nullptr, both = false;
        int beside = 0;
        if (ft && !wr.empty())
            for (const auto& l : ft->lines) {
                const double right = l.x + (l.w > 0 ? l.w : txt->w);
                const bool in_band = l.y + l.h > wr[0].y0 && l.y < wr[0].y1;
                if (in_band) {
                    ++beside;
                    clear = clear && (right <= wr[0].x0 + 0.01 || l.x >= wr[0].x1 - 0.01);
                    both = both || l.x >= wr[0].x1 - 0.01;
                }
            }
        check("wrap: lines beside it keep clear of the edge", clear && beside > 0);
        check("wrap: Around sets text on both sides", both);
        // A row of two: the click picks the side, arrows move by rows.
        if (ft) {
            const L::Line* left = nullptr;
            const L::Line* right = nullptr;
            for (std::size_t k = 0; k + 1 < ft->lines.size() && !right; ++k)
                if (ft->lines[k + 1].y == ft->lines[k].y && ft->lines[k + 1].x > ft->lines[k].x &&
                    ft->lines[k].cap <= 0 && ft->lines[k + 1].cap <= 0) {   // (not a drop cap and its line -- s029)
                    left = &ft->lines[k];
                    right = &ft->lines[k + 1];
                }
            check("wrap: two lines side by side", left && right);
            if (left && right) {
                const auto hl = L::hit_text(d, "story.1", flow, "frame.1", left->x + 5, left->y + 2);
                const auto hr = L::hit_text(d, "story.1", flow, "frame.1", right->x + 5, right->y + 2);
                check("wrap: a click on each side lands on its line",
                      hl.byte >= left->start && hl.byte <= left->end && hr.byte >= right->start && hr.byte <= right->end);
                const auto down = L::line_move(d, "story.1", flow, {left->para, left->start + 1}, 1, left->x + 5);
                const auto cd = L::caret_at(d, "story.1", flow, down);
                check("wrap: down from the left goes down, not across", cd.ok && cd.y > left->y && cd.x < right->x);
            }
        }
        // Jump: nothing beside it.
        check("wrap: set to Jump", core::doc::edit_frames(j, d, {"frame.11"}, "Text wrap",
                                                          [](core::Frame& f) { f.wrap = core::Wrap::Jump; }));
        const auto jf = L::flow_story(d, "story.1");
        bool none = true;
        const auto jr = core::wrap_rects(d, "frame.1");
        for (const auto& f : jf.frames)
            if (f.frame == "frame.1" && !jr.empty())
                for (const auto& l : f.lines) none = none && (l.cap > 0 || !(l.y + l.h > jr[0].y0 && l.y < jr[0].y1));
        check("wrap: Jump leaves its band empty", none && !jr.empty() && jr[0].jump);
        j.undo();
        check("wrap: undo", d.as<core::Frame>("frame.11")->wrap == core::Wrap::Around);
        // None: the opening text as before s018 (no line set on its own).
        check("wrap: off", core::doc::edit_frames(j, d, {"frame.11"}, "Text wrap",
                                                  [](core::Frame& f) { f.wrap = core::Wrap::None; }));
        bool plain = true;
        for (const auto& f : L::flow_story(d, "story.1").frames)
            for (const auto& l : f.lines) {   // (a drop cap's lines are set on their own too -- s029; a hyphenating style's, s038)
                const auto& ps = d.as<core::Story>("story.1")->paras[static_cast<std::size_t>(l.para)].style;
                plain = plain && (l.w <= 0 || ps == "style.10" || d.as<core::Style>(ps)->hyphenate);
            }
        check("wrap: none sets every line with its paragraph", plain);
        // The inspector: Wrap on every frame, Offset when it wraps.
        j.undo();
        const auto items = core::frame_items(d, {"frame.11"}, "");
        bool wf = false, of = false;
        for (const auto& f : items.front().fields) {
            wf = wf || (f.edit == "wrap" && f.value == "Around");
            of = of || (f.edit == "wrap_off" && f.value == "9\t9\t9\t9");
        }
        check("wrap: inspector shows Wrap + offset", wf && of);
    }

    {
        std::cout << "\n-- s173: wrap to an image's shape --\n";
        namespace L = composix::layout;
        // Tracing: a see-through picture -- a triangle, its point up.
        const int W = 40, H = 40;
        std::vector<std::uint32_t> px(W * H, 0x00000000u);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                if (std::abs(x - 20) * 2 <= y) px[static_cast<std::size_t>(y * W + x)] = 0xff336699u;
        const auto tri = core::trace_shape(px.data(), W, H, W, 4);
        check("trace: aspect, then a span a band", tri.size() == 9 && std::abs(tri[0] - 1.f) < 1e-6);
        check("trace: narrow at the top, wide at the foot", tri.size() == 9 && (tri[2] - tri[1]) < (tri[8] - tri[7]) &&
              tri[7] <= 0.05f && tri[8] >= 0.95f && tri[1] > 0.3f && tri[2] < 0.7f);
        {   // No alpha: a product on white -- the corners' colour is the ground.
            std::vector<std::uint32_t> q(W * H, 0xfffefefeu);
            for (int y = 10; y < 30; ++y) for (int x = 15; x < 25; ++x) q[static_cast<std::size_t>(y * W + x)] = 0xff202020u;
            const auto sq = core::trace_shape(q.data(), W, H, W, 4);
            check("trace: on a plain ground -- the object, the empty bands marked",
                  sq.size() == 9 && sq[1] > sq[2] && std::abs(sq[3] - 15.f / 40) < 1e-6 && std::abs(sq[4] - 25.f / 40) < 1e-6 && sq[7] > sq[8]);
            q[0] = 0xff00ff00u;   // a corner of another colour: a photo to its edges
            check("trace: a photo to its edges -- nothing", core::trace_shape(q.data(), W, H, W, 4).empty());
            std::vector<std::uint32_t> none(W * H, 0x00000000u);
            check("trace: see-through all over -- nothing", core::trace_shape(none.data(), W, H, W, 4).empty());
        }
        // As placed: Fit in a frame twice as wide -- the picture centred.
        {
            core::Frame f;
            f.kind = core::FrameKind::Image;
            f.w = 200; f.h = 100;
            f.fit = core::ImageFit::Fit;
            core::Asset a;
            a.shape = tri;
            const auto b = core::shape_bands(f, a);
            check("shape bands: in the frame, the picture's place", b.size() == 4 && b[0].x0 > 50 && b[3].x0 >= 50 - 1e-9 &&
                  b[3].x1 <= 150 + 1e-9 && std::abs(b[0].y0) < 1e-9 && std::abs(b[3].y1 - 100) < 1e-9);
            a.shape.clear();
            check("shape bands: untraced -- none", core::shape_bands(f, a).empty());
        }
        // The sample's lighthouse (Around) -> Shape, traced as the triangle.
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto around = core::wrap_rects(d, "frame.1");
        const auto aid = d.as<core::Frame>("frame.11")->asset;
        const auto steps = j.size();
        check("wrap shape: one step", core::doc::set_wrap_shape(j, d, {"frame.11", "frame.1"}, {{aid, tri}}) && j.size() == steps + 1 &&
              d.as<core::Frame>("frame.11")->wrap == core::Wrap::Shape && d.as<core::Asset>(aid)->shape == tri &&
              d.as<core::Frame>("frame.1")->wrap == core::Wrap::None);
        check("wrap shape: again -- nothing", !core::doc::set_wrap_shape(j, d, {"frame.11"}, {{aid, tri}}) && j.size() == steps + 1);
        const auto shaped = core::wrap_rects(d, "frame.1");
        bool inside = around.size() == 1 && shaped.size() == 4;
        for (const auto& r : shaped)
            inside = inside && r.x0 >= around[0].x0 - 1e-9 && r.x1 <= around[0].x1 + 1e-9 && r.y0 >= around[0].y0 - 1e-9 && r.y1 <= around[0].y1 + 1e-9;
        check("wrap shape: a rect a band, within the frame's wrap", inside && shaped[0].x1 - shaped[0].x0 < shaped[3].x1 - shaped[3].x0);
        {   // The text comes into the frame's old box, and keeps clear of every band.
            const auto flow = L::flow_story(d, "story.1");
            bool clear = true, closer = false;
            for (const auto& ft : flow.frames) {
                if (ft.frame != "frame.1") continue;
                const double fw = d.as<core::Frame>("frame.1")->w;
                for (const auto& l : ft.lines) {
                    const double x1 = l.x + (l.w > 0 ? l.w : fw);
                    for (const auto& r : shaped)
                        if (l.y + l.h > r.y0 && l.y < r.y1) clear = clear && (x1 <= r.x0 + 0.01 || l.x >= r.x1 - 0.01);
                    if (!around.empty() && l.y + l.h > around[0].y0 && l.y < around[0].y1 && l.x > around[0].x0 + 1 && l.x < around[0].x1 - 1)
                        closer = true;
                }
            }
            check("wrap shape: lines keep clear of the outline", clear);
            check("wrap shape: text comes in closer than the frame's box", closer);
        }
        {
            core::Doc e;
            check("wrap shape: saved with the document (outline rounded)", core::from_json(e, core::to_json(d)) &&
                  e.as<core::Frame>("frame.11")->wrap == core::Wrap::Shape && e.as<core::Asset>(aid)->shape.size() == tri.size() &&
                  std::abs(e.as<core::Asset>(aid)->shape[2] - tri[2]) < 1e-4);
        }
        {
            const auto items = core::frame_items(d, {"frame.11"}, "");
            bool sh = false;
            for (const auto& f : items.front().fields)
                if (f.edit == "wrap") for (const auto& c : f.choices) sh = sh || (c.id == "3" && f.value == "Shape");
            const auto titems = core::frame_items(d, {"frame.1"}, "");
            bool tsh = false;
            for (const auto& f : titems.front().fields)
                if (f.edit == "wrap") for (const auto& c : f.choices) tsh = tsh || c.id == "3";
            check("wrap shape: the inspector offers Shape on pictures only", sh && !tsh);
        }
        // A relink keeps a traced outline when the new file brings one; untraced wraps the frame.
        core::ImageFile nf{"/pics/new.png", "new.png", 400, 400, false, "RGB", 5};
        check("wrap shape: relinked without an outline -- the frame's box", core::doc::relink_assets(j, d, {{aid, nf}}) &&
              d.as<core::Asset>(aid)->shape.empty() && core::wrap_rects(d, "frame.1").size() == 1);
        nf.shape = tri;
        nf.stamp = 6;
        check("wrap shape: relinked with one -- kept", core::doc::relink_assets(j, d, {{aid, nf}}) &&
              d.as<core::Asset>(aid)->shape == tri && core::wrap_rects(d, "frame.1").size() == 4);
        {   // s175: placed into a Shape frame with its outline (Replace, a drop): kept -- a new asset or one already there.
            core::ImageFile pf{"/pics/other.png", "other.png", 300, 300, false, "RGB", 3};
            pf.shape = tri;
            const bool placed = core::doc::place_image(j, d, "frame.11", pf);
            const auto* na = d.as<core::Asset>(d.as<core::Frame>("frame.11")->asset);
            check("wrap shape: placed with an outline -- kept, wraps to it", placed && na && na->shape == tri &&
                  core::wrap_rects(d, "frame.1").size() == 4);
            j.undo();
            core::ImageFile again{"/pics/cove.png", "cove.png", 2400, 1600, false, "RGB", 7};
            const bool p1 = core::doc::place_image(j, d, "frame.4", again);   // cove.png now an asset, untraced
            again.shape = tri;
            const bool p2 = core::doc::place_image(j, d, "frame.11", again);
            const auto* ca = d.as<core::Asset>(d.as<core::Frame>("frame.11")->asset);
            check("wrap shape: an asset already there gains the outline", p1 && p2 && ca && ca->shape == tri);
            j.undo(); j.undo();
        }
        j.undo(); j.undo(); j.undo();
        check("wrap shape: undone -- Around, untraced", d.as<core::Frame>("frame.11")->wrap == core::Wrap::Around &&
              d.as<core::Asset>(aid)->shape.empty() && core::wrap_rects(d, "frame.1").size() == 1);
    }

    {
        std::cout << "\n-- new document --\n";
        namespace L = composix::layout;
        const double in = core::kPt;
        check("presets: Trade 6 x 9 first", !core::trim_presets().empty() &&
                                            core::trim_presets().front().w == 6 * in && core::trim_presets().front().h == 9 * in);
        core::NewDoc n;
        check("defaults are usable", core::check_new(n).empty(), core::check_new(n));
        {
            auto b = n; b.pages = 0;
            check("refused: no pages", !core::check_new(b).empty());
            b = n; b.margins.inside = b.margins.outside = 3 * in;
            check("refused: margins eat the page", core::check_new(b).find("half an inch") != std::string::npos);
            b = n; b.columns = 12; b.gutter = 24;
            check("refused: columns too narrow", core::check_new(b).find("narrower") != std::string::npos);
            core::Doc keep;
            core::load_sample(keep);
            const auto before = keep.size();
            b = n; b.trim_w = 0.5 * in;
            check("refused: the document is left alone", !core::new_document(keep, b) && keep.size() == before);
        }
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        n.title = "Field Notes";
        n.pages = 8;
        n.margins = {0.75 * in, 1 * in, 0.875 * in, 0.5 * in};
        n.columns = 2;
        n.gutter = 18;
        check("made", core::new_document(d, n));
        check("settings: title, trim, bleed, facing", d.settings().title == "Field Notes" && d.settings().trim_w == 6 * in &&
                                                      d.settings().bleed == 0.125 * in && d.settings().facing);
        check("eight pages on the master", d.pages().size() == 8 && d.as<core::Page>("page.8")->master == "master.1");
        const auto* m = d.as<core::Master>("master.1");
        check("the master carries margins + columns", m && m->margins == n.margins && m->columns == 2 && m->gutter == 18);
        check("starts on a right-hand page", d.side_of("page.1") == core::Side::Right && d.side_of("page.2") == core::Side::Left);
        const auto* f1 = d.as<core::Frame>("frame.1");
        const auto* f2 = d.as<core::Frame>("frame.2");
        check("a frame in each page's margins (left pages mirrored)",
              f1 && f2 && f1->x == 0.875 * in && f2->x == 0.5 * in && f1->y == 0.75 * in &&
                  std::abs(f1->h - (9 * in - 1.75 * in)) < 1e-9 && f1->columns == 2 && f1->gutter == 18);
        check("threaded page to page", L::thread_frames(d, "story.1").size() == 8 && d.as<core::Frame>("frame.8")->next.empty());
        check("the story flows, nothing over", !L::flow_story(d, "story.1").overset);
        check("styles + swatches to start with", d.ids_of<core::Style>().size() == 6 && d.ids_of<core::Swatch>().size() == 2);
        check("a folio on the master: the page number", d.as<core::Frame>("frame.9") &&
                                                        d.as<core::Frame>("frame.9")->owner == "master.1" &&
                                                        d.as<core::Story>("story.2")->paras[0].text == core::kMarkPage);
        check("not a step: nothing to undo", !j.can_undo());
        check("the next frame gets a fresh id", d.fresh_id("frame") == "frame.10");
        std::string err;
        core::Doc e;
        check("round-trips through JSON", core::from_json(e, core::to_json(d), &err) && e.records() == d.records(), err);
        n.facing = false;
        n.text_frames = false;
        n.folios = false;
        core::Doc u;
        core::new_document(u, n);
        check("unfaced: no sides, no frames", u.side_of("page.2") != core::Side::Left && u.ids_of<core::Frame>().empty() &&
                                              !u.as<core::Story>("story.1"));
        const auto o = core::project(d, "");
        check("the projection shows it", o.size() > 0);
    }

    {
        std::cout << "\n-- page thumbnails --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        L::FlowCache flows;
        const auto k5 = L::page_key(d, flows, "page.5");
        const auto k16 = L::page_key(d, flows, "page.16");
        check("key: a page with text carries its lines",
              std::any_of(k5.lines.begin(), k5.lines.end(), [](const std::string& l) { return l.find("Chapter One") != std::string::npos; }));
        check("key: the same page, the same key", L::page_key(d, flows, "page.5") == k5);
        // A change on page 16 (sources) leaves page 1 of the body alone.
        core::doc::edit_frames(j, d, {"frame.10"}, "Gutter", [](core::Frame& f) { f.gutter = 20; });
        check("key: another page's change leaves it", L::page_key(d, flows, "page.5") == k5 &&
                                                      !(L::page_key(d, flows, "page.16") == k16));
        // Typing in the chapter changes the page it's set on.
        core::doc::edit_text(j, d, "story.1", {1, 0}, {1, 0}, "Then ", "Typing", "");
        check("key: its text changes it", !(L::page_key(d, flows, "page.5") == k5));
        // The master's items are part of every page that shows it.
        const auto k2 = L::page_key(d, flows, "page.6");
        core::doc::edit_masters(j, d, {"master.2"}, "Columns", [](core::Master& m) { m.columns = 2; });
        check("key: a master change reaches its pages", !(L::page_key(d, flows, "page.6") == k2));
        // Drawn: ink where the text is, a soft plate where a picture is missing.
        auto* surf = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 432, 648);
        cairo_t* cr = cairo_create(surf);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_paint(cr);
        std::set<std::string> missing;
        L::draw_page(cr, d, "page.5", flows, {}, &missing, {.placeholders = true});
        cairo_destroy(cr);
        cairo_surface_flush(surf);
        const auto px = [&](int x, int y) {
            const auto* row = cairo_image_surface_get_data(surf) + y * cairo_image_surface_get_stride(surf);
            return reinterpret_cast<const std::uint32_t*>(row)[x] & 0xffffff;
        };
        int ink = 0;
        for (int y = 180; y < 260; ++y) for (int x = 54; x < 378; ++x) ink += px(x, y) != 0xffffff;
        const auto* pic = d.as<core::Frame>("frame.4");
        check("drawn: the text inks the page", ink > 200);
        check("drawn: a missing picture as a plate", px(static_cast<int>(pic->x + pic->w / 2), static_cast<int>(pic->y + 5)) != 0xffffff &&
                                                     missing.count("harbor.psd") == 1);
        cairo_surface_destroy(surf);
    }

    {
        std::cout << "\n-- tints + opacity --\n";
        namespace L = composix::layout;
        namespace fs = std::filesystem;
        check("tint: half of black is mid grey, 100% is itself", core::tint_rgb(0x000000, 0.5) == 0x808080 &&
                                                                core::tint_rgb(0xb5512b, 1) == 0xb5512b &&
                                                                core::tint_rgb(0xb5512b, 0) == 0xffffff);
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        // A Black box at 40% on page 2, and the caption at half opacity.
        const auto box = core::doc::add_frame(j, d, "page.6", core::FrameKind::Shape, {54, 54, 100, 50});
        core::doc::edit_frames(j, d, {box}, "Fill", [](core::Frame& f) { f.fill = "swatch.1"; f.fill_tint = 0.4; f.stroke.clear(); });
        const auto items = core::frame_items(d, {"frame.6"}, "");
        bool ft = false, st = false, op = false;
        for (const auto& f : items.front().fields) {
            ft = ft || (f.edit == "fill_tint" && f.value == "15%");
            st = st || (f.edit == "stroke_tint" && f.value == "100%");
            op = op || (f.edit == "opacity" && f.value == "100%");
        }
        check("inspector: Fill tint, Stroke tint, Opacity", ft && st && op);
        const L::ImageSource none = [](const core::Asset&, double, double) -> cairo_surface_t* { return nullptr; };
        const auto out = (fs::temp_directory_path() / "composix_selftest_tint.pdf").string();
        L::PdfOptions opt;
        opt.compress = false;
        std::string err;
        const auto read = [&] { std::ifstream f(out, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); };
        check("print: ok", L::export_print_pdf(d, out, opt, none, nullptr, &err), err);
        auto pdf = read();
        check("print: a process tint is less ink (40% K)", pdf.find("0 0 0 0.4 k\n") != std::string::npos);
        check("print: a spot tint prints on the ink's plate (15%)", pdf.find(" cs 0.15 scn") != std::string::npos);
        check("print: no transparency when nothing is see-through", pdf.find("/ExtGState") == std::string::npos);
        core::doc::edit_frames(j, d, {"frame.6"}, "Opacity", [](core::Frame& f) { f.opacity = 0.5; });
        check("print: opacity as an ExtGState", L::export_print_pdf(d, out, opt, none, nullptr, &err) &&
                                                (pdf = read(), pdf.find("/ca 0.5 /CA 0.5") != std::string::npos));
        const char* gracol = "/usr/share/color/icc/GRACoL2013_CRPC6.icc";
        if (fs::exists(gracol)) {
            opt.profile = gracol;
            opt.standard = L::plate_standard::X1a;
            check("x1a: opacity refused, the frame named",
                  !L::export_print_pdf(d, out, opt, none, nullptr, &err) && err.find("page 7 at 50% opacity") != std::string::npos, err);
            opt.standard = L::plate_standard::X4;
            check("x4: opacity kept", L::export_print_pdf(d, out, opt, none, nullptr, &err), err);
        }
        std::string json = core::to_json(d);
        core::Doc e;
        check("json: tints + opacity round-trip", core::from_json(e, json, &err) && e.records() == d.records() &&
                                                  e.as<core::Frame>("frame.6")->opacity == 0.5);
    }

    {
        std::cout << "\n-- find / change --\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        load_plain(d);
        const auto any = D::find_all(d, {"the"});
        const auto cased = D::find_all(d, {"The", true});
        check("find: in reading order, the chapter first", !any.empty() && any.front().story == "story.1" &&
                                                           any.front().a == core::TextPos{1, 0} && any.front().b == core::TextPos{1, 3});
        check("find: match case finds fewer", !cased.empty() && cased.size() < any.size());
        check("find: whole word", !D::find_all(d, {"harb"}).empty() && D::find_all(d, {"harb", false, true}).empty() &&
                                  !D::find_all(d, {"harbor", false, true}).empty());
        check("find: a paragraph style alone -- each paragraph in it", D::find_all(d, {"", false, false, "style.2"}).size() == 2);
        check("find: a character style alone -- each run of it", D::find_all(d, {"", false, false, "", "style.5"}).size() == 1);
        check("find: text wholly in a character style", D::find_all(d, {"almanac", false, false, "", "style.5"}).size() == 1 &&
                                                      D::find_all(d, {"harbor", false, false, "", "style.5"}).empty());
        check("find: nothing asked, nothing found", D::find_all(d, {}).empty());
        // Change All: one step, every one; undo puts them all back.
        const auto before = *d.as<core::Story>("story.1");
        const auto steps = j.size();
        const int n = D::change_hits(j, d, any, {true, "a"}, "Change All");
        check("change all: every hit, one step", n == static_cast<int>(any.size()) && j.size() == steps + 1 &&
                                                 D::find_all(d, {"the", false, true}).empty());
        check("change all: later hits kept their places", d.as<core::Story>("story.1")->paras[1].text.rfind("a harbor smelled", 0) == 0);
        j.undo();
        check("change all: undo, all back", *d.as<core::Story>("story.1") == before);
        // Change one, applying a character style to what's put there.
        const auto h = D::find_all(d, {"diesel"});
        check("change: one, styled", D::change_hits(j, d, h, {true, "coal", "", "style.5"}, "Change") == 1 &&
                                     core::char_style_at(d.as<core::Story>("story.1")->paras[1], h.front().a.byte + 1) == "style.5" &&
                                     d.as<core::Story>("story.1")->paras[1].text.find("coal") != std::string::npos);
        // Style only: a paragraph style on every heading's paragraph.
        check("change: style only, text kept", D::change_hits(j, d, D::find_all(d, {"chapter", false, false, "style.2"}), {false, "", "style.6"}, "Change All") == 2 &&
                                               d.as<core::Story>("story.1")->paras[0].style == "style.6" &&
                                               d.as<core::Story>("story.1")->paras[0].text == "Chapter One");
        const auto at = j.size();
        check("change: to the same thing, no step", D::change_hits(j, d, D::find_all(d, {"coal"}), {true, "coal"}, "Change") == 0 &&
                                                    j.size() == at);
    }

    {
        std::cout << "\n-- find by formatting (s051) --\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        load_plain(d);
        const std::string S = "story.1";
        const auto& p1 = [&]() -> const core::Para& { return d.as<core::Story>(S)->paras[1]; };
        bool ascii = p1().text.size() > 40 && p1().runs.empty();
        for (int i = 0; ascii && i < 40; ++i) ascii = static_cast<unsigned char>(p1().text[static_cast<std::size_t>(i)]) < 0x80;
        check("find fmt: fixture -- a plain opening paragraph", ascii);
        // [0,5) italic by hand, [5,10) bold italic by hand, [20,25) bold by hand, [30,35) in Emphasis.
        D::set_local(j, d, S, {1, 0}, {1, 5}, "i", [](core::Local& l) { l.italic = 1; });
        D::set_local(j, d, S, {1, 5}, {1, 10}, "bi", [](core::Local& l) { l.italic = 1; l.bold = 1; });
        D::set_local(j, d, S, {1, 20}, {1, 25}, "b", [](core::Local& l) { l.bold = 1; });
        D::set_char_style(j, d, S, {1, 30}, {1, 35}, "style.5");
        const auto in1 = [&](const std::vector<core::doc::Hit>& hs) {
            std::vector<std::pair<int, int>> out;
            for (const auto& h : hs) if (h.story == S && h.a.para == 1) out.push_back({h.a.byte, h.b.byte});
            return out;
        };
        using V = std::vector<std::pair<int, int>>;
        check("find fmt: italic -- by hand (side by side, joined) and by character style",
              in1(D::find_all(d, D::FindOpts{.italic = true})) == V{{0, 10}, {30, 35}});
        check("find fmt: bold", in1(D::find_all(d, D::FindOpts{.bold = true})) == V{{5, 10}, {20, 25}});
        check("find fmt: bold italic", in1(D::find_all(d, D::FindOpts{.bold = true, .italic = true})) == V{{5, 10}});
        check("find fmt: words already in the style to apply left out",
              in1(D::find_all(d, D::FindOpts{.italic = true, .skip_cstyle = "style.5"})) == V{{0, 10}});
        check("find fmt: in a character style and italic", in1(D::find_all(d, D::FindOpts{.cstyle = "style.5", .italic = true})) == V{{30, 35}});
        const std::string w1 = p1().text.substr(1, 3), w2 = p1().text.substr(21, 3);
        {
            bool at1 = false, at21 = false;
            for (const auto& [a, b] : in1(D::find_all(d, D::FindOpts{.what = w1, .match_case = true, .italic = true}))) at1 = at1 || a == 1;
            for (const auto& [a, b] : in1(D::find_all(d, D::FindOpts{.what = w2, .match_case = true, .italic = true}))) at21 = at21 || a == 21;
            check("find fmt: text only where it is so set", at1 && !at21);
        }
        // Change All: the hand-made italics to Emphasis -- one step; the
        // local italic goes (the style carries it), the local bold stays.
        const auto before = *d.as<core::Story>(S);
        const auto steps = j.size();
        const auto hits = D::find_all(d, D::FindOpts{.italic = true, .skip_cstyle = "style.5"});
        const int n = D::change_hits(j, d, hits, {false, "", "", "style.5"}, "Change All");
        check("change fmt: to Emphasis, one step", n == static_cast<int>(hits.size()) && n > 0 && j.size() == steps + 1);
        check("change fmt: words kept, now Emphasis", p1().text == before.paras[1].text &&
                                                     core::char_style_at(p1(), 1) == "style.5" && core::char_style_at(p1(), 6) == "style.5");
        check("change fmt: the hand-made italic folded into the style",
              core::local_at(p1(), 1).italic == -1 && core::local_at(p1(), 6).italic == -1 && core::local_at(p1(), 6).bold == 1);
        check("change fmt: nothing left to change", in1(D::find_all(d, D::FindOpts{.italic = true, .skip_cstyle = "style.5"})).empty());
        j.undo();
        check("change fmt: undo, all back", *d.as<core::Story>(S) == before);
        D::ChangeOpts keep{false, "", "", "style.5"};
        keep.fold_local = false;
        D::change_hits(j, d, D::find_all(d, D::FindOpts{.italic = true, .skip_cstyle = "style.5"}), keep, "Change All");
        check("change fmt: fold off keeps the local italic", core::local_at(p1(), 1).italic == 1 && core::char_style_at(p1(), 1) == "style.5");
    }

    {
        std::cout << "\n-- swatches: use, groups, view setting (s052) --\n";
        namespace D = core::doc;
        core::Doc d;
        load_plain(d);
        core::Swatch ink{"Test Ink", 99, core::ColorModel::Spot, {0, 0, 0, 0}, "PMS Test", 0x336699};
        core::Swatch rgb{"Test RGB", 99.5, core::ColorModel::RGB, {10, 20, 30, 0}, "", 0x0a141e};
        {
            core::Doc::Loading load(d);
            d.put("swatch.t1", ink);
            d.put("swatch.t2", rgb);
        }
        check("swatch: a new one is unused", D::swatch_uses(d, "swatch.t1") == 0 && D::swatch_uses(d, "") == 0);
        {
            core::Doc::Loading load(d);
            auto f = *d.as<core::Frame>("frame.11");
            f.fill = "swatch.t1";
            f.stroke = "swatch.t1";
            d.put("frame.11", f);
            auto st = *d.as<core::Style>("style.1");
            st.rule_below.color = "swatch.t1";
            d.put("style.1", st);
            auto story = *d.as<core::Story>("story.1");
            story.paras[1].runs = {{0, 3, "", core::Local{-1, -1, 0, "swatch.t1"}}};
            d.put("story.1", story);
        }
        check("swatch: fill, stroke, a rule and a word's colour all count", D::swatch_uses(d, "swatch.t1") == 4);
        const auto o = core::project(d, "");
        const auto field = [&](const char* id, const char* label) {
            const auto* it = o.find(id);
            if (!it) return std::string("?");
            for (const auto& f : it->fields) if (f.label == label) return f.value;
            return std::string("-");
        };
        check("swatch: the tile says where it's used", field("swatch.t1", "Used") == "4 places" && field("swatch.t2", "Used") == "Unused");
        check("swatch: grouped by kind -- spot inks, RGB apart",
              o.find("swatch.t1") && o.find("swatch.t1")->group == "Spot inks" &&
              o.find("swatch.t2") && o.find("swatch.t2")->group.rfind("RGB", 0) == 0);
        bool all_grouped = true;
        for (const auto* it : o.in(core::Segment::Colors)) all_grouped = all_grouped && !it->group.empty();
        check("swatch: every colour has a group", all_grouped);
        const auto dir = std::filesystem::temp_directory_path() / "composix_s052";
        const auto file = (dir / "settings.json").string();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        check("settings: first run is empty", core::load_settings(file).empty());
        core::AppSettings a{{"colors.view", "list"}, {"other", "x"}};
        check("settings: round-trip", core::save_settings(file, a) && core::load_settings(file) == a);
        { std::ofstream bad(file); bad << "{not json"; }
        check("settings: unreadable -> empty, no throw", core::load_settings(file).empty());
        std::filesystem::remove_all(dir, ec);
    }

    {
        std::cout << "\n-- colour report (s053) --\n";
        namespace L = composix::layout;
        check("shift: words by Delta E", L::shift_of(1) == L::Shift::Holds && L::shift_of(3) == L::Shift::Slight &&
                                         L::shift_of(7) == L::Shift::Noticeable && L::shift_of(12) == L::Shift::Large);
        {
            plate::Proof bad;
            check("proof: no such file -> not ok", !bad.load("/nonexistent/x.icc") && !bad.ok());
        }
        // Any CMYK press profile on this computer (Fedora, Ubuntu, TeX Live's).
        std::string icc;
        for (const auto& p : plate::find_cmyk_profiles({"/usr/share/color/icc", "/usr/share/ghostscript/iccprofiles",
                                                        "/usr/share/texlive/texmf-dist/tex/generic/colorprofiles",
                                                        "/usr/share/texmf/tex/generic/colorprofiles"}))
            if (icc.empty()) icc = p.path;
        if (icc.empty()) {
            std::cout << "  (skip colour report: no CMYK profile installed)\n";
        } else {
            std::cout << "  (profile: " << icc << ")\n";
            plate::Proof pr;
            check("proof: a press profile loads, named", pr.load(icc) && pr.ok() && !pr.name().empty());
            const auto gray = pr.check(128, 128, 128), blue = pr.check(0, 0, 255);
            check("proof: a mid grey survives (< 5)", gray.delta_e < 5);
            check("proof: sRGB's pure blue is out of any press's gamut (>= 10)", blue.delta_e >= 10);
            check("proof: CMYK in range, blue mostly C + M", blue.cmyk[0] > 50 && blue.cmyk[1] > 50 && blue.cmyk[2] < 30 &&
                                                              blue.cmyk[0] <= 100 && blue.cmyk[3] <= 100);
            std::vector<std::uint8_t> px{0, 0, 255, 128, 128, 128};
            std::vector<float> de;
            pr.proof_pixels(px.data(), 2, &de);
            check("proof: pixels proofed in place, as check says", de.size() == 2 && px[0] == blue.after[0] &&
                                                                   px[2] == blue.after[2] && px[3] == gray.after[0] &&
                                                                   std::abs(de[0] - blue.delta_e) < 0.01);

            core::Doc d;
            load_plain(d);
            {   // Sky made pure blue and used; a placed RGB picture.
                core::Doc::Loading load(d);
                auto sky = *d.as<core::Swatch>("swatch.4");
                sky.v = {0, 0, 255, 0};
                d.put("swatch.4", sky);
                d.put("asset.t", core::Asset{"blue.png", 99, "/nowhere/blue.png", false, "RGB", 4, 4});
                core::Frame f;
                f.owner = d.pages().back();
                f.kind = core::FrameKind::Image;
                f.asset = "asset.t";
                f.w = f.h = 72;
                d.put("frame.t", f);
                auto cap = *d.as<core::Frame>("frame.11");
                cap.fill = "swatch.4";
                d.put("frame.11", cap);
            }
            const auto blue_px = [](const core::Asset&, int& w, int& h, std::vector<std::uint8_t>& rgb) {
                w = h = 4;
                rgb.assign(48, 0);
                for (int i = 0; i < 16; ++i) rgb[static_cast<std::size_t>(i) * 3 + 2] = 255;
                return true;
            };
            const auto rep = L::color_report(d, icc, false, blue_px);
            const auto row = [&](const std::string& id) -> const L::ColorRow* {
                for (const auto& r : rep.rows) if (r.id == id) return &r;
                return nullptr;
            };
            check("report: profile read, named", rep.ok && rep.profile_name == pr.name());
            const auto* sky = row("swatch.4");
            check("report: the RGB swatch, used, before and after", sky && sky->kind == L::ColorRow::Kind::Swatch && sky->used &&
                                                                   sky->before == 0x0000ff && sky->after != sky->before &&
                                                                   sky->shift == L::Shift::Large);
            check("report: CMYK and spot swatches left out", !row("swatch.1") && !row("swatch.3"));
            const auto* pic = row("asset.t");
            check("report: the placed RGB picture, where it is", pic && pic->kind == L::ColorRow::Kind::Image &&
                                                                 pic->page == d.pages().back() && pic->readable);
            check("report: the picture judged on its pixels", pic && pic->w == 4 && pic->h == 4 && pic->share == 1 &&
                                                              pic->shift == L::Shift::Large && pic->px_after.size() == 48 &&
                                                              pic->px_after != pic->px_before);
            check("report: the worst counted", rep.worst_count >= 2);
            const auto unread = L::color_report(d, icc, false, [](const core::Asset&, int&, int&, std::vector<std::uint8_t>&) { return false; });
            check("report: a picture that can't be read says so", [&] {
                for (const auto& r : unread.rows) if (r.id == "asset.t") return !r.readable;
                return false;
            }());
            check("report: a bad profile -> not ok, why", !L::color_report(d, "/nonexistent.icc", false, blue_px).ok);
            {   // s099: the fast screen path agrees with check, colour by colour (both intents).
                bool same = true;
                for (bool per : {false, true}) {
                    plate::Proof pf;
                    pf.load(icc, per);
                    const std::uint8_t cs[][3] = {{0, 0, 255}, {255, 0, 0}, {30, 200, 90}, {255, 255, 255}, {12, 12, 12}, {180, 120, 60}};
                    for (const auto& c : cs) {
                        std::uint8_t px[3] = {c[0], c[1], c[2]};
                        pf.simulate(px, 1);
                        const auto r = pf.check(c[0], c[1], c[2]);
                        for (int k = 0; k < 3; ++k) same = same && std::abs(int(px[k]) - int(r.after[k])) <= 2;
                    }
                }
                check("proof: the screen's fast path looks as check says (within 2 levels)", same);
            }
            {   // s107: the paper, and CMYK inks on screen through the press.
                plate::Proof plain, paper;
                plain.load(icc);
                paper.load(icc, false, nullptr, true);
                std::uint8_t w0[3] = {255, 255, 255}, w1[3] = {255, 255, 255};
                plain.simulate(w0, 1);
                paper.simulate(w1, 1);
                check("proof: without the paper, white stays white", w0[0] >= 250 && w0[1] >= 250 && w0[2] >= 250 && !plain.paper());
                check("proof: with the paper, white is the paper's (not screen white)",
                      paper.paper() && (w1[0] < 253 || w1[1] < 253 || w1[2] < 253) && w1[0] > 180 && w1[1] > 180 && w1[2] > 180,
                      std::to_string(w1[0]) + "," + std::to_string(w1[1]) + "," + std::to_string(w1[2]));
                const double none[4] = {0, 0, 0, 0}, k[4] = {0, 0, 0, 100}, c[4] = {100, 0, 0, 0};
                const auto wn = plain.cmyk_to_screen(none), kk = plain.cmyk_to_screen(k), cc = plain.cmyk_to_screen(c);
                check("CMYK on screen: no ink is white, K is dark, C is cyan",
                      (wn >> 16 & 0xff) >= 245 && (wn & 0xff) >= 245 && (kk >> 16 & 0xff) < 100 &&
                      (cc >> 16 & 0xff) < 120 && (cc & 0xff) > 150);
            }
            {   // s085: the intent asked for, and the proof made with it.
                const auto per = L::color_report(d, icc, true, blue_px);
                const L::ColorRow* psky = nullptr;
                for (const auto& r : per.rows) if (r.id == "swatch.4") psky = &r;
                check("report: proofed perceptually when asked, saying so", per.ok && per.perceptual && !rep.perceptual && psky &&
                                                                            psky->before == 0x0000ff);
            }
            // Preflight: converting, the shifting colour is a warning; not converting, nothing.
            L::FlowCache flows;
            L::PreflightOpts o;
            o.profile = true;
            o.profile_path = icc;
            const auto has_shift = [&] {
                for (const auto& i : L::preflight_issues(d, flows, o)) if (i.kind == "shift") return true;
                return false;
            };
            check("preflight: not converting, no shift warning", !has_shift());
            o.convert = true;
            check("preflight: converting, the out-of-gamut RGB colour warned", has_shift());
        }
    }

    {
        std::cout << "\n-- page range + bookmarks (s054) --\n";
        namespace L = composix::layout;
        namespace fs = std::filesystem;
        core::Doc d;
        core::load_sample(d);
        const auto pages = d.pages();
        const auto folios = [&](const std::vector<std::string>& v) {
            std::string out;
            for (const auto& p : v) out += (out.empty() ? "" : ",") + d.folio(p);
            return out;
        };
        std::string why;
        check("range: empty or All -- every page", core::pages_in_range(d, "").size() == pages.size() &&
                                                   core::pages_in_range(d, " all ").size() == pages.size());
        check("range: a run across the sections", folios(core::pages_in_range(d, "iii-1")) == "iii,iv,1");
        check("range: in document order, each once", folios(core::pages_in_range(d, "2, ii, 2")) == "ii,2");
        check("range: an en dash reads as a hyphen", folios(core::pages_in_range(d, "1\u20133")) == "1,2,3");
        check("range: unknown folio refused, named", core::pages_in_range(d, "99", &why).empty() && why.find("99") != std::string::npos);
        check("range: backwards refused", core::pages_in_range(d, "7-2", &why).empty() && why.find("backwards") != std::string::npos);

        L::FlowCache flows;
        const auto bm = L::bookmarks(d, flows);
        const auto find_bm = [&](const std::string& t) -> const L::Bookmark* {
            for (const auto& b : bm) if (b.title.find(t) != std::string::npos) return &b;
            return nullptr;
        };
        const auto* one = find_bm("Chapter One");
        const auto* two = find_bm("Chapter Two");
        check("bookmarks: the chapters, from the contents list's styles", one && two && one->level == 1 && two->level == 1);
        check("bookmarks: where they land, in reading order", one && two && d.folio(one->page) == "1" &&
                                                              d.folio(two->page) == "3" && one < two &&
                                                              one->y > 0 && one->y < d.as<core::Page>(one->page)->h);

        const L::ImageSource src = [](const core::Asset&, double, double) -> cairo_surface_t* { return nullptr; };
        const auto base = fs::temp_directory_path() / "composix_selftest_s054";
        std::error_code ec;
        fs::create_directories(base, ec);
        const auto slurp = [](const std::string& f) {
            std::ifstream in(f, std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            return ss.str();
        };
        const auto count = [](const std::string& hay, const std::string& n) {
            int k = 0;
            for (auto at = hay.find(n); at != std::string::npos; at = hay.find(n, at + 1)) ++k;
            return k;
        };
        L::PdfOptions opt;
        opt.compress = false;
        L::PdfReport rep;
        std::string err;
        const auto out = (base / "marks.pdf").string();
        check("print: exports with bookmarks", L::export_print_pdf(d, out, opt, src, &rep, &err), err);
        auto pdf = slurp(out);
        check("print: an outline, opened with it shown", pdf.find("/Outlines") != std::string::npos &&
                                                        pdf.find("/PageMode /UseOutlines") != std::string::npos &&
                                                        pdf.find("/Type /Outlines") != std::string::npos);
        check("print: the chapters in it, each going to its page", pdf.find("(Chapter One)") != std::string::npos &&
                                                                   pdf.find("(Chapter Two)") != std::string::npos &&
                                                                   count(pdf, "/XYZ 0 ") - count(pdf, "/Subtype /Link") == static_cast<int>(bm.size()));   // s083: less the links' own
        opt.only = core::pages_in_range(d, "1-3");
        check("print: a range -- just those pages", L::export_print_pdf(d, out, opt, src, &rep, &err) && rep.pages == 3 &&
                                                    count(slurp(out), "/Type /Page /Parent") == 3, err);
        opt.only = core::pages_in_range(d, "iv");
        L::export_print_pdf(d, out, opt, src, &rep, &err);
        check("print: no chapter in the range, no outline", slurp(out).find("/Outlines") == std::string::npos);
        opt.only.clear();
        opt.bookmarks = false;
        L::export_print_pdf(d, out, opt, src, &rep, &err);
        check("print: bookmarks off, no outline", slurp(out).find("/Outlines") == std::string::npos);
        // Nesting: level 2 under the level 1 before it.
        {
            plate::Document pd;
            pd.set_compress(false);
            pd.add_page(100, 100);
            pd.add_page(100, 100);
            pd.add_bookmark("A", 1, 0, 10);
            pd.add_bookmark("A1", 2, 0, 50);
            pd.add_bookmark("A2", 2, 1, 0);
            pd.add_bookmark("B", 1, 1, 60);
            pd.add_bookmark("Gone", 1, 9, 0);   // no such page: left out
            const auto f = (base / "nest.pdf").string();
            const bool ok = pd.save(f, &err);
            const auto p2 = slurp(f);
            check("plate: bookmarks nest by level, a missing page dropped",
                  ok && p2.find("/Count 4") != std::string::npos && p2.find("/Count 2") != std::string::npos &&
                  p2.find("(Gone)") == std::string::npos && p2.find("/XYZ 0 90 null") != std::string::npos, err);
        }
        // Screen (cairo): the outline too.
        L::PdfOptions so;
        const auto sout = (base / "screen.pdf").string();
        check("screen: exports with bookmarks", L::export_pdf(d, sout, so, src, &rep, &err), err);
        check("screen: an outline in it", slurp(sout).find("/Outlines") != std::string::npos);
        so.only = core::pages_in_range(d, "ii-iii");
        check("screen: a range -- just those pages", L::export_pdf(d, sout, so, src, &rep, &err) && rep.pages == 2, err);
        fs::remove_all(base, ec);
    }

    {
        std::cout << "\n-- column rules + breaks (s055) --\n";
        namespace L = composix::layout;
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        load_plain(d);
        // A two-column frame of its own, two short paragraphs.
        {
            core::Doc::Loading load(d);
            core::Frame f;
            f.owner = d.pages().back();
            f.kind = core::FrameKind::Text;
            f.x = 36; f.y = 36; f.w = 360; f.h = 300;
            f.columns = 2;
            f.gutter = 18;
            f.story = "story.t";
            d.put("frame.t", f);
            core::Frame g = f;
            g.y = 360; g.h = 200; g.columns = 1; g.story = "story.t";
            f.next = "frame.t2";
            d.put("frame.t", f);
            d.put("frame.t2", g);
            core::Story st;
            st.name = "Test";
            st.paras = {{"style.1", "First paragraph.", {}}, {"style.1", "Second paragraph.", {}}, {"style.1", "Third.", {}}};
            d.put("story.t", st);
        }
        const auto where = [&](int para) {   // {frame, column} of a paragraph's first line
            const auto fl = L::flow_story(d, "story.t");
            for (const auto& ft : fl.frames)
                for (const auto& ln : ft.lines)
                    if (ln.para == para && ln.start == 0) return ft.frame + ":" + std::to_string(ln.col);
            return std::string("none");
        };
        check("break: none -- all in the first column", where(1) == "frame.t:0" && where(2) == "frame.t:0");
        const auto before = *d.as<core::Story>("story.t");
        const auto steps = j.size();
        const auto c = D::insert_break(j, d, "story.t", {1, 7}, core::ParaStart::NextColumn);   // "Second |paragraph."
        const auto& st = *d.as<core::Story>("story.t");
        check("break: splits the paragraph, one step", st.paras.size() == 4 && j.size() == steps + 1 &&
                                                       st.paras[1].text == "Second " && st.paras[2].text == "paragraph." &&
                                                       c == core::TextPos{2, 0});
        check("break: the rest keeps the style, carries the break", st.paras[2].style == "style.1" &&
                                                                     st.paras[2].brk == core::ParaStart::NextColumn &&
                                                                     st.paras[1].brk == core::ParaStart::Anywhere);
        check("break: column -- what follows starts the next column", where(1) == "frame.t:0" && where(2) == "frame.t:1");
        j.undo();
        check("break: undo, all back", *d.as<core::Story>("story.t") == before);
        D::insert_break(j, d, "story.t", {2, 0}, core::ParaStart::NextFrame);
        check("break: at a paragraph's start -- no split, that one moves on", d.as<core::Story>("story.t")->paras.size() == 3 &&
                                                                               where(2) == "frame.t2:0" && where(1) == "frame.t:0");
        D::insert_break(j, d, "story.t", {2, 0}, core::ParaStart::Anywhere);
        check("break: taken off again", where(2) == "frame.t:0");
        // Typing keeps a break where it was; Enter's new paragraph has none.
        D::insert_break(j, d, "story.t", {1, 0}, core::ParaStart::NextColumn);
        D::edit_text(j, d, "story.t", {1, 6}, {1, 6}, "X");
        D::edit_text(j, d, "story.t", {1, 18}, {1, 18}, "\n");
        check("break: survives typing; Enter's paragraph has none", d.as<core::Story>("story.t")->paras[1].brk == core::ParaStart::NextColumn &&
                                                                    d.as<core::Story>("story.t")->paras[2].brk == core::ParaStart::Anywhere);
        // Column rules.
        {
            core::Doc::Loading load(d);
            auto f = *d.as<core::Frame>("frame.t");
            f.columns = 3;
            f.col_rule = {1, "swatch.3", 0.5, 6};
            f.inset = {10, 0, 4, 0};
            d.put("frame.t", f);
        }
        const auto& fr = *d.as<core::Frame>("frame.t");
        const auto rules = L::column_rules(fr);
        bool mid = rules.size() == 2;
        for (int k = 0; mid && k < 2; ++k) {
            const double gap0 = core::column_x(fr, k) + core::column_width(fr, k), gap1 = core::column_x(fr, k + 1);
            const auto& r = rules[static_cast<std::size_t>(k)].r;
            mid = std::abs(r.x + r.w / 2 - (gap0 + gap1) / 2) < 1e-6 && r.w == 1 && r.y == 16 && std::abs(r.y + r.h - (300 - 4 - 6)) < 1e-6;
        }
        check("rules: one down each gutter, inset top and bottom", mid && rules[0].color == "swatch.3" && rules[0].tint == 0.5);
        {
            auto one = fr;
            one.columns = 1;
            auto none = fr;
            none.col_rule.weight = 0;
            check("rules: none for one column, or no weight", L::column_rules(one).empty() && L::column_rules(none).empty());
        }
        // Kept in the file.
        const auto json = core::to_json(d);
        core::Doc e;
        std::string err;
        check("json: column rule and break round-trip", core::from_json(e, json, &err) && e.records() == d.records(), err);
    }

    {
        std::cout << "\n-- find: patterns + scope (s056) --\n";
        namespace D = core::doc;
        using M = D::FindOpts::Mode;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        load_plain(d);
        {
            core::Doc::Loading load(d);
            core::Story st;
            st.name = "Patterns";
            st.paras = {{"style.1", "Call 555-1234 or 555-9876.", {}},
                        {"style.1", "Caf\u00e9 cr\u00e8me, na\u00efve r\u00e9sum\u00e9.", {}},
                        {"style.1", "Chapter 12 and chapter 3.", {}}};
            d.put("story.f", st);
        }
        const auto find = [&](D::FindOpts f) { f.story = f.story.empty() ? "story.f" : f.story; return D::find_all(d, f); };
        auto h = find({.what = "^9^9^9-^9^9^9^9", .mode = M::Wildcards});
        check("wildcards: ^9 a digit", h.size() == 2 && h[0].a == core::TextPos{0, 5} && h[0].b == core::TextPos{0, 13});
        h = find({.what = "^$^$^$^$", .whole_word = true, .mode = M::Wildcards});
        check("wildcards: ^$ a letter -- accented ones too, by character",
              std::any_of(h.begin(), h.end(), [](const D::Hit& x) { return x.a == core::TextPos{1, 0} && x.b == core::TextPos{1, 5}; }) &&
              h.front().a == core::TextPos{0, 0});   // "Call" too
        h = find({.what = "Caf^?", .match_case = true, .mode = M::Wildcards});
        check("wildcards: ^? one character (two bytes here)", h.size() == 1 && h[0].b == core::TextPos{1, 5});
        check("wildcards: the rest is plain (a dot is a dot)", find({.what = "^9.", .mode = M::Wildcards}).size() == 2 &&
                                                               find({.what = "^9", .mode = M::Text}).empty());
        h = find({.what = "chapter (\\d+)", .mode = M::Regex});
        check("regex: matches, case folded, groups kept", h.size() == 2 && h[0].groups.size() == 2 && h[0].groups[1] == "12" &&
                                                           h[1].groups[1] == "3");
        check("regex: match case", find({.what = "chapter (\\d+)", .match_case = true, .mode = M::Regex}).size() == 1);
        std::string why;
        check("regex: a bad pattern says why, finds nothing",
              !D::find_pattern_ok({.what = "(abc", .mode = M::Regex}, &why) && why == "unbalanced ( )" &&
              find({.what = "(abc", .mode = M::Regex}).empty() && D::find_pattern_ok({.what = "(abc"}));
        check("regex: an empty match finds nothing", find({.what = "x*", .mode = M::Regex}).empty());
        check("groups: $1 $& $$, unknown left", D::expand_groups("$$1 $& $9 $2", {"ab", "a"}) == "$1 ab $9 $2");
        const auto steps = j.size();
        const int n = D::change_hits(j, d, h, {.with = "Ch. $1", .expand = true}, "Change All");
        check("change: groups put in, one step", n == 2 && j.size() == steps + 1 &&
                                                 d.as<core::Story>("story.f")->paras[2].text == "Ch. 12 and Ch. 3.");
        j.undo();
        // Scope.
        check("scope: one story", find({.what = "the", .story = "story.1"}).size() > 0 && find({.what = "the"}).empty());
        check("scope: every story", D::find_all(d, {.what = "555"}).size() == 2);
        h = find({.what = "^9", .mode = M::Wildcards, .in_range = true, .from = {0, 5}, .to = {0, 8}});
        check("scope: a selection -- only inside it", h.size() == 3 && h.back().b == core::TextPos{0, 8});
        h = find({.what = "chapter", .in_range = true, .from = {1, 0}, .to = {2, 7}});
        check("scope: a selection across paragraphs", h.size() == 1 && h[0].a == core::TextPos{2, 0});
        h = find({.pstyle = "style.1", .in_range = true, .from = {0, 20}, .to = {1, 4}});
        check("scope: a style alone -- each paragraph's part in it", h.size() == 2 && h[0].a == core::TextPos{0, 20} &&
                                                                     h[1].b == core::TextPos{1, 4});
    }

    {
        std::cout << "\n-- saved state (s057) --\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        load_plain(d);
        j.clear();
        const auto saved = j.mark();
        D::edit_text(j, d, "story.1", {1, 0}, {1, 0}, "A", "Typing", "k");
        const auto one = j.mark();
        check("mark: a step moves it", one != saved);
        j.undo();
        check("mark: undo back to the save -- the same mark (nothing to save)", j.mark() == saved);
        j.redo();
        check("mark: redo -- the step's own mark again", j.mark() == one);
        D::edit_text(j, d, "story.1", {1, 1}, {1, 1}, "B", "Typing", "k");   // merges into the same step
        check("mark: a merge into the last step is a new state", j.size() == 1 && j.mark() != one && j.mark() != saved);
        const auto two = j.mark();
        j.undo();
        check("mark: undo of the merged step -- back to the save", j.mark() == saved);
        j.redo();
        check("mark: and forward", j.mark() == two);
        j.clear();
        check("mark: a cleared journal is a fresh start", j.mark() != saved && j.mark() != two);
        core::AppSettings st{{"w", "1400"}, {"bad", "12px"}, {"neg", "-5"}};
        check("settings: a number, clamped; missing or not one -> the fallback",
              core::setting_int(st, "w", 1, 0, 2000) == 1400 && core::setting_int(st, "w", 1, 0, 1000) == 1000 &&
              core::setting_int(st, "bad", 7, 0, 99) == 7 && core::setting_int(st, "none", 3, 0, 9) == 3 &&
              core::setting_int(st, "neg", 3, 0, 9) == 0);
    }

    {
        std::cout << "\n-- swatch editor (s058) --\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        load_plain(d);
        using CM = core::ColorModel;
        check("preview: RGB as is, grey, CMYK by the ink model",
              D::swatch_preview({"a", 0, CM::RGB, {255, 128, 0, 0}}) == 0xff8000 &&
              D::swatch_preview({"g", 0, CM::Gray, {100, 0, 0, 0}}) == 0x000000 &&
              D::swatch_preview({"g", 0, CM::Gray, {0, 0, 0, 0}}) == 0xffffff &&
              D::swatch_preview({"c", 0, CM::CMYK, {100, 0, 0, 0}}) == 0x00ffff &&
              D::swatch_preview({"k", 0, CM::CMYK, {0, 0, 0, 100}}) == 0x000000);
        {
            core::Swatch s{"x", 0, CM::CMYK};
            D::swatch_from_rgb(s, 0xff0000);
            core::Swatch g{"y", 0, CM::Gray};
            D::swatch_from_rgb(g, 0x808080);
            check("from RGB: red is M+Y, grey is a percent", s.v[0] == 0 && s.v[1] == 100 && s.v[2] == 100 && s.v[3] == 0 &&
                                                             g.v[0] >= 49 && g.v[0] <= 50);
        }
        const auto steps = j.size();
        const auto made = D::add_swatch(j, d, {"Teal", 0, CM::CMYK, {80, 0, 30, 10}});
        const auto* t = d.as<core::Swatch>(made);
        double last = 0;
        for (const auto& id : d.ids_of<core::Swatch>()) if (id != made) last = std::max(last, d.as<core::Swatch>(id)->pos);
        check("add: one step, after the others, its preview made", t && j.size() == steps + 1 && t->pos > last &&
                                                                   t->preview == D::swatch_preview(*t) && t->name == "Teal");
        auto e = *t;
        e.v = {0, 0, 0, 50};
        e.preview = 0x123456;   // ignored: made from the values
        check("edit: one step, preview follows", D::edit_swatch(j, d, made, e) && d.as<core::Swatch>(made)->preview == 0x808080);
        check("edit: the same again -- no step", !D::edit_swatch(j, d, made, *d.as<core::Swatch>(made)));
        {   // a swatch's own preview (the sample's tuned rust) survives an edit that leaves its values.
            auto rust = *d.as<core::Swatch>("swatch.3");
            const auto was = rust.preview;
            rust.name = "Rust";
            check("edit: the name only -- its own preview kept", D::edit_swatch(j, d, "swatch.3", rust) &&
                                                                 d.as<core::Swatch>("swatch.3")->preview == was);
            j.undo();
        }
        const auto copy = D::duplicate_swatch(j, d, "swatch.3");
        check("duplicate: \"<name> copy\", same ink", d.as<core::Swatch>(copy) && d.as<core::Swatch>(copy)->name == "Guild Rust copy" &&
                                                      d.as<core::Swatch>(copy)->spot == d.as<core::Swatch>("swatch.3")->spot);
        check("delete: a used one stays", !D::delete_swatch(j, d, "swatch.3") && d.as<core::Swatch>("swatch.3"));
        check("delete: an unused one goes, one step; undo brings it back",
              D::delete_swatch(j, d, made) && !d.as<core::Swatch>(made) && (j.undo(), d.as<core::Swatch>(made) != nullptr));
        // A frame using it follows the edit (the same id, new values).
        {
            core::Doc::Loading load(d);
            auto f = *d.as<core::Frame>("frame.11");
            f.fill = made;
            d.put("frame.11", f);
        }
        auto e2 = *d.as<core::Swatch>(made);
        e2.model = CM::RGB;
        e2.v = {10, 200, 30, 0};
        D::edit_swatch(j, d, made, e2);
        check("edit: what uses it follows (same swatch)", d.as<core::Frame>("frame.11")->fill == made &&
                                                         d.as<core::Swatch>(made)->preview == 0x0ac81e);
    }

    {
        std::cout << "\n-- hidden characters (s059) --\n";
        namespace L = composix::layout;
        core::Doc d;
        load_plain(d);
        {
            core::Doc::Loading load(d);
            core::Frame f;
            f.owner = d.pages().back();
            f.kind = core::FrameKind::Text;
            f.x = 36; f.y = 36; f.w = 360; f.h = 300;
            f.columns = 2;
            f.story = "story.h";
            d.put("frame.h", f);
            core::Story st;
            st.name = "Hidden";
            st.paras = {{"style.1", "A b\tc", {}}, {"style.1", "D", {}}, {"style.1", "", {}}};
            st.paras[1].brk = core::ParaStart::NextColumn;
            d.put("story.h", st);
        }
        const auto flow = L::flow_story(d, "story.h");
        const auto marks = L::hidden_marks(d, "story.h", flow, "frame.h");
        using K = L::HiddenMark::Kind;
        const auto count = [&](K k) { return std::count_if(marks.begin(), marks.end(), [&](const L::HiddenMark& m) { return m.kind == k; }); };
        check("hidden: a space, a tab, a break, a paragraph end, the story's end",
              count(K::Space) == 1 && count(K::Tab) == 1 && count(K::Break) == 1 && count(K::Para) == 1 && count(K::End) == 1);
        const auto first = [&](K k) { return *std::find_if(marks.begin(), marks.end(), [&](const L::HiddenMark& m) { return m.kind == k; }); };
        check("hidden: each where it is -- the space before the tab, a span each",
              first(K::Space).x1 > first(K::Space).x0 && first(K::Tab).x1 > first(K::Tab).x0 && first(K::Tab).x0 >= first(K::Space).x1 &&
              first(K::Break).brk == 1 && first(K::Break).x0 >= first(K::Tab).x1);
        check("hidden: the break's paragraph in the next column -- its end further right",
              first(K::Para).x0 > static_cast<double>(core::column_x(*d.as<core::Frame>("frame.h"), 1)));
        check("hidden: none for a frame that doesn't hold the story", L::hidden_marks(d, "story.h", flow, "frame.11").empty());
    }

    {
        std::cout << "\n-- section editing (s060) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto secs = d.ids_of<core::Section>();
        check("fixture: two sections (front matter, body)", secs.size() >= 2);
        const auto body = secs.back();
        const auto first_page = d.as<core::Section>(body)->start_page;
        const auto steps = j.size();
        check("rename: trimmed, one step", D::rename_section(j, d, body, "  Part One ") && j.size() == steps + 1 &&
                                           d.as<core::Section>(body)->name == "Part One");
        check("rename: empty refused", !D::rename_section(j, d, body, "   ") && d.as<core::Section>(body)->name == "Part One");
        check("rename: Section Name markers read it", L::marker_value(d, 1, first_page, "") == "Part One");
        const auto was = d.folio(first_page);
        check("starts at: the first page takes it", D::set_section_start_at(j, d, {body}, 5) && d.folio(first_page) == "5");
        check("starts at: 0 refused", !D::set_section_start_at(j, d, {body}, 0));
        j.undo();
        check("starts at: undo", d.folio(first_page) == was);
        std::string m0;   // a master the section hasn't got
        for (const auto& m : d.ids_of<core::Master>()) if (m != d.as<core::Section>(body)->master) { m0 = m; break; }
        check("master: set, or a missing one refused", D::set_section_master(j, d, {body}, m0) &&
                                                       d.as<core::Section>(body)->master == m0 &&
                                                       !D::set_section_master(j, d, {body}, "master.none"));
        const auto o = core::project(d, "");
        const auto has_remove = [&](const std::string& id) {
            const auto* it = o.find(id);
            if (!it) return false;
            for (const auto& f : it->fields) if (f.edit == "section") return true;
            return false;
        };
        const auto name_field = [&](const std::string& id) {
            for (const auto& f : o.find(id)->fields) if (f.edit == "sname") return f.text && f.value == d.as<core::Section>(id)->name;
            return false;
        };
        check("inspector: Name typed; Remove offered except for the first section", name_field(body) && has_remove(body) &&
                                                                                     !has_remove(secs.front()));
    }

    {
        std::cout << "\n-- caret follow (s061) --\n";
        core::Viewport vp;
        vp.set_widget(800, 600);
        vp.set_content({0, 0, 5000, 5000});
        vp.set_origin(0, 0);
        check("reveal: already in view -- no move", !vp.reveal({100, 100, 4, 12}, 40) && vp.ox() == 0 && vp.oy() == 0);
        check("reveal: below -- pans up just enough", vp.reveal({100, 700, 4, 12}, 40) &&
                                                      std::abs(vp.oy() - (600 - 40 - 712)) < 1e-9 && vp.ox() == 0);
        vp.set_origin(0, 0);
        check("reveal: to the right and above", (vp.set_origin(0, -500), vp.reveal({1000, 100, 4, 12}, 40)) &&
                                                std::abs(vp.ox() - (800 - 40 - 1004)) < 1e-9 && std::abs(vp.oy() - (40 - 100)) < 1e-9);
        // Where a change ends, in the text after it.
        core::Story a, b;
        a.paras = {{"s", "The harbor", {}}, {"s", "smelled", {}}};
        b = a;
        b.paras[0].text = "The old harbor";
        check("changed end: text came back -- after it", core::changed_end(a, b) == core::TextPos{0, 8});
        check("changed end: text went -- where it was", core::changed_end(b, a) == core::TextPos{0, 4});
        b = a;
        b.paras = {{"s", "The harbor", {}}, {"s", "", {}}, {"s", "smelled", {}}};
        check("changed end: a paragraph came back -- across the break", core::changed_end(a, b) == core::TextPos{1, 0} ||
                                                                         core::changed_end(a, b) == core::TextPos{2, 0});
        check("changed end: the same -- the start", core::changed_end(a, a) == core::TextPos{0, 0});
        b = a;
        b.paras[1].text = "smelled \u00e9";
        check("changed end: never inside a character", core::changed_end(a, b) == core::TextPos{1, 10});
        // A whole paragraph selected (triple-click): every line lit across, not
        // just its first and last (a line's end read as the next line's start).
        {
            namespace L = composix::layout;
            core::Doc d;
            load_plain(d);
            {
                core::Doc::Loading load(d);
                core::Frame f;
                f.owner = d.pages().back();
                f.kind = core::FrameKind::Text;
                f.x = 36; f.y = 36; f.w = 200; f.h = 400;
                f.story = "story.s";
                d.put("frame.s", f);
                core::Story st;
                st.name = "Sel";
                st.paras = {{"style.1", "The harbor smelled of diesel and oranges the morning the ship came in, and nobody on "
                                        "the quay had expected her before the equinox and fewer still the cargo she carried.", {}}};
                d.put("story.s", st);
            }
            const auto fl = L::flow_story(d, "story.s");
            const int n = static_cast<int>(d.as<core::Story>("story.s")->paras[0].text.size());
            const auto rects = L::selection_rects(d, "story.s", fl, "frame.s", {0, 0}, {0, n});
            bool wide = rects.size() >= 3;
            for (std::size_t k = 0; wide && k + 1 < rects.size(); ++k) wide = rects[k].w > 100;
            check("selection: a paragraph's middle lines lit across", wide);
        }
    }

    {
        std::cout << "\n-- page margins + master drop (s062) --\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        std::string pg, other;   // two pages on the same master
        for (const auto& p : d.pages()) {
            if (d.as<core::Page>(p)->master.empty()) continue;
            if (pg.empty()) pg = p;
            else if (d.as<core::Page>(p)->master == d.as<core::Page>(pg)->master) { other = p; break; }
        }
        check("fixture: two pages on one master", !pg.empty() && !other.empty());
        const auto before = core::margin_rect(d, pg), other_before = core::margin_rect(d, other);
        const auto steps = j.size();
        check("own margins: one step, from the master's", D::edit_page_margins(j, d, {pg}, "Top margin", [](core::Margins& m) { m.top = 100; }) &&
                                                         j.size() == steps + 1 && d.as<core::Page>(pg)->margins &&
                                                         d.as<core::Page>(pg)->margins->bottom == core::margins_of(d, other)->bottom);
        const auto after = core::margin_rect(d, pg);
        check("own margins: this page's text area moves; its neighbour's doesn't",
              std::abs(after.y - 100) < 1e-9 && std::abs(after.h - (before.h - (100 - before.y))) < 1e-9 &&
              core::margin_rect(d, other) == other_before);
        const auto json = core::to_json(d);
        core::Doc e;
        std::string err;
        check("own margins: kept in the file", core::from_json(e, json, &err) && e.records() == d.records(), err);
        check("reset: back to the master's, one step", D::reset_page_margins(j, d, {pg}) && !d.as<core::Page>(pg)->margins &&
                                                       core::margin_rect(d, pg) == before);
        // A master dropped on pages is set_master: one step, those pages.
        std::string m2;
        for (const auto& m : d.ids_of<core::Master>()) if (m != d.as<core::Page>(pg)->master) { m2 = m; break; }
        const auto s2 = j.size();
        check("master drop: the pages take it, one step", D::set_master(j, d, {pg, other}, m2) && j.size() == s2 + 1 &&
                                                         d.as<core::Page>(pg)->master == m2 && d.as<core::Page>(other)->master == m2);
    }

    {
        std::cout << "\n-- place several (s063) --\n";
        core::PlaceQueue q;
        check("queue: empty at first", q.empty() && q.size() == 0);
        const auto file = [](const char* n) { core::ImageFile f; f.name = n; f.path = std::string("/x/") + n; return f; };
        q.load({file("a.png"), file("b.png"), file("c.png")});
        check("queue: the first is the one a click puts down", q.size() == 3 && q.current().name == "a.png" && q.next()->name == "b.png");
        q.cycle(+1);
        check("queue: the arrows turn -- next", q.current().name == "b.png" && q.size() == 3);
        q.cycle(-1);
        q.cycle(-1);
        check("queue: and back (the last to the front)", q.current().name == "c.png");
        q.placed();
        check("queue: placed -- it goes, the next comes up", q.size() == 2 && q.current().name == "a.png");
        q.placed();
        q.placed();
        check("queue: the last placed -- nothing loaded", q.empty() && !q.next());
        q.load({file("x.png")});
        q.cycle(+1);
        check("queue: one alone doesn't turn; Esc clears", q.current().name == "x.png" && (q.clear(), q.empty()));
    }

    {
        std::cout << "\n-- start screen (s064) --\n";
        namespace fs = std::filesystem;
        core::Doc d;
        core::load_sample(d);
        const auto dir = (fs::temp_directory_path() / "composix_s064" / "Sample Book").string();
        std::error_code ec;
        fs::remove_all(fs::path(dir).parent_path(), ec);
        fs::create_directories(dir, ec);
        std::string err;
        check("fixture: the sample saved", core::save_project(d, dir, &err), err);
        core::Doc kept;
        const auto sum = core::summarize_project(dir, &kept);
        const long long now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        check("summary: title, trim, pages", sum.ok && sum.title == d.settings().title && sum.pages == static_cast<int>(d.pages().size()) &&
                                             sum.trim_w == d.settings().trim_w && sum.trim_h == d.settings().trim_h);
        check("summary: saved just now, the document kept", std::llabs(now - sum.saved) < 120 && kept.pages().size() == d.pages().size());
        check("summary: no document there -- not ok", !core::summarize_project((fs::path(dir).parent_path() / "none").string()).ok);
        fs::remove_all(fs::path(dir).parent_path(), ec);
        const long long t = 1'760'000'000;
        check("ago: minutes, hours, yesterday, days", core::ago_text(t + 30, t) == "just now" && core::ago_text(t + 60, t) == "1 minute ago" &&
                                                     core::ago_text(t + 3 * 3600, t) == "3 hours ago" &&
                                                     core::ago_text(t + 30 * 3600, t) == "yesterday" &&
                                                     core::ago_text(t + 4 * 86400, t) == "4 days ago");
        const auto date = core::ago_text(t + 30 * 86400, t);
        check("ago: a month on -- the date", date.find("2025") != std::string::npos && date.find("  ") == std::string::npos, date);
    }

    {
        std::cout << "\n-- hyphen control (s065) --\n";
        namespace L = composix::layout;
        core::Doc d;
        load_plain(d);
        const std::string text =
            "Considerable international correspondence concerning extraordinary navigational responsibilities "
            "overwhelmed administrative establishments, notwithstanding unquestionably comprehensive "
            "documentation characterizing transcontinental shipping circumstances, unconditionally.";
        const auto setup = [&](int limit, int shortest, bool last, const std::string& body, double width = 110) {
            core::Doc::Loading load(d);
            auto st = *d.as<core::Style>("style.1");
            st.hyphenate = true;
            st.align = core::Align::Justify;
            st.hyph_limit = limit;
            st.hyph_shortest = shortest;
            st.hyph_last_word = last;
            st.drop_lines = 0;
            d.put("style.h", st);
            core::Frame f;
            f.owner = d.pages().back();
            f.kind = core::FrameKind::Text;
            f.x = 36; f.y = 36; f.w = width; f.h = 2000;
            f.story = "story.y";
            d.put("frame.y", f);
            core::Story sy;
            sy.name = "Hy";
            sy.paras = {{"style.h", body, {}}};
            d.put("story.y", sy);
        };
        const auto lines = [&] {
            const auto fl = L::flow_story(d, "story.y");
            return fl.frames.empty() ? std::vector<L::Line>{} : fl.frames.front().lines;
        };
        const auto max_run = [&] {
            int run = 0, best = 0, total = 0;
            for (const auto& l : lines()) { run = l.hyphen ? run + 1 : 0; best = std::max(best, run); total += l.hyphen; }
            return std::pair{best, total};
        };
        setup(0, 5, true, text);
        const auto [free_run, free_total] = max_run();
        // (How many it hyphenates is the fonts' business -- a note, not a test.)
        std::cout << "  (narrow column: " << free_total << " hyphenated lines, at most " << free_run << " in a row)\n";
        setup(2, 5, true, text);
        check("limit 2: never more than two in a row", max_run().first <= 2);
        setup(1, 5, true, text);
        check("limit 1: never two in a row", max_run().first <= 1);
        setup(0, 30, true, text);
        check("shortest word 30 letters: nothing hyphenates", max_run().second == 0);
        // The last word stays whole.
        setup(0, 5, false, text);
        const auto last_at = static_cast<int>(text.rfind(' ')) + 1;
        bool whole = true;
        for (const auto& l : lines()) whole = whole && !(l.hyphen && l.end > last_at);
        check("last word: never broken (unless asked)", whole);
        // Typed hyphens: the word breaks only there; one at its start -- never.
        const std::string shy = "\u00AD";
        const std::string word = "trans" + shy + "continental" + shy + "ism";
        setup(0, 5, true, "Unquestionably " + word + " " + word + " " + word + " notwithstanding.");
        const auto& tx = d.as<core::Story>("story.y")->paras[0].text;
        bool only_typed = true;
        int at_typed = 0;
        for (const auto& l : lines()) {
            if (!l.hyphen) continue;
            const bool after_shy = l.end >= 2 && static_cast<unsigned char>(tx[static_cast<std::size_t>(l.end - 2)]) == 0xC2;
            const bool in_word = tx.substr(0, static_cast<std::size_t>(l.end)).rfind("trans") != std::string::npos &&
                                 static_cast<int>(tx.substr(0, static_cast<std::size_t>(l.end)).rfind(' ')) <
                                     static_cast<int>(tx.substr(0, static_cast<std::size_t>(l.end)).rfind("trans"));
            if (in_word && !after_shy) only_typed = false;
            at_typed += after_shy;
        }
        // The rule, not the layout: wherever a marked word breaks, it's at a
        // typed hyphen (whether one lands at a line's end is the fonts' doing).
        check("typed hyphens: a marked word breaks only at them", only_typed, std::to_string(at_typed));
        // (a column the words fit across: too narrow and Pango breaks a word anywhere)
        setup(0, 5, true, "Unquestionably " + shy + "transcontinentalism " + shy + "transcontinentalism notwithstanding.", 240);
        const auto& tx2 = d.as<core::Story>("story.y")->paras[0].text;
        std::vector<std::pair<int, int>> spans;   // each marked word, its typed hyphen to its end
        for (std::size_t at = tx2.find(shy); at != std::string::npos; at = tx2.find(shy, at + 1))
            spans.push_back({static_cast<int>(at) + 2, static_cast<int>(tx2.find(' ', at))});
        bool never = spans.size() == 2;
        for (const auto& l : lines())
            for (const auto& [s0, s1] : spans) never = never && !(l.end > s0 && l.end < s1);
        if (!never) for (const auto& l : lines()) std::cout << "    line " << l.start << ".." << l.end << (l.hyphen ? " -" : "") << "\n";
        check("typed hyphen at a word's start: that word never breaks", never);
    }

    {
        std::cout << "\n-- document settings (s066) --\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto st0 = d.settings();
        check("title: trimmed, one step; empty refused", D::set_title(j, d, "  The Outer Ports ") && d.settings().title == "The Outer Ports" &&
                                                         !D::set_title(j, d, "  "));
        // Trim: pages at the trim follow, a foldout in proportion; one undo.
        std::string plain, fold;
        for (const auto& p : d.pages()) {
            const auto* pg = d.as<core::Page>(p);
            if (pg->folds.empty() && plain.empty()) plain = p;
            if (!pg->folds.empty() && fold.empty()) fold = p;
        }
        const auto fold0 = *d.as<core::Page>(fold);
        const auto steps = j.size();
        const double W = 5.5 * core::kPt, H = 8.5 * core::kPt;
        check("trim: one step", D::set_trim(j, d, W, H) && j.size() == steps + 1);
        check("trim: a page takes it", std::abs(d.as<core::Page>(plain)->w - W) < 1e-6 && std::abs(d.as<core::Page>(plain)->h - H) < 1e-6);
        const auto* f1 = d.as<core::Page>(fold);
        check("trim: a foldout keeps its panels, in proportion",
              std::abs(f1->w - fold0.w * W / st0.trim_w) < 1e-6 && f1->folds.size() == fold0.folds.size() &&
              std::abs(f1->folds[0] - fold0.folds[0] * W / st0.trim_w) < 1e-6 && std::abs(f1->h - H) < 1e-6);
        bool masters = true;
        for (const auto& m : d.ids_of<core::Master>()) masters = masters && std::abs(d.as<core::Master>(m)->h - H) < 1e-6;
        check("trim: the masters too", masters);
        check("trim: too small refused", !D::set_trim(j, d, 10, 10));
        j.undo();
        check("trim: undo, every page back", d.as<core::Page>(fold)->w == fold0.w && d.settings().trim_w == st0.trim_w);
        check("bleed and facing", D::set_bleed(j, d, 18) && d.settings().bleed == 18 && !D::set_bleed(j, d, -1) &&
                                  D::set_facing(j, d, !st0.facing) && d.settings().facing == !st0.facing);
        const auto o = core::project(d, "");
        const auto* it = o.find("doc");
        check("inspector: the document's settings in Document", it && it->segment == core::Segment::Document && !it->fields.empty() &&
                                                               it->fields[0].text && it->fields[0].value == "The Outer Ports");
    }

    {
        std::cout << "\n-- break marks (s098) --\n";
        namespace L = composix::layout;
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        D::insert_break(j, d, "story.1", {2, 0}, core::ParaStart::NextPage);
        L::FlowCache flows;
        const auto& flow = flows.get(d, "story.1");
        const L::HiddenMark* brk = nullptr;
        std::vector<L::HiddenMark> all;
        for (const auto& fid : d.ids_of<core::Frame>())
            if (d.as<core::Frame>(fid)->story == "story.1")
                for (const auto& m : L::hidden_marks(d, "story.1", flow, fid)) all.push_back(m);
        for (const auto& m : all) if (m.kind == L::HiddenMark::Kind::Break) brk = &m;
        const auto* f1 = d.as<core::Frame>("frame.1");
        check("mark: the page break, its rule to the column's edge", brk && brk->brk == 3 && brk->edge > brk->x0 &&
                                                                     std::abs(brk->edge - core::column_width(*f1, 0) - core::column_x(*f1, 0)) < 0.01);
        check("names", std::string(L::break_name(1)) == "Column break" && std::string(L::break_name(3)) == "Page break" &&
                       std::string(L::break_name(4)) == "Right-page break");
    }

    {
        std::cout << "\n-- unused asset files (s097) --\n";
        namespace fs = std::filesystem;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto dir = (fs::temp_directory_path() / "composix_selftest_s097").string();
        std::error_code ec;
        fs::remove_all(dir, ec);
        std::string err;
        core::save_project(d, dir, &err);
        const auto put = [&](const std::string& name, std::size_t n) {
            std::ofstream o(fs::path(dir) / "assets" / name, std::ios::binary);
            o << std::string(n, 'x');
        };
        put("guild-map.tif", 10);      // the sample's embedded files
        put("fleuron.svg", 10);
        put("old-scan.png", 2000);     // nothing uses these
        put("draft.jpg", 3000);
        put("undone.png", 100);        // an asset only undo could bring back
        fs::create_directories(fs::path(dir) / "assets" / "sub", ec);   // a folder: left alone
        const auto held = std::vector<std::string>{"assets/undone.png"};
        const auto u = core::unused_asset_files(d, dir, held);
        check("unused: what nothing uses, sorted, with sizes", u.size() == 2 && fs::path(u[0].path).filename() == "draft.jpg" &&
                                                              u[0].bytes == 3000 && fs::path(u[1].path).filename() == "old-scan.png");
        check("unused: without the undo history, the undone one too", core::unused_asset_files(d, dir).size() == 3);
        check("unused: an unsaved document -- none", core::unused_asset_files(d, "").empty());
        // The journal: what undo could bring back.
        core::doc::embed_assets(j, d, {{"asset.2", "assets/harbor-copy.psd"}});
        j.undo();
        std::vector<std::string> paths;
        j.each_value([&](const core::Record& r) { if (const auto* a = std::get_if<core::Asset>(&r)) paths.push_back(a->path); });
        check("journal: the undone embed's path is still held", std::find(paths.begin(), paths.end(), "assets/harbor-copy.psd") != paths.end());
        check("bytes: in words", core::bytes_text(512) == "512 bytes" && core::bytes_text(12 * 1024) == "12 KB" &&
                                 core::bytes_text(2516582) == "2.4 MB");
        fs::remove_all(dir, ec);
    }

    {
        std::cout << "\n-- small fixes (s096) --\n";
        namespace fs = std::filesystem;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        core::doc::set_units(j, d, core::Units::Millimetres);
        const auto dir = (fs::temp_directory_path() / "composix_selftest_s096").string();
        std::error_code ec;
        fs::remove_all(dir, ec);
        std::string err;
        const bool saved = core::save_project(d, dir, &err);
        const auto sum = core::summarize_project(dir);
        check("start screen: a card knows its document's units", saved && sum.ok && sum.units == core::Units::Millimetres &&
                                                                core::size_text(sum.trim_w, sum.trim_h, 0, sum.units).find("mm") != std::string::npos, err);
        fs::remove_all(dir, ec);
        // A level's picked entry style, not yet written: stale.
        L::build_list(j, d, "list.1");
        const auto why = [&]() -> std::string {
            L::FlowCache f;
            const auto v = L::stale_lists(d, f);
            return v.empty() ? std::string("current") : v.front().why;
        };
        check("stale: built -- current", why() == "current");
        core::doc::set_list_entry_style(j, d, "list.1", 1, "style.3");
        check("stale: a picked entry style not yet written", why() == "Entries not in Caption");
        L::build_list(j, d, "list.1");
        check("stale: rebuilt in it -- current", why() == "current");
    }

    {
        std::cout << "\n-- find across paragraphs (s095) --\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        {   // three short paragraphs to search
            core::Doc::Loading load(d);
            auto st = *d.as<core::Story>("story.3");
            st.paras = {{"style.1", "alpha end"}, {"style.1", "Begin beta end"}, {"style.4", "begin gamma"}};
            d.put("story.3", st);
        }
        const auto find = [&](const std::string& what, D::FindOpts::Mode m = D::FindOpts::Mode::Text) {
            D::FindOpts f;
            f.what = what;
            f.mode = m;
            f.story = "story.3";
            return D::find_all(d, f);
        };
        auto h = find("end^pbegin");
        check("text ^p: over each break, case folded", h.size() == 2 && h[0].a == core::TextPos{0, 6} && h[0].b == core::TextPos{1, 5} &&
                                                       h[1].a == core::TextPos{1, 11} && h[1].b == core::TextPos{2, 5});
        check("wildcards ^p: three letters, the break, a b (either case)", find("^$^$^$^pB", D::FindOpts::Mode::Wildcards).size() == 2);
        h = find("(\\w+)\\n(\\w+)", D::FindOpts::Mode::Regex);
        check("regex \\n: across, groups as written", h.size() == 2 && h[0].groups.size() == 3 && h[0].groups[1] == "end" &&
                                                     h[0].groups[2] == "Begin");
        {
            D::FindOpts f;
            f.what = "end^pbegin";
            f.story = "story.3";
            f.pstyle = "style.4";
            check("a paragraph style: the one the hit starts in", D::find_all(d, f).empty());
        }
        // Change across: join the paragraphs, or break one.
        D::ChangeOpts c;
        c.with = "end / begin";
        const auto n = D::change_hits(j, d, find("end^pbegin"), c, "Change All");
        const auto& ps = d.as<core::Story>("story.3")->paras;
        check("change: the breaks taken out, one step", n == 2 && ps.size() == 1 && ps[0].text == "alpha end / begin beta end / begin gamma");
        c.with = "^p";
        D::change_hits(j, d, find(" / "), c, "Change All");
        check("change to ^p: broken back into paragraphs", d.as<core::Story>("story.3")->paras.size() == 3);
        check("plain text without ^p: within a paragraph, as before", find("beta").size() == 1);
    }

    {
        std::cout << "\n-- a paragraph's keeps, hyphenation (s094) --\n";
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto body = *d.as<core::Style>("style.1");
        check("set: keep with next, keep lines, hyphenate -- one step",
              core::doc::set_para_local(j, d, "story.1", {3, 0}, {3, 0}, "Keeps", [&](core::ParaLocal& o) {
                  o.keep_next = !body.keep_next; o.keep_lines = -1; o.hyphenate = !body.hyphenate; }));
        const auto& p3 = d.as<core::Story>("story.1")->paras.at(3);
        const auto e = core::doc::para_style(d, p3);
        check("para_style: its own keeps and hyphenation", e.keep_next == !body.keep_next && e.keep_lines == -1 && e.hyphenate == !body.hyphenate);
        const auto f = core::text_fields(d, "story.1", {3, 0}, {3, 0});
        std::string kl, kl_own, hy_own = "?";
        for (const auto& x : f) {
            if (x.edit == "pkeep_lines") { kl = x.value; kl_own = x.own; }
            if (x.edit == "phyphenate") hy_own = x.own;
        }
        check("inspector: Keep lines All together, its own; Hyphenate its own", kl == "All together" && kl_own == "para.keep_lines" &&
                                                                               hy_own == "para.hyphenate");
        check("clear: keep lines back to the style's", core::doc::clear_para_local(j, d, "story.1", {3, 0}, {3, 0}, {"keep_lines"}) &&
                                                      !d.as<core::Story>("story.1")->paras.at(3).over.keep_lines &&
                                                      d.as<core::Story>("story.1")->paras.at(3).over.keep_next.has_value());
        std::string err;
        core::Doc g;
        check("json: keeps and hyphenation kept", core::from_json(g, core::to_json(d), &err) &&
                                                 g.as<core::Story>("story.1")->paras.at(3).over == d.as<core::Story>("story.1")->paras.at(3).over, err);
    }

    {
        std::cout << "\n-- a paragraph's own settings (s093) --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto& ded = d.as<core::Story>("story.3")->paras.at(0);
        check("sample: the dedication -- Body, centred on its own", ded.style == "style.1" && ded.over.align == core::Align::Center &&
                                                                    core::doc::para_style(d, ded).align == core::Align::Center &&
                                                                    d.as<core::Style>("style.1")->align != core::Align::Center);
        const auto x_of = [&](int para) {
            L::FlowCache f;
            return L::caret_at(d, "story.1", f.get(d, "story.1"), {para, 0}).x;
        };
        const double x0 = x_of(3);
        const auto n0 = j.size();
        check("set: one step over a range", core::doc::set_para_local(j, d, "story.1", {3, 0}, {4, 0}, "Left indent",
                                                                      [](core::ParaLocal& o) { o.left_indent = 36; }) &&
                                            j.size() == n0 + 1 && d.as<core::Story>("story.1")->paras.at(4).over.left_indent == 36.0);
        check("set: the same again -- no step", !core::doc::set_para_local(j, d, "story.1", {3, 0}, {4, 0}, "Left indent",
                                                                           [](core::ParaLocal& o) { o.left_indent = 36; }));
        check("layout: the paragraph set in 36 pt", std::abs(x_of(3) - x0 - 36) < 0.5);
        // Enter in the middle: both halves keep it; Enter at the end: the next paragraph (Next style) doesn't.
        core::doc::edit_text(j, d, "story.1", {3, 4}, {3, 4}, "\n");
        const auto& ps = d.as<core::Story>("story.1")->paras;
        check("Enter mid-paragraph: both halves keep their own settings", ps.at(3).over.left_indent == 36.0 && ps.at(4).over.left_indent == 36.0);
        const int e = static_cast<int>(ps.at(4).text.size());
        core::doc::edit_text(j, d, "story.1", {4, e}, {4, e}, "\n");
        check("Enter at the end: a fresh paragraph, its style's", d.as<core::Story>("story.1")->paras.at(5).over.empty());
        check("clear: one setting back to the style's", core::doc::clear_para_local(j, d, "story.1", {3, 0}, {3, 0}, {"left_indent"}) &&
                                                       !d.as<core::Story>("story.1")->paras.at(3).over.left_indent &&
                                                       d.as<core::Story>("story.1")->paras.at(4).over.left_indent == 36.0);
        check("clear: nothing set -- no step", !core::doc::clear_para_local(j, d, "story.1", {3, 0}, {3, 0}, {"left_indent"}));
        // The inspector while typing: effective values, the own ones marked.
        {
            const auto f = core::text_fields(d, "story.3", {0, 0}, {0, 0});
            std::string align_own, align_val, lind_own = "?";
            for (const auto& x : f) {
                if (x.edit == "palign") { align_own = x.own; align_val = x.value; }
                if (x.edit == "plindent") lind_own = x.own;
            }
            check("inspector: Align Center, its own; Left indent the style's", align_val == "Center" && align_own == "para.align" &&
                                                                              lind_own.empty());
        }
        std::string err;
        core::Doc g;
        check("json: a paragraph's own settings kept", core::from_json(g, core::to_json(d), &err) &&
                                                      g.as<core::Story>("story.1")->paras.at(4).over == d.as<core::Story>("story.1")->paras.at(4).over &&
                                                      g.as<core::Story>("story.3")->paras.at(0).over.align == core::Align::Center, err);
    }

    {
        std::cout << "\n-- own settings, row by row (s092) --\n";
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto rows = [&](const std::string& id) {
            std::map<std::string, std::string> m;   // label -> own key
            const auto o = core::project(d, "");
            for (const auto* it : o.in(core::Segment::Styles))
                if (it->id == id)
                    for (const auto& f : it->fields) if (!f.own.empty()) m[f.label] = f.own;
            return m;
        };
        auto m = rows("style.10");
        check("rows: Body First's own -- First indent and Drop cap, marked", m.size() == 2 && m["First indent"] == "first_indent" &&
                                                                            m["Drop cap"] == "drop_lines");
        check("rows: a style on no base -- nothing marked", rows("style.1").empty());
        check("edit keys -> style keys", core::doc::style_key_of_edit("ssize") == "size" && core::doc::style_key_of_edit("rule_a_w") == "rule_above" &&
                                         core::doc::style_key_of_edit("list_hang") == "list" && core::doc::style_key_of_edit("pstart") == "start" &&
                                         core::doc::style_key_of_edit("zoom").empty());
        const auto base = *d.as<core::Style>("style.1");
        const auto n0 = j.size();
        check("reset one: one step", core::doc::reset_style_fields(j, d, {"style.10"}, {"drop_lines"}) && j.size() == n0 + 1);
        const auto& bf = *d.as<core::Style>("style.10");
        check("reset one: the base's value, no longer its own; the other kept", bf.drop_lines == base.drop_lines &&
              bf.own == std::vector<std::string>{"first_indent"});
        check("reset one: not owned -- no step", !core::doc::reset_style_fields(j, d, {"style.10"}, {"drop_lines"}) &&
                                                 !core::doc::reset_style_fields(j, d, {"style.1"}, {"size"}));
        m = rows("style.10");
        check("rows: the reset row unmarked", m.size() == 1 && m.count("First indent") == 1);
        j.undo();
        check("reset one: undone", d.as<core::Style>("style.10")->own.size() == 2 && d.as<core::Style>("style.10")->drop_lines >= 2);
        // Several picked: marked only where every one owns it.
        {
            const auto o = core::project(d, "");
            std::vector<const core::Item*> sel;
            for (const auto* it : o.in(core::Segment::Styles)) if (it->id == "style.10" || it->id == "style.1") sel.push_back(it);
            bool any = false;
            for (const auto& f : core::shared_fields(sel)) any = any || !f.own.empty();
            check("several picked: not marked unless all own it", sel.size() == 2 && !any);
        }
    }

    {
        std::cout << "\n-- find: Unicode case (s091) --\n";
        namespace D = core::doc;
        check("fold: Latin-1, Extended-A, Greek, Cyrillic, Armenian, fullwidth",
              D::fold_char(U'É') == U'é' && D::fold_char(U'Ž') == U'ž' && D::fold_char(U'Ł') == U'ł' && D::fold_char(U'Σ') == U'σ' &&
              D::fold_char(U'ς') == U'σ' && D::fold_char(U'Ά') == U'ά' && D::fold_char(U'Ж') == U'ж' && D::fold_char(U'Ё') == U'ё' &&
              D::fold_char(U'Ա') == U'ա' && D::fold_char(U'Ａ') == U'ａ' && D::fold_char(U'Ẁ') == U'ẁ' && D::fold_char(U'×') == U'×' &&
              D::fold_char(U'é') == U'é' && D::fold_char(U'7') == U'7');
        core::Doc d;
        core::load_sample(d);
        {
            core::Doc::Loading load(d);
            auto st = *d.as<core::Story>("story.3");
            st.paras.at(0).text = "ÉTÉ à Москва; ΟΔΟΣ and οδος; Łódź";
            d.put("story.3", st);
        }
        const auto hits = [&](const std::string& what, D::FindOpts::Mode m = D::FindOpts::Mode::Text, bool mc = false, bool ww = false) {
            D::FindOpts f;
            f.what = what;
            f.mode = m;
            f.match_case = mc;
            f.whole_word = ww;
            f.story = "story.3";
            return D::find_all(d, f);
        };
        const auto& txt = d.as<core::Story>("story.3")->paras.at(0).text;
        const auto h1 = hits("été");
        check("text: été finds ÉTÉ, its bytes", h1.size() == 1 && txt.substr(static_cast<std::size_t>(h1[0].a.byte),
                                                                           static_cast<std::size_t>(h1[0].b.byte - h1[0].a.byte)) == "ÉTÉ");
        check("text: Cyrillic and Greek either way", hits("МОСКВА").size() == 1 && hits("οδοσ").size() == 2 && hits("ŁÓDŹ").size() == 1);
        check("text: match case still matches case", hits("été", D::FindOpts::Mode::Text, true).empty() && hits("ÉTÉ", D::FindOpts::Mode::Text, true).size() == 1);
        check("text: whole word with letters past ASCII", hits("ét", D::FindOpts::Mode::Text, false, true).empty() &&
                                                         hits("été", D::FindOpts::Mode::Text, false, true).size() == 1);
        const auto h2 = hits("(é)t(é)", D::FindOpts::Mode::Regex);
        check("regex: folded both sides, the groups as written", h2.size() == 1 && h2[0].groups.size() == 3 && h2[0].groups[1] == "É");
        check("regex: \\W still means not-a-word", hits("\\W\\W", D::FindOpts::Mode::Regex).size() >= 1 &&
                                                  hits("Москва\\W", D::FindOpts::Mode::Regex).size() == 1);
        check("wildcards: ^$ and folded letters", hits("Ł^$dź", D::FindOpts::Mode::Wildcards).size() == 1);
    }

    {
        std::cout << "\n-- contents out of date by an automatic level's style (s170) --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        core::doc::set_list_styles(j, d, "list.1", {"style.2", "style.3"});
        L::FlowCache flows;
        L::build_list(j, d, "list.1");
        const auto made = d.as<core::GenList>("list.1")->entry_styles.at(1);
        const auto stale = [&]() -> std::string {
            L::FlowCache f;
            for (const auto& x : L::stale_lists(d, f)) if (x.list == "list.1") return x.why;
            return "current";
        };
        check("built: current", stale() == "current", stale());
        check("level 2 automatic again: found by name -- current", core::doc::set_list_entry_style(j, d, "list.1", 2, "") && stale() == "current", stale());
        const auto name = d.as<core::Style>(made)->name;
        core::doc::edit_styles(j, d, {made}, "Name", [](core::Style& st) { st.name = "Figure Entry"; });
        check("its style renamed: none of the name -- out of date", stale() == "No " + name + " style yet", stale());
        j.undo();
        check("named again: current", stale() == "current", stale());
        {   // another style takes the name; the entries are still in the old one
            core::doc::edit_styles(j, d, {made}, "Name", [](core::Style& st) { st.name = "Old Entry"; });
            const auto other = core::doc::duplicate_style(j, d, made);
            core::doc::edit_styles(j, d, {other}, "Name", [&](core::Style& st) { st.name = name; });
            check("another style of the name: entries not in it", stale() == "Entries not in " + name, stale());
        }
    }

    {
        std::cout << "\n-- override several master items with a marquee (s169) --\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        std::vector<std::string> sp;
        for (const auto& x : core::spreads(d)) if (x.size() == 2 && !D::overridable_items(d, x[0]).empty() && !D::overridable_items(d, x[1]).empty()) { sp = x; break; }
        check("fixture: a spread whose pages show master items", !sp.empty());
        const auto pl = core::layout_pages(d, sp);
        const auto all = core::master_items_in(d, pl, {-1e4, -1e4, 2e4, 2e4});
        check("all of both pages: each page's items", all.size() == D::overridable_items(d, sp[0]).size() + D::overridable_items(d, sp[1]).size());
        const auto right = core::master_items_in(d, pl, {pl[1].r.x + 1, -1e4, 1e4, 2e4});
        check("the right page alone", !right.empty() && std::all_of(right.begin(), right.end(), [&](const core::MasterItemOn& m) { return m.page == sp[1]; }));
        const auto made = D::override_master_items(j, d, all);
        check("overridden: one step, a copy each", made.size() == all.size() && j.size() == 1 &&
              std::all_of(made.begin(), made.end(), [&](const auto& id) { return d.as<core::Frame>(id) && !d.as<core::Frame>(id)->overrides.empty(); }));
        check("now none to touch there", core::master_items_in(d, pl, {-1e4, -1e4, 2e4, 2e4}).empty());
        check("again: nothing, no step", D::override_master_items(j, d, all).empty() && j.size() == 1);
        j.undo();
        check("undo: the master's again", core::master_items_in(d, pl, {-1e4, -1e4, 2e4, 2e4}).size() == all.size());
        check("marquee overrides: through the door", d.strays() == 0);
    }

    {
        std::cout << "\n-- column rules stop at a spanning paragraph (s168) --\n";
        namespace L = composix::layout;
        core::Frame f;
        f.kind = core::FrameKind::Text;
        f.w = 300;
        f.h = 400;
        f.columns = 2;
        f.col_rule.weight = 1;
        f.col_rule.inset = 4;
        check("no flow: one rule down the gutter", L::column_rules(f).size() == 1);
        L::FrameText ft;
        L::Line a{0, 0, 10, 20, 14, 0, 0};
        L::Line s1{1, 0, 10, 100, 28, 0, 0};
        s1.cw = 300;
        s1.band = 1;
        L::Line s2 = s1;
        s2.y = 128;
        L::Line b{2, 0, 10, 170, 14, 0, 0};
        b.band = 2;
        ft.lines = {a, s1, s2, b};
        const auto r = L::column_rules(f, &ft);
        check("a spanning paragraph: the rule above it and under it", r.size() == 2 &&
              std::abs((r[0].r.y + r[0].r.h) - (100 - 4)) < 0.01 && std::abs(r[1].r.y - (156 + 4)) < 0.01 &&
              std::abs(r[1].r.y + r[1].r.h - (400 - 4)) < 0.01);
        L::Line top = s1;
        top.y = 0;
        ft.lines = {top, b};
        const auto r2 = L::column_rules(f, &ft);
        check("at the top: only under it", r2.size() == 1 && r2[0].r.y > 28);
    }

    {
        std::cout << "\n-- a sidebar's footnotes at the page's foot (s167) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const std::string page = d.as<core::Frame>("frame.1")->owner;
        const core::Rect mr = core::margin_rect(d, page);
        core::Doc::Id body;
        for (const auto& id : d.ids_of<core::Style>()) if (d.as<core::Style>(id)->name == "Body") body = id;
        {
            core::Doc::Loading load(d);
            core::Story side{"Sidebar", {}};
            core::Para pp;
            pp.style = body;
            pp.text = "A sidebar's words, with a note of its own.";
            side.paras.push_back(pp);
            d.put("story.90", side);
            core::Frame f;
            f.owner = page;
            f.kind = core::FrameKind::Text;
            f.x = mr.x;
            f.y = mr.y + 40;
            f.w = mr.w * 0.4;
            f.h = 120;
            f.z = 50;
            f.story = "story.90";
            d.put("frame.90", f);
        }
        D::insert_note(j, d, "story.90", {0, 10}, "The sidebar's note -- at the page's foot, under the main text.");
        L::FlowCache fl;
        const auto& sf = fl.get(d, "story.90");
        const auto* fr = d.as<core::Frame>("frame.90");
        const L::NotePlace* np = nullptr;
        for (const auto& ft : sf.frames) if (ft.frame == "frame.90" && !ft.notes.empty()) np = &ft.notes.front();
        check("the sidebar's note is set", np != nullptr);
        if (np) {
            check("at the margins' width", std::abs(np->x - (mr.x - fr->x)) < 0.01 && std::abs(np->w - mr.w) < 0.01);
            check("at the page's foot", std::abs(fr->y + np->y + np->h - mr.b()) < 0.5, std::to_string(fr->y + np->y + np->h) + " vs " + std::to_string(mr.b()));
            const double top_of_notes = fr->y + np->y;
            const auto& mf = fl.get(d, "story.1");
            bool clear = true;
            for (const auto& ft : mf.frames) {
                const auto* f = d.as<core::Frame>(ft.frame);
                if (f->owner != page) continue;
                for (const auto& l : ft.lines) if (f->y + l.y + l.h > top_of_notes + 0.5) clear = false;
                for (const auto& n : ft.notes) if (f->y + n.y + n.h > top_of_notes + 0.5) clear = false;
            }
            check("the main text (and its notes) stop above it", clear);
            // the sidebar's words change (text only): the main story is flowed again around more notes
            D::insert_note(j, d, "story.90", {0, 30}, "A second note, longer than the first, so the foot it takes grows by a line or two at least.");
            const auto& sf2 = fl.get(d, "story.90");
            double top2 = 1e9;
            for (const auto& ft : sf2.frames) if (ft.frame == "frame.90") for (const auto& n : ft.notes) top2 = std::min(top2, fr->y + n.y);
            const auto& mf2 = fl.get(d, "story.1");
            bool clear2 = true;
            for (const auto& ft : mf2.frames) {
                const auto* f = d.as<core::Frame>(ft.frame);
                if (f->owner != page) continue;
                for (const auto& l : ft.lines) if (f->y + l.y + l.h > top2 + 0.5) clear2 = false;
            }
            check("a note added to the sidebar: the main text stops higher", top2 < top_of_notes && clear2);
        }
        check("a wide frame's notes stay its own (the main story's)", [&] {
            const auto& mf = fl.get(d, "story.1");
            for (const auto& ft : mf.frames) {
                const auto* f = d.as<core::Frame>(ft.frame);
                for (const auto& n : ft.notes) if (std::abs(n.w - core::column_width(*f, 0)) > 0.01 && core::column_count(*f) == 1) return false;
            }
            return true;
        }());
        {   // a sidebar reaching down to the foot: its own words stop above its notes
            core::Doc::Loading load(d);
            auto f = *d.as<core::Frame>("frame.90");
            f.h = mr.b() - f.y;
            d.put("frame.90", f);
            auto st = *d.as<core::Story>("story.90");
            std::string more;
            for (int k = 0; k < 40; ++k) more += " More words of the sidebar, running down the page.";
            st.paras.at(0).text += more;
            d.put("story.90", st);
        }
        {
            L::FlowCache f2;
            const auto& sf3 = f2.get(d, "story.90");
            const auto* f9 = d.as<core::Frame>("frame.90");
            double ntop = 1e9, lbot = 0;
            for (const auto& ft : sf3.frames) if (ft.frame == "frame.90") {
                for (const auto& n : ft.notes) ntop = std::min(ntop, n.y);
                for (const auto& l : ft.lines) lbot = std::max(lbot, l.y + l.h);
            }
            check("a sidebar down to the foot: its words stop above its notes", ntop < 1e9 && lbot <= ntop + 0.5,
                  std::to_string(lbot) + " / " + std::to_string(ntop) + " (frame h " + std::to_string(f9->h) + ")");
        }
        check("foot neighbours: the main story waits on the sidebar's", [&] {
            const auto nb = L::foot_neighbours(d, "story.90");
            return std::find(nb.begin(), nb.end(), "story.1") != nb.end();
        }());
    }

    {
        std::cout << "\n-- a paragraph spanning the columns: bands (s166) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        {   // the chapters in two columns
            core::Doc::Loading load(d);
            for (const auto& fid : d.ids_of<core::Frame>()) {
                auto f = *d.as<core::Frame>(fid);
                if (f.story != "story.1") continue;
                f.columns = 2;
                d.put(fid, f);
            }
        }
        core::Doc::Id head;
        for (const auto& id : d.ids_of<core::Style>()) if (d.as<core::Style>(id)->name == "Heading 1") head = id;
        const auto sp = D::duplicate_style(j, d, head);
        check("Span columns: a style setting", D::style_edit_label("span") == "Span columns" &&
              D::edit_styles(j, d, {sp}, "Span columns", [&](core::Style& st) { D::style_edit(d, st, "span", "1"); st.start = core::ParaStart::Anywhere; }) &&
              d.as<core::Style>(sp)->span);
        // a spanning head part way through the opening chapter
        const int P = 3;
        D::set_para_style(j, d, "story.1", {P, 0}, {P, 0}, sp);
        L::FlowCache fl;
        const auto& f = fl.get(d, "story.1");
        const L::FrameText* ft = nullptr;
        for (const auto& x : f.frames) for (const auto& l : x.lines) if (l.para == P) ft = &x;
        check("the spanning paragraph is set", ft != nullptr);
        if (ft) {
            const auto* fr = d.as<core::Frame>(ft->frame);
            const double x0 = core::column_x(*fr, 0), x1 = core::column_x(*fr, 1) + core::column_width(*fr, 1);
            double s_top = 1e9, s_bot = -1;
            bool wide = true;
            for (const auto& l : ft->lines)
                if (l.para == P) {
                    s_top = std::min(s_top, l.y);
                    s_bot = std::max(s_bot, l.y + l.h);
                    wide = wide && std::abs(l.cw - (x1 - x0)) < 0.01 && l.x >= x0 - 0.01 && l.col == 0 && (l.w > 0 || std::abs(l.x - x0) < 0.01);
                }
            std::string dbg;
            for (const auto& l : ft->lines) if (l.para == P) dbg += "[x" + std::to_string(l.x) + " cw" + std::to_string(l.cw) + " c" + std::to_string(l.col) + " b" + std::to_string(l.band) + " w" + std::to_string(l.w) + "]";
            check("across both columns, its own width", wide, dbg + " x0 " + std::to_string(x0) + " x1 " + std::to_string(x1));
            bool above_ok = true, below_ok = true, two_above = false, two_below = false;
            std::set<int> cols_above, cols_below;
            for (const auto& l : ft->lines) {
                if (l.para < P) { above_ok = above_ok && l.y + l.h <= s_top + 0.01; cols_above.insert(l.col); }
                if (l.para > P) { below_ok = below_ok && l.y >= s_bot - 0.01; cols_below.insert(l.col); }
            }
            two_above = cols_above.size() == 2 || cols_above.empty();
            two_below = cols_below.size() == 2 || cols_below.size() <= 1;
            check("the text before it: above it", above_ok);
            check("the text before it: balanced over both columns", two_above);
            check("the text after it: below it, in columns", below_ok && two_below);
            int bands = 0;
            for (const auto& l : ft->lines) bands = std::max(bands, l.band + 1);
            check("bands: before, across, after", bands >= 1 + (cols_above.empty() ? 0 : 1) + (cols_below.empty() ? 0 : 1),
                  std::to_string(bands));
            // balanced: the two columns above end within a line or so of each other
            double b0 = 0, b1 = 0;
            for (const auto& l : ft->lines) if (l.para < P) (l.col == 0 ? b0 : b1) = std::max(l.col == 0 ? b0 : b1, l.y + l.h);
            const double lead = 14;
            check("balanced: the columns above end together", cols_above.size() < 2 || std::abs(b0 - b1) <= 2 * lead + 0.01,
                  std::to_string(b0) + " / " + std::to_string(b1));
        }
        check("no spanning paragraph: as before (one band)", [&] {
            core::Doc e;
            core::load_sample(e);
            L::FlowCache fe;
            for (const auto& x : fe.get(e, "story.1").frames) for (const auto& l : x.lines) if (l.band != 0 || l.cw != 0) return false;
            return true;
        }());
        {
            core::Doc e;
            check("json: span round-trips", core::from_json(e, core::to_json(d)) && e.as<core::Style>(sp)->span);
        }
        check("span: through the door", d.strays() == 0);
    }

    {
        std::cout << "\n-- a style's setting, one code path; the specimen shows what's waiting (s164) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::load_sample(d);
        core::Doc::Id body, emph;
        for (const auto& id : d.ids_of<core::Style>()) {
            if (d.as<core::Style>(id)->name == "Body") body = id;
            if (d.as<core::Style>(id)->kind == core::StyleKind::Character && emph.empty()) emph = id;
        }
        core::Style b = *d.as<core::Style>(body);
        check("labels: a plain setting has one, the others none", D::style_edit_label("ssize") == "Size" &&
              D::style_edit_label("rule_b_w") == "Rule" && D::style_edit_label("based_on").empty() && D::style_edit_label("style").empty());
        const double lead0 = b.leading, size0 = b.size;
        D::style_edit(d, b, "ssize", "13");
        check("size: set; leading follows when it was the usual ratio",
              b.size == 13 && (std::abs(lead0 - size0 * 1.2) < 1.5 ? b.leading == std::round(13 * 1.2 * 2) / 2 : b.leading == lead0));
        D::style_edit(d, b, "align", "1");
        D::style_edit(d, b, "rule_b_w", "2");
        check("align and a rule below (offset clear of the text)", b.align == core::Align::Center && b.rule_below.weight == 2 &&
                                                                  b.rule_below.offset > 0);
        D::style_edit(d, b, "list_type", "1");
        check("a list makes room for its marker", b.list.type == core::ListType::Bullet && b.left_indent >= b.list.hang);
        core::Style e = *d.as<core::Style>(emph);
        const auto e0 = e;
        D::style_edit(d, e, "lindent", "36");
        check("a paragraph setting leaves a character style alone", e == e0);
        L::StyleOver over{{body, b}};
        const auto sd = L::style_specimen_doc(d, body, false, 432, 324, &over);
        check("specimen: the waiting change shown", sd.as<core::Style>(body)->size == 13 && sd.as<core::Style>(body)->align == core::Align::Center);
        check("...the document's style as it was", d.as<core::Style>(body)->size == size0);
    }

    {
        std::cout << "\n-- a style, greeked: on its own and in use (s163) --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::load_sample(d);
        core::Doc::Id body, head, emph;
        for (const auto& id : d.ids_of<core::Style>()) {
            const auto* s = d.as<core::Style>(id);
            if (s->name == "Body") body = id;
            if (s->name == "Heading 1") head = id;
            if (s->kind == core::StyleKind::Character && emph.empty()) emph = id;
        }
        check("fixture: Body, Heading 1, a character style", !body.empty() && !head.empty() && !emph.empty());
        const auto alone = L::style_specimen_doc(d, body, false, 432, 324);
        const auto* st = alone.as<core::Story>("story.1");
        check("on its own: paragraphs all in it", st && st->paras.size() >= 2 &&
              std::all_of(st->paras.begin(), st->paras.end(), [&](const core::Para& p) { return p.style == body; }));
        const auto use = L::style_specimen_doc(d, head, true, 432, 648);
        const auto* su = use.as<core::Story>("story.1");
        int mine = 0, other = 0;
        for (const auto& p : su->paras) (p.style == head ? mine : other)++;
        check("in use: it among others", mine >= 1 && other >= 2);
        const auto use_styles = use.ids_of<core::Style>();
        check("in use: a next-page start set where it falls (one page)",
              std::all_of(use_styles.begin(), use_styles.end(),
                          [&](const auto& id) { return use.as<core::Style>(id)->start == core::ParaStart::Anywhere; }));
        L::FlowCache fl;
        check("in use: it all fits the page", !fl.get(use, "story.1").overset);
        const auto ch = L::style_specimen_doc(d, emph, true, 432, 648);
        bool run = false;
        for (const auto& p : ch.as<core::Story>("story.1")->paras) for (const auto& r : p.runs) run = run || r.style == emph;
        check("a character style: on some words of the body", run);
        check("the document untouched", d.strays() == 0);
        auto* surf = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 800, 600);
        cairo_t* cr = cairo_create(surf);
        cairo_set_source_rgb(cr, 0.9, 0.9, 0.9);
        cairo_paint(cr);
        L::draw_style_specimen(cr, d, head, 800, 600);
        cairo_destroy(cr);
        cairo_surface_flush(surf);
        int white = 0;
        const auto* data = cairo_image_surface_get_data(surf);
        const int stride = cairo_image_surface_get_stride(surf);
        for (int y = 0; y < 600; y += 4)
            for (int x = 0; x < 800; x += 4) white += (reinterpret_cast<const std::uint32_t*>(data + y * stride)[x] & 0xffffff) == 0xffffff;
        cairo_surface_destroy(surf);
        check("drawn: paper on the pasteboard", white > 1000);
    }

    {
        std::cout << "\n-- a style's rows in groups; Set as one step (s161 / s162) --\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto o = core::project(d, "");
        const core::Item* para = nullptr;
        for (const auto* it : o.in(core::Segment::Styles))
            if (const auto* st = d.as<core::Style>(it->id); st && st->kind == core::StyleKind::Paragraph) { para = it; break; }
        check("a paragraph style item", para != nullptr);
        std::set<std::string> groups;
        bool ok = true;
        std::string why;
        for (const auto& f : para->fields) {
            if (f.label == "Kind" || f.buttons) { ok = ok && (f.group.empty() || f.edit == "style_base"); continue; }   // Kind above, actions below
            if (f.group.empty()) { ok = false; why = f.label; }
            groups.insert(f.group);
        }
        check("every row but Kind in a group", ok, why);
        check("shared fields keep their groups", [&] {
            const auto sh = core::shared_fields({para});
            return std::all_of(sh.begin(), sh.end(), [](const core::Field& f) { return f.label == "Kind" || f.buttons || !f.group.empty(); });
        }());
        check("groups: Text, Paragraph, Flow, List, Rules, Style",
              groups.count("Text") && groups.count("Paragraph") && groups.count("Flow") && groups.count("List") && groups.count("Rules") &&
              groups.count("Style"));
        {   // each group's rows together (one heading each)
            std::vector<std::string> seq;
            for (const auto& f : para->fields) if (!f.group.empty() && (seq.empty() || seq.back() != f.group)) seq.push_back(f.group);
            check("each group's rows together", std::set<std::string>(seq.begin(), seq.end()).size() == seq.size());
        }
        const auto sid = para->id;
        const auto before = *d.as<core::Style>(sid);
        const auto n0 = j.size();
        const bool did = D::styles_as_one(j, d, "Style settings", [&] {
            D::edit_styles(j, d, {sid}, "Size", [](core::Style& s) { s.size = 15; });
            D::edit_styles(j, d, {sid}, "Space after", [](core::Style& s) { s.space_after = 9; });
        });
        check("Set: one step for both", did && j.size() == n0 + 1 && j.undo_label() == "Style settings" &&
                                        d.as<core::Style>(sid)->size == 15 && d.as<core::Style>(sid)->space_after == 9);
        j.undo();
        check("undo: both back", *d.as<core::Style>(sid) == before);
        check("Set with nothing changed: no step", !D::styles_as_one(j, d, "Style settings", [] {}));
        check("groups: through the door", d.strays() == 0);
    }

    {
        std::cout << "\n-- find bold / italic by the paragraph style too (s160) --\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        // a bold paragraph style on paragraph 0, a word made italic by hand in paragraph 1
        const auto& st = *d.as<core::Story>("story.1");
        const auto ps0 = st.paras.at(0).style;
        D::edit_styles(j, d, {ps0}, "Weight", [](core::Style& s) { s.bold = true; });
        const bool other_bold = d.as<core::Style>(st.paras.at(1).style)->bold;
        D::set_local(j, d, "story.1", {1, 0}, {1, 3}, "Bold", [](core::Local& l) { l.bold = 1; });
        D::FindOpts f;
        f.bold = true;
        f.story = "story.1";
        const auto by_hand = D::find_all(d, f);
        check("bold, by hand only: not the bold style's paragraph",
              std::none_of(by_hand.begin(), by_hand.end(), [](const D::Hit& h) { return h.a.para == 0; }) &&
              std::any_of(by_hand.begin(), by_hand.end(), [](const D::Hit& h) { return h.a.para == 1 && h.a.byte == 0 && h.b.byte == 3; }));
        f.para_counts = true;
        const auto with = D::find_all(d, f);
        const int len0 = static_cast<int>(d.as<core::Story>("story.1")->paras.at(0).text.size());
        check("by the paragraph style too: the whole bold paragraph",
              std::any_of(with.begin(), with.end(), [&](const D::Hit& h) { return h.a.para == 0 && h.a.byte == 0 && h.b.byte == len0; }));
        if (!other_bold)
            check("...and still the word set by hand", std::any_of(with.begin(), with.end(), [](const D::Hit& h) { return h.a.para == 1 && h.b.byte == 3; }));
        D::set_local(j, d, "story.1", {0, 0}, {0, 2}, "Bold", [](core::Local& l) { l.bold = 0; });
        const auto with2 = D::find_all(d, f);
        check("words set not-bold by hand in a bold style: left out",
              std::any_of(with2.begin(), with2.end(), [&](const D::Hit& h) { return h.a.para == 0 && h.a.byte == 2 && h.b.byte == len0; }));
        f.what = "a";
        check("with words too", !D::find_all(d, f).empty() || len0 == 0);
    }

    {
        std::cout << "\n-- No break on the words themselves (s159) --\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto& p1 = [&]() -> const core::Para& { return d.as<core::Story>("story.1")->paras.at(1); };
        check("none to start", core::no_break_spans(d, p1()).empty());
        check("No break on: one step", D::set_local(j, d, "story.1", {1, 4}, {1, 20}, "No break", [](core::Local& l) { l.no_break = 1; }) &&
                                      j.undo_label() == "No break");
        const auto sp = core::no_break_spans(d, p1());
        check("its words kept together", sp.size() == 1 && sp[0].a == 4 && sp[0].b == 20);
        {
            core::Doc e;
            check("json: round-trips", core::from_json(e, core::to_json(d)) && core::no_break_spans(e, e.as<core::Story>("story.1")->paras.at(1)).size() == 1);
        }
        // a character style with No break, turned off on some of its words
        core::Doc::Id cs;
        for (const auto& id : d.ids_of<core::Style>())
            if (d.as<core::Style>(id)->kind == core::StyleKind::Character) { cs = id; break; }
        D::edit_styles(j, d, {cs}, "No break", [](core::Style& st) { st.no_break = true; });
        D::set_local(j, d, "story.1", {1, 0}, {1, 20}, "No break", [](core::Local& l) { l.no_break = -1; });
        D::set_char_style(j, d, "story.1", {1, 30}, {1, 50}, cs);
        check("the character style's", core::no_break_spans(d, p1()).size() == 1);
        D::set_local(j, d, "story.1", {1, 30}, {1, 40}, "No break", [](core::Local& l) { l.no_break = 0; });
        const auto sp2 = core::no_break_spans(d, p1());
        check("Off on the words beats the style's", sp2.size() == 1 && sp2[0].a == 40 && sp2[0].b == 50);
        check("keep_together breaks before them", core::keep_together(p1().text, {{4, 20}}, 0, 10) <= 4);
        check("no break: through the door", d.strays() == 0);
    }

    {
        std::cout << "\n-- a paragraph's own tab stops (s157) --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto& st0 = *d.as<core::Story>("story.1");
        const std::string ps = st0.paras.at(1).style;
        const auto style_tabs = d.as<core::Style>(ps)->tabs;
        std::vector<core::TabStop> mine{{144, core::TabAlign::Right, "."}, {72, core::TabAlign::Left, ""}};
        check("own stops: one step, sorted", core::doc::set_para_tabs(j, d, "story.1", {1, 0}, {1, 0}, "Add tab", mine) &&
                                             j.undo_label() == "Add tab" && d.as<core::Story>("story.1")->paras[1].over.tabs &&
                                             d.as<core::Story>("story.1")->paras[1].over.tabs->front().pos == 72);
        check("the style keeps its own", d.as<core::Style>(ps)->tabs == style_tabs);
        check("the layout sets it in them", core::doc::para_style(d, d.as<core::Story>("story.1")->paras[1]).tabs.size() == 2 &&
                                            core::doc::para_style(d, d.as<core::Story>("story.1")->paras[1]).tabs[1].leader == ".");
        check("only that paragraph", !d.as<core::Story>("story.1")->paras[2].over.tabs);
        check("the same again: no step", !core::doc::set_para_tabs(j, d, "story.1", {1, 0}, {1, 0}, "Add tab", mine));
        {
            core::Doc e;
            check("json: own stops round-trip", core::from_json(e, core::to_json(d)) && e.as<core::Story>("story.1")->paras[1].over.tabs &&
                                                *e.as<core::Story>("story.1")->paras[1].over.tabs == *d.as<core::Story>("story.1")->paras[1].over.tabs);
        }
        check("Reset to style: one step, the style's again",
              core::doc::clear_para_local(j, d, "story.1", {1, 0}, {1, 0}, {"tabs"}) && !d.as<core::Story>("story.1")->paras[1].over.tabs &&
              core::doc::para_style(d, d.as<core::Story>("story.1")->paras[1]).tabs == style_tabs);
        j.undo();
        check("undo: its own again", d.as<core::Story>("story.1")->paras[1].over.tabs.has_value());
        check("set to the style's: follows the style (none of its own)",
              core::doc::set_para_tabs(j, d, "story.1", {1, 0}, {1, 0}, "Move tab", style_tabs) && !d.as<core::Story>("story.1")->paras[1].over.tabs);
        check("a range: each paragraph", core::doc::set_para_tabs(j, d, "story.1", {1, 0}, {3, 0}, "Add tab", mine) &&
                                         d.as<core::Story>("story.1")->paras[2].over.tabs && d.as<core::Story>("story.1")->paras[3].over.tabs);
        {   // a style change later doesn't reach a paragraph with its own
            core::doc::edit_styles(j, d, {ps}, "Tabs", [](core::Style& s) { s.tabs = {{36, core::TabAlign::Left, ""}}; });
            check("its own stops hold over a style change", core::doc::para_style(d, d.as<core::Story>("story.1")->paras[1]).tabs.size() == 2 &&
                                                            d.as<core::Style>(ps)->tabs.size() == 1);
        }
        L::FlowCache f;
        check("still flows", !f.get(d, "story.1").frames.empty());
        check("own tabs: through the door", d.strays() == 0);
    }

    {
        std::cout << "\n-- footnotes page width: one foot under all the columns (s156) --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        {   // the chapters in two columns
            core::Doc::Loading load(d);
            for (const auto& fid : d.ids_of<core::Frame>()) {
                auto f = *d.as<core::Frame>(fid);
                if (f.story != "story.1") continue;
                f.columns = 2;
                d.put(fid, f);
            }
        }
        const int np = static_cast<int>(d.as<core::Story>("story.1")->paras.size());
        for (int p = 1; p < std::min(np, 7); ++p)
            core::doc::insert_note(j, d, "story.1", {p, 2}, "A note for paragraph " + std::to_string(p) + ", a line or two long in the note size.");
        L::FlowCache flows;
        const auto& f = flows.get(d, "story.1");
        int placed = 0, last = 0;
        bool wide = true, clear = true, ordered = true, inside = true;
        std::string why;
        for (const auto& ft : f.frames) {
            const auto* fr = d.as<core::Frame>(ft.frame);
            const int n = core::column_count(*fr);
            const double x0 = core::column_x(*fr, 0), x1 = core::column_x(*fr, n - 1) + core::column_width(*fr, n - 1);
            double top = 1e9, prev_y = -1;
            for (const auto& note : ft.notes) {
                ++placed;
                ordered = ordered && note.number > last && note.y > prev_y;
                last = note.number;
                prev_y = note.y;
                top = std::min(top, note.y);
                if (n > 1 && (std::abs(note.x - x0) > 0.01 || std::abs(note.w - (x1 - x0)) > 0.01)) { wide = false; why = ft.frame; }
                inside = inside && note.y + note.h <= fr->h - std::max(0.0, fr->inset.bottom) + 0.01;
            }
            for (const auto& l : ft.lines)   // no line in ANY column runs into the foot
                if (l.y + l.h > top + 0.01) {
                    bool first = true;
                    for (const auto& m : ft.lines) if (m.col == l.col && m.y < l.y) first = false;
                    if (!first) { clear = false; why = ft.frame + " col " + std::to_string(l.col); }
                }
        }
        check("notes placed, numbered in order, stacked in order", placed >= 6 && ordered, std::to_string(placed));
        check("each note the width of all the columns", wide, why);
        check("no line in any column runs into the foot", clear, why);
        check("the foot inside its frame", inside);
        {   // one rule over the foot, across the first note's width share
            int rules = 0;
            for (const auto& ft : f.frames) {
                const auto* fr = d.as<core::Frame>(ft.frame);
                if (ft.notes.empty() || core::column_count(*fr) < 2) continue;
                std::set<int> cols;
                for (const auto& note : ft.notes) cols.insert(note.col);
                rules += static_cast<int>(cols.size());
                check("one foot: one rule over it (" + ft.frame + ")", cols.size() == 1);
            }
            check("a frame with notes in two columns", rules >= 1);
        }
        {   // one column: as before -- the column's width
            core::Doc e;
            core::DocJournal je{e};
            e.set_door([&je] { return je.open(); });
            core::load_sample(e);
            core::doc::insert_note(je, e, "story.1", {1, 4}, "One note.");
            L::FlowCache fe;
            bool same = false;
            for (const auto& ft : fe.get(e, "story.1").frames)
                for (const auto& note : ft.notes) {
                    const auto* fr = e.as<core::Frame>(ft.frame);
                    if (core::column_count(*fr) == 1) same = std::abs(note.w - core::column_width(*fr, 0)) < 0.01;
                }
            check("one column: the note its width", same);
        }
    }

    {
        std::cout << "\n-- master items across the spine (s155) --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        // a facing master, a spread of pages using it
        std::string M, Lp, Rp;
        for (const auto& sp : core::spreads(d)) {
            if (sp.size() != 2) continue;
            const auto* a = d.as<core::Page>(sp[0]);
            const auto* b = d.as<core::Page>(sp[1]);
            if (!a || !b || a->master.empty() || a->master != b->master) continue;
            const auto* m = d.as<core::Master>(a->master);
            if (m && m->facing && m->folds.empty() && a->w == m->w && b->w == m->w) { M = a->master; Lp = sp[0]; Rp = sp[1]; break; }
        }
        check("fixture: a facing master's spread", !M.empty());
        const double w = d.as<core::Master>(M)->w;
        const auto id = core::doc::add_frame(j, d, M, core::FrameKind::Shape, {w - 100, 100, 160, 50}, 1, 12, true);
        const auto& f = *d.as<core::Frame>(id);
        check("drawn across: one item, across, both sides", !id.empty() && f.across && f.side == core::Side::Any && j.size() == 1);
        check("dx: none on the left, a page width back on the right",
              core::across_dx(d, f, core::Side::Left) == 0 && core::across_dx(d, f, core::Side::Right) == -w);
        check("shows on both pages", core::master_item_on(d, id, Lp) && core::master_item_on(d, id, Rp));
        const auto mpl = core::layout_pages(d, {M});
        check("master spread: on both panels, one place", mpl.size() == 2 && core::on_panel(d, mpl[0], id) && core::on_panel(d, mpl[1], id) &&
                                                          core::frame_in_layout(d, mpl, id).x == mpl[0].r.x + w - 100);
        const auto pl = core::layout_pages(d, {Lp, Rp});
        core::Doc::Id on;
        check("hit: its right part on the right page", core::hit_master_item(d, pl, pl[1].r.x + 30, 120, 0, &on) == id && on == Rp);
        check("hit: its left part on the left page", core::hit_master_item(d, pl, pl[0].r.x + w - 50, 120, 0, &on) == id && on == Lp);
        const auto t = core::snap_targets(d, pl, {});
        check("snap: its right edge on the right page", std::any_of(t.xs.begin(), t.xs.end(), [&](double x) { return std::abs(x - (pl[1].r.x + 60)) < 1e-6; }));
        {   // drawn: each page its part, cut at the spine
            core::Doc::Loading load(d);
            auto g = f;
            for (const auto& sw : d.ids_of<core::Swatch>()) if (d.as<core::Swatch>(sw)->name == "Black") g.fill = sw;
            d.put(id, g);
        }
        L::FlowCache flows;
        const auto ink_at = [&](const std::string& page, int x, int y) {
            const auto* pg = d.as<core::Page>(page);
            auto* surf = cairo_image_surface_create(CAIRO_FORMAT_RGB24, static_cast<int>(pg->w), static_cast<int>(pg->h));
            cairo_t* cr = cairo_create(surf);
            cairo_set_source_rgb(cr, 1, 1, 1);
            cairo_paint(cr);
            L::draw_page(cr, d, page, flows, {}, nullptr, {});
            cairo_destroy(cr);
            cairo_surface_flush(surf);
            const auto* row = cairo_image_surface_get_data(surf) + y * cairo_image_surface_get_stride(surf);
            const bool ink = (reinterpret_cast<const std::uint32_t*>(row)[x] & 0xffffff) != 0xffffff;
            cairo_surface_destroy(surf);
            return ink;
        };
        check("drawn: the left page its left part", ink_at(Lp, static_cast<int>(w) - 50, 120) && !ink_at(Lp, static_cast<int>(w) - 120, 120));
        check("drawn: the right page its right part", ink_at(Rp, 30, 120) && !ink_at(Rp, 80, 120));
        check("json: across round-trips", [&] { core::Doc e; return core::from_json(e, core::to_json(d)) && e.as<core::Frame>(id)->across; }());
        check("shows on Right pages: off the spine, its place kept", core::doc::set_shows_on(j, d, {id}, 1) && !d.as<core::Frame>(id)->across &&
                                                                    d.as<core::Frame>(id)->side == core::Side::Right && d.as<core::Frame>(id)->x == -100);
        check("...and back across", core::doc::set_shows_on(j, d, {id}, 3) && d.as<core::Frame>(id)->across && d.as<core::Frame>(id)->x == w - 100);
        j.undo();
        check("undo: Right again", !d.as<core::Frame>(id)->across && d.as<core::Frame>(id)->side == core::Side::Right);
        j.redo();
        const auto copy = core::doc::override_master_item(j, d, Rp, id);
        check("override on the right page: its copy where it showed -- cut at the spine (s182)",
              !copy.empty() && !d.as<core::Frame>(copy)->across && d.as<core::Frame>(copy)->x == 0 &&
              d.as<core::Frame>(copy)->w == 60);
        {   // s182 (W2): the left page's copy -- its part up to the spine; a picture stays put; text whole
            const auto lcopy = core::doc::override_master_item(j, d, Lp, id);
            const auto* lc = d.as<core::Frame>(lcopy);
            check("override on the left page: its part up to the spine", lc && lc->x == w - 100 && lc->w == 100);
            const auto pic = core::doc::add_frame(j, d, M, core::FrameKind::Image, {w - 120, 300, 200, 100}, 1, 12, true);
            {
                core::Doc::Loading load(d);
                core::Asset a;
                a.name = "spread.jpg";
                a.path = "/x/spread.jpg";
                a.px_w = 4000;
                a.px_h = 1000;   // 4:1 -- Fill covers 200 x 100 at 400 x 100, centred
                d.put("asset.s182", a);
                auto g = *d.as<core::Frame>(pic);
                g.asset = "asset.s182";
                d.put(pic, g);
            }
            const auto before = core::picture_rect({w - 120, 300, 200, 100}, *d.as<core::Frame>(pic), 4000, 1000);
            const auto rp = core::doc::override_master_item(j, d, Rp, pic);
            const auto* rf = d.as<core::Frame>(rp);
            const auto after = rf ? core::picture_rect({rf->x + w, rf->y, rf->w, rf->h}, *rf, 4000, 1000) : core::Rect{};
            const auto near = [](double a, double b) { return std::abs(a - b) < 1e-6; };
            check("override a picture on the right: cut at the spine, the picture where it was",
                  rf && near(rf->x, 0) && near(rf->w, 80) && rf->fit == core::ImageFit::Custom &&
                  near(after.x, before.x) && near(after.y, before.y) && near(after.w, before.w) && near(after.h, before.h));
            const auto tx = core::doc::add_frame(j, d, M, core::FrameKind::Text, {w - 50, 500, 100, 40}, 1, 12, true);
            const auto tc = core::doc::override_master_item(j, d, Lp, tx);
            check("override a text frame across: kept whole (its words would reflow)",
                  d.as<core::Frame>(tc) && d.as<core::Frame>(tc)->w == 100 && d.as<core::Frame>(tc)->x == w - 50);
            j.undo();
            j.undo();
            j.undo();
            j.undo();
            j.undo();
            check("overrides undone", !d.as<core::Frame>(lcopy) && !d.as<core::Frame>(rp) && !d.as<core::Frame>(tc));
        }
        // a single (not facing) master has no spine
        std::string single;
        for (const auto& m : d.ids_of<core::Master>()) if (!d.as<core::Master>(m)->facing && !d.frames_on(m).empty()) single = m;
        if (!single.empty()) check("single master: no across", !core::doc::set_shows_on(j, d, {d.frames_on(single).front()}, 3));
        check("across: through the door", d.strays() == 0);
    }

    {
        std::cout << "\n-- ruler origin (s154) --\n";
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        std::vector<std::string> sp;
        for (const auto& x : core::spreads(d)) if (x.size() == 2) { sp = x; break; }
        const std::string L = sp.at(0), R = sp.at(1);
        const double wl = d.as<core::Page>(L)->w;
        using O = core::RulerOrigin;
        const auto near = [](double a, double b) { return std::abs(a - b) < 1e-6; };
        check("origin: Spread by default -- the right page's zero its left page's corner",
              d.settings().ruler_origin == O::Spread && near(core::ruler_zero(d, L).x, 0) && near(core::ruler_zero(d, R).x, -wl));
        const auto pl = core::layout_pages(d, {L, R});
        const auto s1 = core::ruler_spans(d, pl);
        check("spans: a spread is one piece from its corner", s1.size() == 1 && near(s1[0].zero, 0));
        // a frame on the right page: X from the spread's corner
        std::string fr;
        for (const auto& f : d.frames_on(R)) { fr = f; break; }
        if (fr.empty()) {
            core::Doc::Loading load(d);
            d.put("frame.95", core::Frame{R, core::FrameKind::Shape, 54, 54, 72, 72, 0, 9});
            fr = "frame.95";
        }
        const auto bx = [&]() -> std::string {
            const auto items = core::frame_items(d, {fr});
            for (const auto& f : items.at(0).fields) if (f.edit == "bounds") return f.value.substr(0, f.value.find('\t'));
            return "";
        };
        const double fx = d.as<core::Frame>(fr)->x;
        check("inspector: X from the spread's corner", bx() == core::length_in(fx + wl, d.settings().units), bx());
        check("origin: one step, the zero point back", core::doc::set_ruler_origin(j, d, O::Page) && d.settings().ruler_origin == O::Page &&
                                                     j.undo_label() == "Ruler origin" && !core::doc::set_ruler_origin(j, d, O::Page));
        check("Page: each page its own zero", near(core::ruler_zero(d, R).x, 0) && bx() == core::length_in(fx, d.settings().units));
        const auto s2 = core::ruler_spans(d, pl);
        check("spans: Page -- two pieces, cut at the spine", s2.size() == 2 && near(s2[0].hi, wl) && near(s2[1].lo, wl) && near(s2[1].zero, wl));
        core::doc::set_ruler_origin(j, d, O::Spine);
        check("Spine: the left page's right edge, the right page's left", near(core::ruler_zero(d, L).x, wl) && near(core::ruler_zero(d, R).x, 0));
        const auto s3 = core::ruler_spans(d, pl);
        check("spans: Spine -- one piece, zero at the spine", s3.size() == 1 && near(s3[0].zero, wl));
        check("Spine: the zero point can't move", !core::doc::set_ruler_zero(j, d, 10, 10));
        core::doc::set_ruler_origin(j, d, O::Spread);
        const auto z = core::zero_at(d, pl, wl + 10, 20);
        check("zero_at: from the spread's corner", near(z.x, wl + 10) && near(z.y, 20));
        check("zero point: one step", core::doc::set_ruler_zero(j, d, z.x, z.y) && j.undo_label() == "Zero point");
        check("zero point: both pages read from it", near(core::ruler_zero(d, R).x, 10) && near(core::ruler_zero(d, L).x, wl + 10) &&
                                                    near(core::ruler_zero(d, R).y, 20));
        check("inspector: X / Y from the zero point", bx() == core::length_in(fx - 10, d.settings().units), bx() + " vs " + core::length_in(fx - 10, d.settings().units));
        {
            core::Doc e;
            check("json: origin and zero point round-trip", core::from_json(e, core::to_json(d)) && e.settings().ruler_origin == O::Spread &&
                                                            near(e.settings().zero_x, wl + 10) && near(e.settings().zero_y, 20));
        }
        check("origin again: the zero point back", core::doc::set_ruler_origin(j, d, O::Spread) && d.settings().zero_x == 0);
        j.undo();
        check("undo: the zero point returns", near(d.settings().zero_x, wl + 10));
        check("ruler origin: through the door", d.strays() == 0);
    }

    {
        std::cout << "\n-- find: full case folding (s152) --\n";
        namespace D = core::doc;
        check("fold_text: sharp s (both cases) ss, ligatures their letters",
              D::fold_text("Straße") == "strasse" && D::fold_text("STRAẞE") == "strasse" &&
              D::fold_text("\uFB01ne \uFB04") == "fine ffl" && D::fold_text("ÉTÉ") == "été");
        core::Doc d;
        core::load_sample(d);
        {
            core::Doc::Loading load(d);
            auto st = *d.as<core::Story>("story.3");
            st.paras.at(0).text = "Die Straße, the STRASSE; a \uFB01ne line; ß alone";
            d.put("story.3", st);
        }
        const auto hits = [&](const std::string& what, D::FindOpts::Mode m = D::FindOpts::Mode::Text, bool mc = false, bool ww = false) {
            D::FindOpts f;
            f.what = what;
            f.mode = m;
            f.match_case = mc;
            f.whole_word = ww;
            f.story = "story.3";
            return D::find_all(d, f);
        };
        const auto& txt = d.as<core::Story>("story.3")->paras.at(0).text;
        const auto word = [&](const D::Hit& h) { return txt.substr(static_cast<std::size_t>(h.a.byte), static_cast<std::size_t>(h.b.byte - h.a.byte)); };
        const auto h1 = hits("strasse");
        check("text: strasse finds Straße and STRASSE, their bytes", h1.size() == 2 && word(h1[0]) == "Straße" && word(h1[1]) == "STRASSE");
        const auto h2 = hits("STRAẞE");
        check("text: capital sharp s finds both", h2.size() == 2);
        const auto h3 = hits("fine");
        check("text: a ligature found by its letters", h3.size() == 1 && word(h3[0]) == "\uFB01ne");
        check("text: never half a sharp s", hits("s alone").empty() && hits("ss alone").size() == 1 && hits("ss").size() == 3);
        check("text: match case does not fold", hits("strasse", D::FindOpts::Mode::Text, true).empty() &&
                                               hits("Straße", D::FindOpts::Mode::Text, true).size() == 1);
        check("text: whole word through the fold", hits("strasse", D::FindOpts::Mode::Text, false, true).size() == 2);
        const auto h4 = hits("stra(ss)e", D::FindOpts::Mode::Regex);
        check("regex: folded, the groups as written", h4.size() == 2 && h4[0].groups.size() == 2 && h4[0].groups[1] == "ß" && h4[1].groups[1] == "SS");
        check("regex: sharp s in the pattern; in a class one for one", hits("straße", D::FindOpts::Mode::Regex).size() == 2 &&
                                                                     hits("ß+ alone", D::FindOpts::Mode::Regex).size() == 1);
        check("wildcards: sharp s in the pattern", hits("^$traße", D::FindOpts::Mode::Wildcards).size() == 2);
    }

    {
        std::cout << "\n-- level styles, page numbers (s090) --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto stale = [&]() -> std::string {
            L::FlowCache f;
            const auto v = L::stale_lists(d, f);
            return v.empty() ? std::string("current") : v.front().why;
        };
        L::build_list(j, d, "list.1");
        const auto texts = [&] {
            std::string t;
            for (const auto& p : d.as<core::Story>("story.9")->paras) t += p.text + "|";
            return t;
        };
        check("numbers off: one step", core::doc::set_list_numbers(j, d, "list.1", false) && !d.as<core::GenList>("list.1")->numbers &&
                                       !core::doc::set_list_numbers(j, d, "list.1", false));
        check("numbers off: the list is out of date", stale() != "current");
        L::build_list(j, d, "list.1");
        check("numbers off: built -- the words alone, the title kept", texts() == "Contents|Chapter One|Chapter Two|");
        check("numbers off: current, and a rebuild changes nothing", stale() == "current" && !L::build_list(j, d, "list.1"));
        core::doc::set_list_numbers(j, d, "list.1", true);
        check("numbers on again: says so", stale() == "Page numbers not written");
        L::build_list(j, d, "list.1");
        check("numbers on again: built", texts() == "Contents|Chapter One\t1|Chapter Two\t3|");
        // Level 1 in a picked style.
        check("entry style: picked, one step", core::doc::set_list_entry_style(j, d, "list.1", 1, "style.3") &&
                                               d.as<core::GenList>("list.1")->entry_styles == std::vector<std::string>{"style.3"});
        check("entry style: a character style refused", !core::doc::set_list_entry_style(j, d, "list.1", 1, "style.5"));
        L::build_list(j, d, "list.1");
        const auto& ps = d.as<core::Story>("story.9")->paras;
        check("entry style: built in it, the title kept", ps.size() == 3 && ps[0].text == "Contents" && ps[1].style == "style.3" &&
                                                          ps[2].style == "style.3");
        check("entry style: back to Automatic -- the list forgets it", core::doc::set_list_entry_style(j, d, "list.1", 1, "") &&
                                                                       d.as<core::GenList>("list.1")->entry_styles.empty());
        // The title rule.
        core::Story st{"t", {{"style.9", "Contents"}, {"style.8", "A"}, {"style.8", "B"}}};
        const bool titled = core::doc::list_title_paras(st) == 1;
        core::Story same{"t", {{"style.8", "A"}, {"style.8", "B"}}};
        core::Story alone{"t", {{"style.9", "Contents"}}};
        core::Story tabbed{"t", {{"style.8", "A\t1"}, {"style.9", "B"}}};
        check("title: another style below -- a title; all alike -- none; alone -- a title; a tab -- none",
              titled && core::doc::list_title_paras(same) == 0 && core::doc::list_title_paras(alone) == 1 &&
              core::doc::list_title_paras(tabbed) == 0);
        std::string err;
        core::Doc f;
        core::doc::set_list_numbers(j, d, "list.1", false);
        check("json: page numbers off kept", core::from_json(f, core::to_json(d), &err) && !f.as<core::GenList>("list.1")->numbers, err);
    }

    {
        std::cout << "\n-- stale contents (s089) --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto why = [&]() -> std::string {
            L::FlowCache f;
            const auto v = L::stale_lists(d, f);
            return v.empty() ? std::string("current") : v.front().list + ": " + v.front().why;
        };
        check("stale: the sample's extra line", why() == "list.1: \u201cSources\u201d listed, no such heading");
        L::build_list(j, d, "list.1");
        check("stale: built -- current", why() == "current");
        const auto toc = *d.as<core::Story>("story.9");
        const auto body = *d.as<core::Story>("story.1");
        {   // a written number that's wrong
            core::Doc::Loading load(d);
            auto st = toc;
            st.paras.at(2).text = "Chapter Two\t9";
            d.put("story.9", st);
        }
        check("stale: a page number moved", why() == "list.1: Chapter Two: page 9 → 3");
        {   // ... put back; a heading renamed
            core::Doc::Loading load(d);
            d.put("story.9", toc);
            auto st = body;
            for (auto& p : st.paras) if (p.text == "Chapter Two") p.text = "Chapter Deux";
            d.put("story.1", st);
        }
        check("stale: a heading renamed", why() == "list.1: \u201cChapter Deux\u201d not listed yet");
        {
            core::Doc::Loading load(d);
            d.put("story.1", body);
        }
        check("stale: put back -- current again", why() == "current");
        {
            auto l = *d.as<core::GenList>("list.1");
            l.built_entries = -1;
            core::Doc e = d;
            core::DocJournal je{e};
            e.set_door([&je] { return je.open(); });
            e.put("list.1", l);
            L::FlowCache f;
            const auto v = L::stale_lists(e, f);
            check("stale: never built", v.size() == 1 && v[0].why == "Not built yet");
        }
        // The outline and preflight say so.
        {
            core::Doc e;
            core::load_sample(e);
            L::FlowCache f;
            std::map<std::string, std::string> m;
            for (const auto& x : L::stale_lists(e, f)) m[x.list] = x.why;
            const auto o = core::project(e, "", nullptr, nullptr, &m);
            std::string built;
            for (const auto* it : o.in(core::Segment::Generated))
                if (it->id == "list.1")
                    for (const auto& fl : it->fields) if (fl.label == "Built") built = fl.value;
            check("outline: Built says what's out of date", built == "Out of date — \u201cSources\u201d listed, no such heading");
            const auto iss = L::preflight_issues(e, f, {});
            const auto it = std::find_if(iss.begin(), iss.end(), [](const L::Issue& i) { return i.kind == "stale_list"; });
            check("preflight: a warning at the contents frame, naming the list", it != iss.end() && it->level == L::Issue::Level::Warning &&
                                                                                it->item == "list.1" && it->frame == "frame.16" &&
                                                                                it->page == e.as<core::GenList>("list.1")->placed_on);
        }
        // Export's "rebuild first", kept.
        auto e = d.settings().exp;
        check("export: rebuild first by default", e.rebuild);
        e.rebuild = false;
        core::doc::set_export(j, d, e);
        std::string err;
        core::Doc f;
        check("json: rebuild first kept", core::from_json(f, core::to_json(d), &err) && !f.settings().exp.rebuild, err);
    }

    {
        std::cout << "\n-- list levels (s088) --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto n0 = j.size();
        check("styles: Caption as level 2, one step", core::doc::set_list_styles(j, d, "list.1", {"style.2", "style.3"}) &&
                                                      j.size() == n0 + 1 &&
                                                      d.as<core::GenList>("list.1")->from_styles == std::vector<std::string>{"style.2", "style.3"});
        check("styles: the same -- no step", !core::doc::set_list_styles(j, d, "list.1", {"style.2", "style.3"}));
        check("styles: a character style or a repeat is dropped", core::doc::set_list_styles(j, d, "list.1", {"style.2", "style.5", "style.2"}) &&
                                                                  d.as<core::GenList>("list.1")->from_styles == std::vector<std::string>{"style.2"});
        j.undo();
        L::FlowCache flows;
        const auto en = L::list_entries(d, flows, "list.1");
        bool fig = false;
        for (const auto& e : en) fig = fig || (e.level == 2 && e.text.find("Fig. 1") == 0);
        check("entries: the caption at level 2, in reading order", en.size() == 3 && en[0].level == 1 && en[1].level == 1 && fig &&
                                                                   en[2].level == 2);
        const auto entry1 = d.as<core::Story>("story.9")->paras.at(1).style;
        const auto styles0 = d.ids_of<core::Style>().size();
        check("build: one step, with a level-2 style made", L::build_list(j, d, "list.1") && d.ids_of<core::Style>().size() == styles0 + 1);
        const auto& ps = d.as<core::Story>("story.9")->paras;
        const auto* l = d.as<core::GenList>("list.1");
        const auto* s2 = l->entry_styles.size() == 2 ? d.as<core::Style>(l->entry_styles[1]) : nullptr;
        const auto* s1 = d.as<core::Style>(entry1);
        check("build: the new style -- named for level 2, based on level 1, indented", s2 && s2->name == s1->name + " 2" &&
              s2->based_on == entry1 && s2->left_indent == s1->left_indent + 12 && s2->own == std::vector<std::string>{"left_indent", "tabs"} &&
              !s2->tabs.empty() && s2->tabs.front().pos == s1->tabs.front().pos - 12);
        check("build: level 2's entry in it, level 1's as before", ps.size() == 4 && ps[1].style == entry1 && ps[3].style == l->entry_styles[1] &&
                                                                   ps[3].text.find("Fig. 1") == 0);
        const auto styles1 = d.ids_of<core::Style>().size();
        check("rebuild: the style found again, not made twice", !L::build_list(j, d, "list.1") && d.ids_of<core::Style>().size() == styles1);
        j.undo();
        check("undo: the made style goes too", d.ids_of<core::Style>().size() == styles0);
        j.redo();
        // Bookmarks nest the same way.
        L::FlowCache f2;
        const auto bm = L::bookmarks(d, f2, {"list.1"});
        check("bookmarks: the caption under its chapter", bm.size() == 3 && bm[2].level == 2);
        std::string err;
        core::Doc f;
        check("json: the levels' styles kept", core::from_json(f, core::to_json(d), &err) &&
                                               f.as<core::GenList>("list.1")->entry_styles == d.as<core::GenList>("list.1")->entry_styles, err);
    }

    {
        std::cout << "\n-- list build (s087) --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        L::FlowCache flows;
        const auto en = L::list_entries(d, flows, "list.1");
        check("entries: each heading and its folio", en == std::vector<core::doc::ListEntry>{{"Chapter One", "1"}, {"Chapter Two", "3"}});
        const auto before = d.as<core::Story>("story.9")->paras;
        const auto n0 = j.size();
        check("build: one step", L::build_list(j, d, "list.1") && j.size() == n0 + 1);
        const auto& ps = d.as<core::Story>("story.9")->paras;
        check("build: the title kept, an entry a heading, in the entry style",
              ps.size() == 3 && ps[0].text == "Contents" && ps[1].text == "Chapter One\t1" && ps[2].text == "Chapter Two\t3" &&
              ps[1].style == before[1].style && ps[2].style == before[1].style);
        check("build: counted -- not stale", d.as<core::GenList>("list.1")->built_entries == 2);
        check("build: again, no change -- no step", !L::build_list(j, d, "list.1") && j.size() == n0 + 1);
        L::FlowCache f2;
        check("build: the contents links still found", L::contents_links(d, f2).size() == 2);
        j.undo();
        check("build: undone -- as it was", d.as<core::Story>("story.9")->paras == before);
        j.redo();
        // A Contents list with no story: Build makes one, in a frame on its page.
        {
            auto l = *d.as<core::GenList>("list.1");
            l.story.clear();
            l.placed_on = d.pages()[0];
            core::Doc e = d;
            core::DocJournal je{e};
            e.set_door([&je] { return je.open(); });
            e.put("list.1", l);
            const auto frames0 = e.frames_on(l.placed_on).size();
            check("build, no story: made", L::build_list(je, e, "list.1") && je.size() == 1);
            const auto* nl = e.as<core::GenList>("list.1");
            const auto* ns = nl ? e.as<core::Story>(nl->story) : nullptr;
            check("build, no story: a story with the entries, in a new frame on its page",
                  ns && ns->paras.size() == 2 && ns->paras[0].text == "Chapter One\t1" &&
                  e.frames_on(l.placed_on).size() == frames0 + 1);
            const auto mr = core::margin_rect(e, l.placed_on);
            bool inside = false;
            for (const auto& fid : e.frames_on(l.placed_on))
                if (const auto* f = e.as<core::Frame>(fid); f->story == nl->story)
                    inside = mr.w < 1 || (f->x == mr.x && f->y == mr.y && f->w == mr.w);
            check("build, no story: the frame across the margins", inside);
            je.undo();
            check("build, no story: undone -- story and frame gone", e.as<core::GenList>("list.1")->story.empty() &&
                                                                    e.frames_on(l.placed_on).size() == frames0);
            l.placed_on.clear();
            e.put("list.1", l);
            check("build: no story, no page -- nothing", !L::build_list(je, e, "list.1"));
        }
        std::string err;
        core::Doc f;
        check("json: the list's story kept", core::from_json(f, core::to_json(d), &err) && f.as<core::GenList>("list.1")->story == "story.9", err);
    }

    {
        std::cout << "\n-- export summary (s086) --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::load_sample(d);
        L::FlowCache flows;
        L::PdfOptions o;
        const auto has = [](const std::string& hay, const std::string& n) { return hay.find(n) != std::string::npos; };
        auto s = L::export_summary(d, flows, o, true, "");
        check("summary: every page, its size and bleed", s.pages == 16 && s.sheets == 16 && has(s.size, "16 pages · ") &&
                                                         !has(s.size, "sheet") && has(s.size, "bleed"));
        check("summary: no profile -- device CMYK", has(s.colour, "Device CMYK"));
        check("summary: the bookmarks and links counted (s123: and the sample's note's two)",
              s.bookmarks == 2 && s.links == 2 && s.note_links == 2 && s.nav == "2 bookmarks · 2 contents links · 2 note links", s.nav);
        o.spreads = true;
        s = L::export_summary(d, flows, o, true, "");
        check("summary: spreads -- pages to sheets", s.sheets == 9 && has(s.size, "16 pages → 9 sheets"));
        o.only = core::pages_in_range(d, "ii-7");
        o.mark_lists = {"list.1", "list.2"};
        s = L::export_summary(d, flows, o, true, "");
        check("summary: a range -- its pages, its sheets, its bookmarks", s.pages == 10 && s.sheets == 5 && s.bookmarks == 2);
        o.only.clear();
        s = L::export_summary(d, flows, o, true, "");
        check("summary: both lists -- the group and its entries", s.bookmarks == 6);
        o.convert = true;
        o.perceptual = true;
        s = L::export_summary(d, flows, o, true, "Press");
        check("summary: converting, and how", s.colour == "CMYK for Press — RGB converted, perceptual");
        o.convert = false;
        o.standard = L::plate_standard::X4;
        s = L::export_summary(d, flows, o, true, "Press");
        check("summary: PDF/X-4 -- RGB kept, links left out", has(s.colour, "PDF/X-4 · ") && has(s.colour, "RGB kept") &&
                                                              s.links == 0 && has(s.nav, "left out"));
        o.standard = L::plate_standard::X1a;
        check("summary: X-1a always converts", has(L::export_summary(d, flows, o, true, "Press").colour, "RGB converted"));
        o.standard = L::plate_standard::None;
        o.bookmarks = false;
        s = L::export_summary(d, flows, o, false, "Press");
        check("summary: screen is RGB; bookmarks off -- none", s.colour == "RGB, for screen" && s.bookmarks == 0 &&
                                                              has(s.nav, "No bookmarks"));
    }

    {
        std::cout << "\n-- export intent (s085) --\n";
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        auto e = d.settings().exp;
        check("intent: relative colorimetric by default", !e.perceptual);
        e.perceptual = true;
        const auto n0 = j.size();
        check("intent: kept with the document, one step", core::doc::set_export(j, d, e) && j.size() == n0 + 1 && d.settings().exp.perceptual);
        j.undo();
        check("intent: undone", !d.settings().exp.perceptual);
        j.redo();
        std::string err;
        core::Doc f;
        check("json: the intent kept", core::from_json(f, core::to_json(d), &err) && f.settings().exp.perceptual, err);
        core::Doc g, h;
        core::load_sample(g);
        const auto js = core::to_json(g);
        check("json: relative -> no key, read back relative", js.find("\"perceptual\"") == std::string::npos &&
                                                              core::from_json(h, js, &err) && !h.settings().exp.perceptual, err);
        // The export says which intent it converted with (when a profile is installed).
        std::string icc;
        for (const auto& p : plate::find_cmyk_profiles({"/usr/share/color/icc", "/usr/share/ghostscript/iccprofiles",
                                                        "/usr/share/texlive/texmf-dist/tex/generic/colorprofiles",
                                                        "/usr/share/texmf/tex/generic/colorprofiles"}))
            if (icc.empty()) icc = p.path;
        if (icc.empty()) {
            std::cout << "  (skip intent export: no CMYK profile installed)\n";
        } else {
            namespace L = composix::layout;
            const L::ImageSource src = [](const core::Asset&, double, double) -> cairo_surface_t* { return nullptr; };
            L::PdfOptions opt;
            opt.profile = icc;
            opt.convert = true;
            opt.perceptual = true;
            opt.only = core::pages_in_range(d, "iii");
            L::PdfReport rep;
            const auto out = (std::filesystem::temp_directory_path() / "composix_selftest_s085.pdf").string();
            const bool ok = L::export_print_pdf(d, out, opt, src, &rep, &err);
            check("export: the report names the intent", ok && !rep.warnings.empty() &&
                                                         rep.warnings.front().find("perceptual") != std::string::npos, err);
            std::error_code ec;
            std::filesystem::remove(out, ec);
        }
    }

    {
        std::cout << "\n-- PDF spreads (s084) --\n";
        namespace L = composix::layout;
        namespace fs = std::filesystem;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto pages = d.pages();
        const auto singles = L::pdf_sheets(d, pages, false);
        check("sheets: one a page without spreads", singles.size() == pages.size() && singles[1].label == d.folio(pages[1]) &&
                                                    singles[1].pages.size() == 1 && singles[1].x[0] == 0);
        const auto sp = L::pdf_sheets(d, pages, true);
        const auto pw = [&](const std::string& p) { return d.as<core::Page>(p)->w; };
        bool same = sp.size() == core::spreads(d).size();
        for (std::size_t k = 0; same && k < sp.size(); ++k) same = sp[k].pages == core::spreads(d)[k];
        check("sheets: one a spread, as the Pages view pairs them", same);
        check("sheets: the first page alone", !sp.empty() && sp[0].pages.size() == 1 && sp[0].label == "i");
        check("sheets: side by side, widths added, labelled", sp.size() > 1 && sp[1].pages.size() == 2 &&
              sp[1].x[1] == pw(sp[1].pages[0]) && sp[1].w == pw(sp[1].pages[0]) + pw(sp[1].pages[1]) &&
              sp[1].label == "ii–iii");
        bool wide = false;   // the foldout's spread is wider than two plain pages
        for (const auto& s : sp) wide = wide || s.w > 2 * pw(pages[0]) + 1;
        check("sheets: a foldout widens its spread", wide);
        const auto cut = L::pdf_sheets(d, core::pages_in_range(d, "iii-1"), true);
        check("sheets: a range keeps just its pages of each spread", cut.size() == 2 && cut[0].label == "iii" &&
                                                                     cut[1].label == "iv–1");
        const auto at = L::sheet_of(sp, core::pages_in_range(d, "1").front());
        check("sheet_of: page 1 -- the iv-1 sheet, right of iv", at.sheet >= 0 && sp[static_cast<std::size_t>(at.sheet)].label == "iv–1" &&
                                                                 at.x == pw(core::pages_in_range(d, "iv").front()));
        check("sheet_of: a page not written", L::sheet_of(cut, pages[0]).sheet == -1);
        {
            core::Doc e = d;
            auto st = e.settings();
            st.facing = false;
            e.put("doc", st);
            for (const auto& mid : e.ids_of<core::Master>()) {   // s147: facing is each master's
                auto m = *e.as<core::Master>(mid);
                m.facing = false;
                e.put(mid, m);
            }
            check("sheets: no facing pages -- one a page anyway", L::pdf_sheets(e, e.pages(), true).size() == e.pages().size());
        }
        const L::ImageSource src = [](const core::Asset&, double, double) -> cairo_surface_t* { return nullptr; };
        const auto base = fs::temp_directory_path() / "composix_selftest_s084";
        std::error_code ec;
        fs::create_directories(base, ec);
        const auto slurp = [](const std::string& f) {
            std::ifstream in(f, std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            return ss.str();
        };
        const auto count = [](const std::string& hay, const std::string& n) {
            int k = 0;
            for (auto at2 = hay.find(n); at2 != std::string::npos; at2 = hay.find(n, at2 + 1)) ++k;
            return k;
        };
        L::PdfOptions opt;
        opt.compress = false;
        opt.spreads = true;
        L::PdfReport rep;
        std::string err;
        const auto out = (base / "spreads.pdf").string();
        const bool ok = L::export_print_pdf(d, out, opt, src, &rep, &err);
        const auto pdf = slurp(out);
        const double b = d.settings().bleed;
        char box[96];
        std::snprintf(box, sizeof box, "/MediaBox [0 0 %g %g]", 2 * pw(pages[1]) + 2 * b, d.as<core::Page>(pages[1])->h + 2 * b);
        check("print: one PDF page a spread, its pages still counted", ok && count(pdf, "/Type /Page /Parent") == static_cast<int>(sp.size()) &&
                                                                     rep.pages == static_cast<int>(pages.size()), err);
        check("print: a spread's media is both pages and the outer bleed", pdf.find(box) != std::string::npos);
        L::FlowCache flows;
        const auto lk = L::contents_links(d, flows);
        const auto iii = L::sheet_of(sp, lk.empty() ? std::string() : lk[0].page);
        char rect[48];
        std::snprintf(rect, sizeof rect, "/Rect [%g ", b + iii.x + (lk.empty() ? 0 : lk[0].box.x));
        check("print: the contents links moved with their page", !lk.empty() && iii.x > 0 &&
                                                                count(pdf, "/Subtype /Link") == 2 + static_cast<int>(L::note_links(d, flows).size()) &&
                                                                pdf.find(rect) != std::string::npos);
        check("print: the outline still there", pdf.find("(Chapter One)") != std::string::npos);
        L::PdfOptions so;
        so.spreads = true;
        const auto sout = (base / "spreads_screen.pdf").string();
        check("screen: spreads too", L::export_pdf(d, sout, so, src, &rep, &err) && rep.pages == static_cast<int>(pages.size()), err);
        fs::remove_all(base, ec);
        // Kept with the document.
        auto e = d.settings().exp;
        check("export: spreads off by default", !e.spreads);
        e.spreads = true;
        const auto n0 = j.size();
        check("export: spreads kept, one step", core::doc::set_export(j, d, e) && j.size() == n0 + 1 && d.settings().exp.spreads);
        core::Doc f;
        check("json: spreads kept", core::from_json(f, core::to_json(d), &err) && f.settings().exp.spreads, err);
        core::Doc g, h;
        core::load_sample(g);
        const auto js = core::to_json(g);
        check("json: off -> no key, read back off", js.find("\"spreads\"") == std::string::npos && core::from_json(h, js, &err) &&
                                                    !h.settings().exp.spreads, err);
    }

    {
        std::cout << "\n-- contents links (s083) --\n";
        namespace L = composix::layout;
        namespace fs = std::filesystem;
        core::Doc d;
        core::load_sample(d);
        L::FlowCache flows;
        const auto links = L::contents_links(d, flows);
        const auto toc = L::bookmarks(d, flows, {"list.1"});
        const auto* fr = d.as<core::Frame>("frame.16");
        check("links: the two chapter entries (Sources names no heading)", links.size() == 2 && toc.size() == 2);
        bool on_page = links.size() == 2, inside = on_page, to_heads = on_page;
        for (std::size_t k = 0; k < links.size(); ++k) {
            const auto& b = links[k].box;
            on_page = on_page && links[k].page == d.as<core::GenList>("list.1")->placed_on && d.folio(links[k].page) == "iii";
            inside = inside && b.w > 0 && b.h > 0 && b.x >= fr->x - 0.01 && b.r() <= fr->x + fr->w + 0.01 &&
                     b.y >= fr->y - 0.01 && b.b() <= fr->y + fr->h + 0.01;
            to_heads = to_heads && links[k].to == toc[k].page && links[k].to_y == toc[k].y;
        }
        check("links: on the contents page, inside its frame", on_page && inside);
        check("links: each to where its heading lands", to_heads && d.folio(links[0].to) == "1" && d.folio(links[1].to) == "3");
        check("links: in order down the page", links.size() == 2 && links[0].box.b() <= links[1].box.y + 0.01);
        {   // A second "Chapter One" entry: one heading, so one link.
            auto st = *d.as<core::Story>("story.9");
            st.paras.push_back(st.paras.at(1));
            core::Doc e = d;
            e.put("story.9", st);
            L::FlowCache f2;
            check("links: the k-th entry to the k-th heading -- no second target", L::contents_links(e, f2).size() == 2);
            auto l = *e.as<core::GenList>("list.1");
            l.placed_on.clear();
            e.put("list.1", l);
            L::FlowCache f3;
            check("links: a list not placed -- none", L::contents_links(e, f3).empty());
        }
        const auto base = fs::temp_directory_path() / "composix_selftest_s083";
        std::error_code ec;
        fs::create_directories(base, ec);
        const auto slurp = [](const std::string& f) {
            std::ifstream in(f, std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            return ss.str();
        };
        const auto count = [](const std::string& hay, const std::string& n) {
            int k = 0;
            for (auto at = hay.find(n); at != std::string::npos; at = hay.find(n, at + 1)) ++k;
            return k;
        };
        {   // Plate: an invisible /Link annotation, a missing page dropped.
            plate::Document pd;
            pd.set_compress(false);
            pd.add_page(100, 200);
            pd.add_page(100, 200);
            pd.add_link(0, 10, 20, 30, 12, 1, 50);
            pd.add_link(0, 10, 40, 30, 12, 7, 0);   // no such page
            const auto f = (base / "link.pdf").string();
            std::string err;
            const bool ok = pd.save(f, &err);
            const auto p = slurp(f);
            check("plate: a link -- its box flipped, going to the page", ok && count(p, "/Subtype /Link") == 1 &&
                  p.find("/Rect [10 168 40 180]") != std::string::npos && p.find("/XYZ 0 150 null") != std::string::npos &&
                  p.find("/Border [0 0 0]") != std::string::npos && p.find("/Annots [") != std::string::npos, err);
        }
        const L::ImageSource src = [](const core::Asset&, double, double) -> cairo_surface_t* { return nullptr; };
        L::PdfOptions opt;
        opt.compress = false;
        L::PdfReport rep;
        std::string err;
        const auto out = (base / "toc.pdf").string();
        check("print: the contents page's two links", L::export_print_pdf(d, out, opt, src, &rep, &err) &&
                                                      count(slurp(out), "/Subtype /Link") == 2 + [&] {   // s123: and the notes'
                                                          L::FlowCache nf;
                                                          return static_cast<int>(L::note_links(d, nf).size());
                                                      }(), err);
        opt.only = core::pages_in_range(d, "iii");
        check("print: the targets not written -- no links", L::export_print_pdf(d, out, opt, src, &rep, &err) &&
                                                            count(slurp(out), "/Subtype /Link") == 0, err);
        L::PdfOptions so;
        const auto sout = (base / "toc_screen.pdf").string();
        check("screen: the links too", L::export_pdf(d, sout, so, src, &rep, &err) && count(slurp(sout), "/Link") >= 2, err);
        fs::remove_all(base, ec);
    }

    {
        std::cout << "\n-- bookmarks from several lists (s082) --\n";
        namespace L = composix::layout;
        namespace fs = std::filesystem;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        L::FlowCache flows;
        check("lists: the ones gathering styles, in list order", L::bookmark_lists(d) == std::vector<std::string>{"list.1", "list.2"});
        const auto toc = L::bookmarks(d, flows, {"list.1"});
        check("default (none picked) = the contents, as s054", L::bookmarks(d, flows, {}) == L::bookmarks(d, flows) &&
                                                               L::bookmarks(d, flows, {}) == toc && !toc.empty());
        check("a list that's gone -> the default", L::bookmarks(d, flows, {"list.99"}) == toc);
        const auto bib = L::bookmarks(d, flows, {"list.2"});
        const bool bib_flat = !bib.empty() && std::all_of(bib.begin(), bib.end(), [](const L::Bookmark& b) { return b.level == 1; });
        bool chap = false;
        for (const auto& b : bib) chap = chap || b.title.find("Chapter") != std::string::npos;
        check("just the bibliography: its entries, at the top level", bib_flat && !chap && bib.size() == 3);
        check("two columns: in story order, not by height", bib.size() == 3 && bib[0].title.find("Almanac") == 0 &&
                                                            bib[1].title.find("Harbor") == 0 && bib[2].title.find("Letters") == 0);
        const auto both = L::bookmarks(d, flows, {"list.1", "list.2"});
        const bool grouped = both.size() == toc.size() + 1 + bib.size() &&
                             std::equal(toc.begin(), toc.end(), both.begin()) &&
                             both[toc.size()].title == "Bibliography" && both[toc.size()].level == 1 &&
                             both[toc.size()].page == bib.front().page && both[toc.size()].y == bib.front().y;
        bool nested = grouped;
        for (std::size_t k = 0; grouped && k < bib.size(); ++k)
            nested = nested && both[toc.size() + 1 + k].title == bib[k].title && both[toc.size() + 1 + k].level == 2;
        check("both: the contents on top, the bibliography under its name", grouped && nested);
        check("the lists' order, not the picking order", L::bookmarks(d, flows, {"list.2", "list.1"}) == both);
        d.put("list.3", core::GenList{"Index", core::ListKind::Index, 3, {}, "", -1});
        check("a list gathering no styles isn't offered", L::bookmark_lists(d).size() == 2);
        // A range without the bibliography's page: no empty group.
        std::vector<std::string> early;
        for (const auto& p : d.pages()) if (p != bib.front().page) early.push_back(p);
        const auto cut = L::bookmarks(d, flows, {"list.1", "list.2"}, early);
        bool no_bib = true;
        for (const auto& b : cut) no_bib = no_bib && b.page != bib.front().page;
        check("a range: entries off it left out, a group only where it has some", no_bib &&
              (cut.empty() || cut.back().title != "Bibliography"));
        // Into the PDFs.
        const L::ImageSource src = [](const core::Asset&, double, double) -> cairo_surface_t* { return nullptr; };
        const auto base = fs::temp_directory_path() / "composix_selftest_s082";
        std::error_code ec;
        fs::create_directories(base, ec);
        const auto slurp = [](const std::string& f) {
            std::ifstream in(f, std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            return ss.str();
        };
        L::PdfOptions opt;
        opt.compress = false;
        opt.mark_lists = {"list.1", "list.2"};
        L::PdfReport rep;
        std::string err;
        const auto out = (base / "lists.pdf").string();
        const bool okp = L::export_print_pdf(d, out, opt, src, &rep, &err);
        const auto pdf = slurp(out);
        check("print: the group and its entries in the outline", okp && pdf.find("(Bibliography)") != std::string::npos &&
                                                                 pdf.find("(Chapter One)") != std::string::npos &&
                                                                 pdf.find("(Letters of the Surveyor General.)") != std::string::npos, err);
        L::PdfOptions so;
        so.mark_lists = {"list.2"};
        const auto sout = (base / "lists_screen.pdf").string();
        check("screen: exports from the bibliography alone", L::export_pdf(d, sout, so, src, &rep, &err) &&
                                                             slurp(sout).find("/Outlines") != std::string::npos, err);
        fs::remove_all(base, ec);
        // Kept with the document: one step, and in the file.
        auto e = d.settings().exp;
        check("export: no lists picked by default", e.mark_lists.empty());
        e.mark_lists = {"list.1", "list.2"};
        const auto n0 = j.size();
        check("export: the lists kept, one step", core::doc::set_export(j, d, e) && j.size() == n0 + 1 &&
                                                  d.settings().exp.mark_lists == e.mark_lists);
        core::Doc f;
        check("json: the lists kept", core::from_json(f, core::to_json(d), &err) && f.settings().exp.mark_lists == e.mark_lists, err);
        core::Doc g;
        core::load_sample(g);
        const auto js = core::to_json(g);
        core::Doc h;
        check("json: none picked -> no key, read back as none", js.find("mark_lists") == std::string::npos &&
                                                                core::from_json(h, js, &err) && h.settings().exp.mark_lists.empty(), err);
    }

    {
        std::cout << "\n-- paste through Next style (s081) --\n";
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        // Two paragraph styles: A -> next B, B -> next B (none set: itself).
        core::Doc::Id A, B;
        for (const auto& id : d.ids_of<core::Style>())
            if (d.as<core::Style>(id)->kind == core::StyleKind::Paragraph) { if (A.empty()) A = id; else if (B.empty() && id != A) B = id; }
        check("sample: two paragraph styles", !A.empty() && !B.empty());
        core::doc::edit_styles(j, d, {A}, "Next style", [&](core::Style& s) { s.next = B; });
        core::doc::edit_styles(j, d, {B}, "Next style", [&](core::Style& s) { s.next.clear(); });
        // A paragraph in A, at its end: paste three lines.
        core::doc::set_para_style(j, d, "story.1", {1, 0}, {1, 0}, A);
        const int end = static_cast<int>(d.as<core::Story>("story.1")->paras.at(1).text.size());
        const auto n0 = d.as<core::Story>("story.1")->paras.size();
        core::doc::edit_text(j, d, "story.1", {1, end}, {1, end}, "\nTitle two\nBody one\nBody two", "Paste");
        const auto& ps = d.as<core::Story>("story.1")->paras;
        check("paste: three new paragraphs", ps.size() == n0 + 3);
        check("paste at an end: A, then its Next (B), then B's (B)", ps.at(1).style == A && ps.at(2).style == B &&
                                                                     ps.at(3).style == B && ps.at(4).style == B);
        // Into the middle of an A paragraph: the last piece is that paragraph's rest -- it stays A.
        core::doc::set_para_style(j, d, "story.1", {1, 0}, {1, 0}, A);
        core::doc::edit_text(j, d, "story.1", {1, 3}, {1, 3}, "x\ny\nz", "Paste");
        const auto& p2 = d.as<core::Story>("story.1")->paras;
        check("paste mid-paragraph: the middle takes Next, the rest keeps A", p2.at(1).style == A && p2.at(2).style == B && p2.at(3).style == A);
        // Enter alone, as before: at the end -> Next; in the middle -> the same.
        core::doc::set_para_style(j, d, "story.1", {1, 0}, {1, 0}, A);
        const int e1 = static_cast<int>(d.as<core::Story>("story.1")->paras.at(1).text.size());
        core::doc::edit_text(j, d, "story.1", {1, e1}, {1, e1}, "\n");
        check("Enter at the end: Next style", d.as<core::Story>("story.1")->paras.at(2).style == B);
        core::doc::edit_text(j, d, "story.1", {1, 2}, {1, 2}, "\n");
        check("Enter in the middle: the same style", d.as<core::Story>("story.1")->paras.at(2).style == A);
    }

    {
        std::cout << "\n-- New sheet memory (s079) --\n";
        core::NewDoc n;
        n.trim_w = 5.5 * core::kPt;
        n.trim_h = 8.5 * core::kPt;
        n.pages = 48;
        n.facing = false;
        n.margins = {40, 50, 60, 70};
        n.columns = 2;
        n.gutter = 14;
        n.bleed = 0;
        n.text_frames = false;
        n.folios = false;
        n.units = core::Units::Millimetres;
        core::AppSettings st{{"window.width", "1400"}};
        core::new_to_settings(n, st);
        auto back = core::new_from_settings(st);
        back.title = n.title;
        check("new sheet: every choice comes back", back == n);
        check("new sheet: other settings left alone", st.at("window.width") == "1400");
        check("new sheet: nothing saved -> the defaults", core::new_from_settings({}) == core::NewDoc{});
        core::AppSettings bad{{"new.pages", "lots"}, {"new.trim_w", "1e9"}, {"new.units", "7"}, {"new.columns", "3.5x"}};
        const auto b = core::new_from_settings(bad);
        check("new sheet: unreadable keys keep defaults, numbers held in range",
              b.pages == core::NewDoc{}.pages && b.trim_w == 5000 && b.units == core::Units::Points && b.columns == core::NewDoc{}.columns);
        core::Doc d;
        check("new document: in the chosen unit", core::new_document(d, n) && d.settings().units == core::Units::Millimetres);
    }

    {
        std::cout << "\n-- saved queries (s078) --\n";
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        check("sample: one saved query", d.settings().queries.size() == 1 && d.settings().queries[0].name == "Double spaces");
        core::SavedQuery q{"  Dashes ", "--", "\u2014"};
        q.mode = 1;
        check("save: one step, the name trimmed, kept by name", core::doc::save_query(j, d, q) && j.size() == 1 &&
                                                                 d.settings().queries.size() == 2 && d.settings().queries[0].name == "Dashes");
        check("save: the same again is no step", !core::doc::save_query(j, d, d.settings().queries[0]));
        q.name = "Dashes";
        q.with = "\u2013";
        check("save: the same name replaces", core::doc::save_query(j, d, q) && d.settings().queries.size() == 2 &&
                                              d.settings().queries[0].with == "\u2013");
        check("save: no name, no query", !core::doc::save_query(j, d, {"   ", "x"}));
        std::string err;
        core::Doc e;
        check("json: queries kept, every setting", core::from_json(e, core::to_json(d), &err) && e.settings().queries == d.settings().queries, err);
        check("delete: one step; unknown name none", core::doc::delete_query(j, d, "Dashes") && d.settings().queries.size() == 1 &&
                                                     !core::doc::delete_query(j, d, "Dashes"));
        j.undo();
        check("undo: back", d.settings().queries.size() == 2);
    }

    {
        std::cout << "\n-- export memory (s077) --\n";
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        check("export: all pages and bookmarks by default", d.settings().exp.range.empty() && d.settings().exp.bookmarks);
        auto e = d.settings().exp;
        e.range = "iii-7, 12";
        e.bookmarks = false;
        check("export: the range and bookmarks kept, one step", core::doc::set_export(j, d, e) && j.size() == 1 &&
                                                                d.settings().exp.range == "iii-7, 12" && !d.settings().exp.bookmarks);
        check("export: the same again is no step", !core::doc::set_export(j, d, e));
        std::string err;
        core::Doc f;
        check("json: range and bookmarks kept", core::from_json(f, core::to_json(d), &err) &&
                                                f.settings().exp.range == "iii-7, 12" && !f.settings().exp.bookmarks, err);
        check("json: an older file (no keys) -> all pages, bookmarks on", [] {
            core::Doc g;
            core::load_sample(g);
            auto js = core::to_json(g);   // (keys are written sorted: cut the two new ones out)
            bool cut = false;
            for (const std::string k : {"\"bookmarks\": true,", "\"range\": \"\","})
                if (const auto at = js.find(k); at != std::string::npos) { js.erase(at, k.size()); cut = true; }
            core::Doc h;
            std::string err2;
            return cut && js.find("\"bookmarks\"") == std::string::npos && core::from_json(h, js, &err2) &&
                   h.settings().exp.range.empty() && h.settings().exp.bookmarks;
        }());
        check("export: the kept range still reads", !core::pages_in_range(d, d.settings().exp.range).empty());
    }

    {
        std::cout << "\n-- picture offsets (s076) --\n";
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto& f0 = *d.as<core::Frame>("frame.11");
        const auto& a = *d.as<core::Asset>(f0.asset);
        const double iw = a.px_w > 0 ? a.px_w : 1, ih = a.px_h > 0 ? a.px_h : 1;
        const auto r0 = core::picture_rect({0, 0, f0.w, f0.h}, f0, iw, ih);
        check("sample: the lighthouse is fitted, not by hand", f0.fit != core::ImageFit::Custom);
        check("offset X: one step, by hand, Y and size kept",
              core::doc::set_picture_offset(j, d, {"frame.11"}, 0, -6) && j.size() == 1);
        const auto& f1 = *d.as<core::Frame>("frame.11");
        const auto r1 = core::picture_rect({0, 0, f1.w, f1.h}, f1, iw, ih);
        check("offset X: there", f1.fit == core::ImageFit::Custom && std::abs(r1.x + 6) < 1e-9 && std::abs(r1.y - r0.y) < 1e-9 &&
                                 std::abs(r1.w - r0.w) < 1e-9);
        check("offset Y: there", core::doc::set_picture_offset(j, d, {"frame.11"}, 1, 4) &&
                                 std::abs(d.as<core::Frame>("frame.11")->pic_y - 4) < 1e-9);
        check("offset: the same again is no step", !core::doc::set_picture_offset(j, d, {"frame.11"}, 1, 4));
        check("offset: not on a text frame", !core::doc::set_picture_offset(j, d, {"frame.1"}, 0, 1));
        const auto items = core::frame_items(d, {"frame.11"});
        const core::Field* off = nullptr;
        for (const auto& f : items.at(0).fields) if (f.edit == "pic_off") off = &f;
        check("inspector: Offset X / Y typed, inches", off && off->cols.size() == 2 && off->unit == 72 && off->value == "-0.083\t0.056",
              off ? off->value : "");
        j.undo();
        j.undo();
        check("undo: fitted as before", d.as<core::Frame>("frame.11")->fit == f0.fit);
    }

    {
        std::cout << "\n-- no break (s075) --\n";
        namespace L = composix::layout;
        //            0123456789012345678901234
        const std::string t = "one two three four five";
        const std::vector<core::ByteSpan> sp{{8, 18}};   // "three four"
        check("keep together: a break inside goes back to the span's word start",
              core::keep_together(t, sp, 0, 14) == 8);
        check("keep together: a break outside stays", core::keep_together(t, sp, 0, 8) == 8 && core::keep_together(t, sp, 0, 19) == 19);
        check("keep together: a span starting the line breaks as usual", core::keep_together(t, sp, 8, 6) == 6);
        check("keep together: mid-word span start -> back to that word", core::keep_together("ab 12km x", {{5, 7}}, 0, 6) == 3);
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        // A character style with No break, over a stretch of the opening paragraph.
        core::Doc::Id cs;
        for (const auto& id : d.ids_of<core::Style>())
            if (d.as<core::Style>(id)->kind == core::StyleKind::Character) { cs = id; break; }
        check("sample: a character style", !cs.empty());
        check("no break: one step on the style", core::doc::edit_styles(j, d, {cs}, "No break", [](core::Style& st) { st.no_break = true; }));
        // The full column (the lighthouse's wrap off), so 30 bytes surely fit one line.
        core::doc::edit_frames(j, d, {"frame.11"}, "Text wrap", [](core::Frame& f) { f.wrap = core::Wrap::None; });
        const auto& para = d.as<core::Story>("story.1")->paras.at(1);
        // Some 30 bytes from the 3rd word on, ending at a word's end.
        int a = 0, words = 0;
        while (a < static_cast<int>(para.text.size()) && words < 3) if (para.text[static_cast<std::size_t>(a++)] == ' ') ++words;
        int b = std::min(static_cast<int>(para.text.size()), a + 30);
        while (b < static_cast<int>(para.text.size()) && para.text[static_cast<std::size_t>(b)] != ' ') ++b;
        core::doc::set_char_style(j, d, "story.1", {1, a}, {1, b}, cs);
        const auto spans = core::no_break_spans(d, d.as<core::Story>("story.1")->paras.at(1));
        check("no break: the run is a span", !spans.empty() && spans[0].a <= a && spans[0].b >= b);
        bool never = !spans.empty(), whole = false;
        for (const auto& fr : L::flow_story(d, "story.1").frames)
            for (const auto& l : fr.lines) {
                if (l.para != 1) continue;
                for (const auto& s : spans) {
                    if (l.end > s.a && l.end < s.b && l.start < s.a) never = false;
                    whole = whole || (l.start <= s.a && l.end >= s.b);
                }
            }
        check("no break: no line ends inside it", never);
        check("no break: it sits whole on one line (it's narrower than the column)", whole);
        const auto o = core::project(d, "");
        bool row = false;
        for (const auto& f : o.find(cs)->fields) row = row || (f.edit == "nobreak" && f.value == "On");
        check("inspector: a character style's No break", row);
        std::string err;
        core::Doc e;
        check("json: no break kept", core::from_json(e, core::to_json(d), &err) && e.as<core::Style>(cs)->no_break, err);
    }

    {
        std::cout << "\n-- units (s073) --\n";
        using U = core::Units;
        check("units: points per unit", core::unit_pt(U::Inches) == 72 && core::unit_pt(U::Points) == 1 &&
                                        core::unit_pt(U::Picas) == 12 && std::abs(core::unit_pt(U::Millimetres) * 25.4 - 72) < 1e-9);
        check("units: lengths as text", core::length_text(54, U::Inches) == "0.75 in" && core::length_text(54, U::Points) == "54 pt" &&
                                        core::length_text(54, U::Picas) == "4p6" && core::length_text(72, U::Picas) == "6p" &&
                                        core::length_text(72, U::Millimetres) == "25.4 mm",
              core::length_text(54, U::Picas) + " / " + core::length_text(72, U::Millimetres));
        // Every unit reads back: what's shown, typed with that unit, is the same length.
        bool round = true;
        for (U u : {U::Inches, U::Millimetres, U::Picas, U::Points})
            for (double v : {0.0, 9.0, 54.0, 431.25, 648.0}) {
                const auto back = core::parse_length(core::length_in(v, u), core::unit_pt(u));
                round = round && back && std::abs(*back - v) < 0.01 * core::unit_pt(u) + 1e-9;
            }
        check("units: what's shown types back to the same length", round);
        check("units: size text", core::size_text(432, 648, 0, U::Millimetres) == "152.4 × 228.6 mm" &&
                                  core::size_text(432, 648, 0, U::Picas) == "36p × 54p");
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        check("units: one step; Millimetres", core::doc::set_units(j, d, U::Millimetres) && d.settings().units == U::Millimetres &&
                                              !core::doc::set_units(j, d, U::Millimetres));
        {   // s154: X / Y from the page's own corner here (Page origin)
            core::Doc::Loading load(d);
            auto st = d.settings();
            st.ruler_origin = core::RulerOrigin::Page;
            d.put("doc", st);
        }
        const auto items = core::frame_items(d, {"frame.1"});
        const core::Field* b = nullptr;
        for (const auto& f : items.at(0).fields) if (f.edit == "bounds") b = &f;
        check("inspector: Bounds in mm, a bare number in mm", b && b->label == "Bounds (mm)" && std::abs(b->unit - 72.0 / 25.4) < 1e-9 &&
                                                              b->value.rfind("19.05\t", 0) == 0, b ? b->value : "");
        const auto o = core::project(d, "");
        bool trim = false, units = false;
        for (const auto& f : o.find("doc")->fields) units = units || (f.edit == "dunits" && f.value == "Millimetres" && f.choices.size() == 4);
        for (const auto& f : o.find(d.ids_of<core::Master>().front())->fields)   // s148: the size is the master's
            trim = trim || (f.edit == "msize" && f.label == "Size (mm)" && f.value == "152.4\t228.6");
        check("inspector: a master's Size in mm, Units offered", trim && units);
        std::string err;
        core::Doc e;
        check("json: units kept", core::from_json(e, core::to_json(d), &err) && e.settings().units == U::Millimetres, err);
        j.undo();
        check("units: undo -- inches again", d.settings().units == U::Inches);
    }

    {
        std::cout << "\n-- override all (s072) --\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        // Two pages with master items showing (the sample's folios: one each).
        std::vector<core::Doc::Id> pages;
        for (const auto& p : d.pages())
            if (!core::doc::overridable_items(d, p).empty() && pages.size() < 2) pages.push_back(p);
        check("sample: two pages with master items", pages.size() == 2);
        if (pages.size() == 2) {
            const auto open0 = core::doc::overridable_items(d, pages[0]);
            const auto open1 = core::doc::overridable_items(d, pages[1]);
            const auto steps = j.size();
            const auto made = D::override_all(j, d, pages);
            check("override all: both pages, ONE step, a copy each", j.size() == steps + 1 && made.size() == open0.size() + open1.size());
            bool linked = made.size() == open0.size() + open1.size();
            for (std::size_t k = 0; linked && k < open0.size(); ++k)
                linked = d.as<core::Frame>(made[k])->overrides == open0[k] && d.as<core::Frame>(made[k])->owner == pages[0];
            check("override all: each copy on its page, overriding its item", linked);
            check("override all: none left to override", core::doc::overridable_items(d, pages[0]).empty() &&
                                                         core::doc::overridable_items(d, pages[1]).empty() && D::override_all(j, d, pages).empty());
            const auto o = core::project(d, "");
            bool row = false, back = false;
            for (const auto& f : o.find(pages[0])->fields) {
                row = row || (f.label == "Master items" && f.value.find("overridden") != std::string::npos);
                if (f.edit == "pover") for (const auto& c : f.choices) back = back || c.id == "none";
            }
            check("inspector: the page counts them, offers Remove overrides", row && back);
            check("remove all: one step, all back", D::remove_all_overrides(j, d, pages) &&
                                                    core::doc::overridable_items(d, pages[0]) == open0 && core::doc::overridable_items(d, pages[1]) == open1);
            j.undo();
            check("undo: overridden again", core::doc::overridable_items(d, pages[0]).empty());
            j.undo();
            check("undo: as it was", core::doc::overridable_items(d, pages[0]) == open0 && core::doc::overridable_items(d, pages[1]) == open1);
        }
    }

    {
        std::cout << "\n-- typed tints (s071) --\n";
        check("percent: 40, 40%, 40 %, 12.5%", core::parse_percent("40") == 40.0 && core::parse_percent("40%") == 40.0 &&
                                                core::parse_percent(" 40 % ") == 40.0 && core::parse_percent("12.5%") == 12.5);
        check("percent: not a percentage", !core::parse_percent("") && !core::parse_percent("%") && !core::parse_percent("4o") &&
                                           !core::parse_percent("-5") && !core::parse_percent("1.2.3") && !core::parse_percent("12pt"));
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto field = [](const std::vector<core::Field>& fs, const std::string& key) -> const core::Field* {
            for (const auto& f : fs) if (f.edit == key) return &f;
            return nullptr;
        };
        core::doc::edit_frames(j, d, {"frame.1"}, "Fill", [&](core::Frame& f) { f.fill = d.ids_of<core::Swatch>().front(); });
        const auto items = core::frame_items(d, {"frame.1"});
        const auto* ft = field(items.at(0).fields, "fill_tint");
        const auto* op = field(items.at(0).fields, "opacity");
        check("inspector: Fill tint typed, a percentage, presets kept", ft && ft->percent && ft->unit == 0 && ft->lo == 1 && ft->hi == 100 &&
                                                                      !ft->choices.empty() && core::parse_percent(ft->value));
        check("inspector: Opacity typed down to 0", op && op->percent && op->lo == 0);
        std::vector<const core::Item*> one{&items.at(0)};
        const auto sh = core::shared_fields(one);
        check("inspector: percent survives the shared view", field(sh, "fill_tint") && field(sh, "fill_tint")->percent);
        const auto o = core::project(d, "");
        bool style_tint = false;
        for (const auto& it : o.items())
            for (const auto& f : it.fields) style_tint = style_tint || (f.edit == "color_tint" && f.percent);
        check("inspector: a style's tint typed too", style_tint);
    }

    {
        std::cout << "\n-- arrange (s070) --\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto owner = d.as<core::Frame>("frame.1")->owner;
        const auto order = [&] { return d.frames_on(owner); };
        const auto at = [&](const std::string& id) {
            const auto o = order();
            return static_cast<int>(std::find(o.begin(), o.end(), id) - o.begin());
        };
        const auto n = static_cast<int>(order().size());
        check("sample: page 1 has several frames", n >= 3);
        check("to front: one step, on top", D::arrange(j, d, {"frame.1"}, D::Arrange::Front) && j.size() == 1 && at("frame.1") == n - 1);
        check("to front again: nothing moves", !D::arrange(j, d, {"frame.1"}, D::Arrange::Front));
        check("backward: one down", D::arrange(j, d, {"frame.1"}, D::Arrange::Backward) && at("frame.1") == n - 2);
        check("to back: at the bottom", D::arrange(j, d, {"frame.1"}, D::Arrange::Back) && at("frame.1") == 0);
        check("forward: one up", D::arrange(j, d, {"frame.1"}, D::Arrange::Forward) && at("frame.1") == 1);
        // Two picked keep their order among themselves.
        const auto o0 = order();
        const auto a = o0[0], b = o0[1];
        check("two to front: both on top, in order", D::arrange(j, d, {b, a}, D::Arrange::Front) && at(a) == n - 2 && at(b) == n - 1);
        bool z = true;
        const auto o1 = order();
        for (std::size_t k = 0; k < o1.size(); ++k) z = z && d.as<core::Frame>(o1[k])->z == static_cast<double>(k);
        check("z renumbered 0..n-1", z);
        // Stacking drives "text beneath" (s069): the lighthouse sent back stops pushing.
        D::set_wrap_beneath(j, d, true);
        D::arrange(j, d, {"frame.11"}, D::Arrange::Front);
        const bool pushes = !core::wrap_rects(d, "frame.1").empty();
        D::arrange(j, d, {"frame.11"}, D::Arrange::Back);
        check("beneath: in front pushes, at the back doesn't", pushes && core::wrap_rects(d, "frame.1").empty());
        const auto steps = j.size();
        j.undo();
        check("undo: the lighthouse back in front", at("frame.11") == n - 1 && j.size() == steps);
        bool keys = true;
        for (const char* act : {"win.bring-front", "win.bring-forward", "win.send-backward", "win.send-back"}) {
            bool found = false;
            for (const auto& r : core::shortcut_registry()) found = found || (r.action == act && !r.accels.empty());
            keys = keys && found;
        }
        check("arrange: four keys in the registry", keys);
    }

    {
        std::cout << "\n-- wrap options (s069) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto field = [](const std::vector<core::Field>& fs, const std::string& key) -> const core::Field* {
            for (const auto& f : fs) if (f.edit == key) return &f;
            return nullptr;
        };
        // The sample: the lighthouse (frame.11) sits above the opening text (frame.1) and wraps it.
        const auto& pic = *d.as<core::Frame>("frame.11");
        const auto& txt = *d.as<core::Frame>("frame.1");
        check("sample: the picture is above the text", pic.owner == txt.owner && pic.z > txt.z);
        check("wrap: it pushes the text", core::wrap_rects(d, "frame.1").size() == 1);
        // Ignore wrap: the text frame alone, one step; the lines run straight through.
        check("ignore wrap: one step", D::edit_frames(j, d, {"frame.1"}, "Ignore text wrap",
                                                      [](core::Frame& f) { f.ignore_wrap = true; }) && j.size() == 1);
        check("ignore wrap: nothing pushes it", core::wrap_rects(d, "frame.1").empty());
        // Ignoring wrap sets the text exactly as if the picture had no wrap.
        const auto lines_of = [&] {
            for (const auto& fr : L::flow_story(d, "story.1").frames) if (fr.frame == "frame.1") return fr.lines;
            return std::vector<L::Line>{};
        };
        const auto ignored = lines_of();
        D::edit_frames(j, d, {"frame.11"}, "Text wrap", [](core::Frame& f) { f.wrap = core::Wrap::None; });
        const auto none = lines_of();
        j.undo();
        bool same = !ignored.empty() && ignored.size() == none.size();
        for (std::size_t i = 0; same && i < none.size(); ++i)
            same = std::abs(ignored[i].x - none[i].x) < 1e-6 && std::abs(ignored[i].y - none[i].y) < 1e-6 &&
                   ignored[i].start == none[i].start && ignored[i].end == none[i].end;
        check("ignore wrap: the text sets as with no wrap at all", same);
        const auto items = core::frame_items(d, {"frame.1"});
        const auto* iw = field(items.at(0).fields, "ignore_wrap");
        check("inspector: a text frame's Others' wrap", iw && iw->value == "Ignored" && iw->choices.size() == 2);
        j.undo();
        check("ignore wrap: undo -- pushed again", core::wrap_rects(d, "frame.1").size() == 1);
        // Wrap only text beneath: the picture is above -> still pushes; put it below -> it doesn't.
        check("beneath: one step", D::set_wrap_beneath(j, d, true) && d.settings().wrap_beneath && !D::set_wrap_beneath(j, d, true));
        check("beneath: a frame above still pushes", core::wrap_rects(d, "frame.1").size() == 1);
        D::edit_frames(j, d, {"frame.11"}, "Arrange", [&](core::Frame& f) { f.z = txt.z - 1; });
        check("beneath: a frame below doesn't", core::wrap_rects(d, "frame.1").empty());
        D::set_wrap_beneath(j, d, false);
        check("all text: below pushes too", core::wrap_rects(d, "frame.1").size() == 1);
        D::set_wrap_beneath(j, d, true);
        // A master's wrapping item sits beneath every page frame.
        const auto chain = core::master_chain(d, txt.owner);
        check("beneath: the page has a master", !chain.empty());
        if (!chain.empty()) {
            const auto added = D::add_frame(j, d, chain.back(), core::FrameKind::Shape, {txt.x + 20, txt.y + 40, 60, 60});
            const bool made = !added.empty() && D::edit_frames(j, d, {added}, "Text wrap", [](core::Frame& f) {
                f.wrap = core::Wrap::Around;
                f.z = 1e6;   // top of its master -- still under the page
            });
            check("beneath: a master's wrap stays under the page's text", made && core::wrap_rects(d, "frame.1").empty());
            D::set_wrap_beneath(j, d, false);
            check("all text: the master's wrap pushes", !core::wrap_rects(d, "frame.1").empty());
        }
        const auto o = core::project(d, "");
        const auto* dw = field(o.find("doc")->fields, "dwrap");
        check("inspector: Document settings' Text wrap", dw && dw->value == "All text");
        D::set_wrap_beneath(j, d, true);
        D::edit_frames(j, d, {"frame.1"}, "Ignore text wrap", [](core::Frame& f) { f.ignore_wrap = true; });
        std::string err;
        core::Doc e;
        check("json: both kept", core::from_json(e, core::to_json(d), &err) && e.records() == d.records() &&
                                     e.settings().wrap_beneath && e.as<core::Frame>("frame.1")->ignore_wrap, err);
    }

    {
        std::cout << "\n-- up a level (s068) --\n";
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto o = core::project(d, "");
        const auto first = [&](core::Segment s) { const auto in = o.in(s); return in.empty() ? std::string{} : in.front()->id; };
        using S = core::Segment;
        check("up: a page -> Pages & Spreads", core::parent_of(o, {d.pages().front()}) == S::Pages);
        check("up: a master -> Masters", core::parent_of(o, {first(S::Masters)}) == S::Masters);
        check("up: a story -> Stories", core::parent_of(o, {first(S::Stories)}) == S::Stories);
        check("up: a swatch -> Colors", core::parent_of(o, {first(S::Colors)}) == S::Colors);
        check("up: the first known item decides", core::parent_of(o, {"nothing.1", first(S::Masters)}) == S::Masters);
        check("up: nothing known -> no level", !core::parent_of(o, {}) && !core::parent_of(o, {"nothing.1"}));
        bool reg = false;
        for (const auto& r : core::shortcut_registry()) reg = reg || (r.action == "win.go-up" && !r.accels.empty());
        check("up: Alt+Up in the shortcut registry", reg);
        bool back = false, fwd = false;
        for (const auto& r : core::shortcut_registry()) {
            for (const auto& a : r.accels) {
                back = back || (r.action == "win.go-up" && a == "<Alt>Left");
                fwd = fwd || (r.action == "win.go-forward" && a == "<Alt>Right");
            }
        }
        check("back / forward: Alt+Left and Alt+Right in the registry", back && fwd);
    }

    {
        std::cout << "\n-- per-side insets, wrap offsets, typed column widths (s067) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto lines_of = [&](const std::string& story, const std::string& frame) {
            for (const auto& f : L::flow_story(d, story).frames) if (f.frame == frame) return f.lines;
            return std::vector<L::Line>{};
        };
        const auto in0 = d.as<core::Frame>("frame.1")->inset;
        const auto before = lines_of("story.1", "frame.1");
        // One side: the left inset alone.
        check("inset: the left side alone, one step", D::set_side(j, d, {"frame.1"}, D::SideOf::Inset, 3, 30) && j.size() == 1);
        const auto& f1 = *d.as<core::Frame>("frame.1");
        check("inset: only the left changed", f1.inset.left == 30 && f1.inset.top == in0.top && f1.inset.right == in0.right &&
                                                  f1.inset.bottom == in0.bottom);
        const auto left = lines_of("story.1", "frame.1");
        bool inside = !left.empty();
        for (const auto& l : left) inside = inside && l.x >= 30 - 0.01;
        check("inset: every line right of the left inset", inside);
        check("inset: the first line's top unchanged", !left.empty() && !before.empty() && std::abs(left[0].y - before[0].y) < 0.01);
        // The top alone moves the first line down by the difference.
        D::set_side(j, d, {"frame.1"}, D::SideOf::Inset, 0, in0.top + 24);
        const auto top = lines_of("story.1", "frame.1");
        check("inset: the top alone moves the first line down", !top.empty() && std::abs(top[0].y - (before[0].y + 24)) < 0.01);
        check("inset: all four (same all round)", D::set_side(j, d, {"frame.1"}, D::SideOf::Inset, -1, 5) &&
                                                     d.as<core::Frame>("frame.1")->inset == core::Sides{5, 5, 5, 5});
        check("inset: held to 500 pt", D::set_side(j, d, {"frame.1"}, D::SideOf::Inset, 2, 900) &&
                                          d.as<core::Frame>("frame.1")->inset.bottom == 500);
        check("inset: not on a picture", !D::set_side(j, d, {"frame.11"}, D::SideOf::Inset, 0, 12));
        check("inset: a side out of range refused", !D::set_side(j, d, {"frame.1"}, D::SideOf::Inset, 4, 12));
        const auto steps = j.size();
        j.undo();
        j.undo();
        j.undo();
        j.undo();
        check("inset: undo, side by side, back to the start", j.size() == steps && d.as<core::Frame>("frame.1")->inset == in0);
        // Wrap: the right side alone widens the band on the right only.
        const auto w0 = core::wrap_rects(d, "frame.1");
        check("wrap: the right side alone", D::set_side(j, d, {"frame.11"}, D::SideOf::Wrap, 1, 30));
        const auto w1 = core::wrap_rects(d, "frame.1");
        check("wrap: its band reaches further right, the left edge stays",
              w0.size() == 1 && w1.size() == 1 && std::abs(w1[0].x1 - (w0[0].x1 + 21)) < 1e-6 && std::abs(w1[0].x0 - w0[0].x0) < 1e-6);
        // Columns: one typed, the others share the rest in their proportions.
        const auto& src = *d.as<core::Frame>("frame.10");
        const double room = core::column_width(src, 0) + core::column_width(src, 1);
        check("column width: one step", D::set_column_width(j, d, {"frame.10"}, 0, 150));
        const auto& s1 = *d.as<core::Frame>("frame.10");
        check("column width: the first takes 150 pt, the second the rest",
              std::abs(core::column_width(s1, 0) - 150) < 0.02 && std::abs(core::column_width(s1, 0) + core::column_width(s1, 1) - room) < 0.02,
              std::to_string(core::column_width(s1, 0)) + " / " + std::to_string(core::column_width(s1, 1)));
        D::set_column_width(j, d, {"frame.10"}, 1, 1e6);
        check("column width: too wide leaves the other at least 1 pt", core::column_width(*d.as<core::Frame>("frame.10"), 0) >= 1 - 1e-9);
        D::edit_frames(j, d, {"frame.10"}, "Columns", [](core::Frame& f) { f.columns = 3; f.widths = {2, 1, 1}; });
        const auto& s3 = *d.as<core::Frame>("frame.10");
        const double room3 = core::column_width(s3, 0) + core::column_width(s3, 1) + core::column_width(s3, 2);
        D::set_column_width(j, d, {"frame.10"}, 0, room3 / 4);
        const auto& s4 = *d.as<core::Frame>("frame.10");
        check("column width: the others keep their proportions",
              std::abs(core::column_width(s4, 1) - core::column_width(s4, 2)) < 0.02 && std::abs(core::column_width(s4, 0) - room3 / 4) < 0.02);
        check("column width: not on a one-column frame", !D::set_column_width(j, d, {"frame.1"}, 0, 100));
        // The inspector: four typed sides with the link; widths typed, one per column.
        const auto field = [](const std::vector<core::Field>& fs, const std::string& key) -> const core::Field* {
            for (const auto& f : fs) if (f.edit == key) return &f;
            return nullptr;
        };
        D::set_side(j, d, {"frame.1"}, D::SideOf::Inset, 3, 12);
        const auto items = core::frame_items(d, {"frame.1", "frame.10", "frame.11"});
        const auto* ins = field(items.at(0).fields, "inset");
        check("inspector: Inset is four typed sides with a link",
              ins && ins->link && ins->cols.size() == 4 && ins->unit == 1 && ins->value.find('\t') != std::string::npos &&
              ins->value.substr(ins->value.rfind('\t') + 1) == "12");
        const auto* cw = field(items.at(1).fields, "colw");
        check("inspector: Widths typed, one per column, presets beside",
              cw && cw->cols.size() == 3 && cw->unit == 1 && !cw->choices.empty() && cw->choices[0].id == "=");
        const auto* wo = field(items.at(2).fields, "wrap_off");
        check("inspector: Wrap offset is four typed sides", wo && wo->link && wo->cols.size() == 4 && wo->value == "9\t30\t9\t9");
        {   // what the inspector actually shows goes through shared_fields
            std::vector<const core::Item*> one{&items.at(0)};
            const auto sh = core::shared_fields(one);
            check("inspector: the link survives the shared view", field(sh, "inset") && field(sh, "inset")->link);
        }
        std::string err;
        core::Doc e;
        check("json: per-side insets and typed widths kept", core::from_json(e, core::to_json(d), &err) && e.records() == d.records(), err);
    }

    {
        std::cout << "\n-- page numbers --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        load_plain(d);
        const std::string t = std::string("p") + core::kMarkPage + "s" + core::kMarkSection;
        check("markers: found where they are", core::marker_at(t, 1) == 0 && core::marker_at(t, 5) == 1 &&
                                               core::marker_at(t, 0) == -1 && core::marker_at(t, 2) == -1);
        check("markers: a story with one", core::has_markers(*d.as<core::Story>("story.6")) &&
                                           !core::has_markers(*d.as<core::Story>("story.1")));
        // A master's folio, filled in for the page it's drawn on.
        const auto body2 = L::resolve_story(d, "story.6", "page.6");
        const auto front1 = L::resolve_story(d, "story.5", "page.1");
        check("folio: page 2 of the body reads 2", body2.any && body2.text.paras[0].text == "2");
        check("folio: the front matter's first page reads i", front1.text.paras[0].text == "i");
        check("folio: set on the page's own lines", !body2.flow.frames.empty());
        // On the master itself: the sign, the same number of bytes.
        const auto onm = L::resolve_story(d, "story.6", "");
        check("folio: on its master, the marker stays (a sign on screen)", onm.text.paras[0].text == core::kMarkPage);
        // Page frames: each marker takes the page its line is on; next /
        // previous are the thread's neighbours.
        core::doc::edit_text(j, d, "story.1", {1, 0}, {1, 0},
                             std::string("[") + core::kMarkSection + " " + core::kMarkPrev + "/" + core::kMarkNext + "] ");
        const auto r1 = L::resolve_story(d, "story.1", "");
        check("page frame: section name, no previous, the next frame's page",
              r1.text.paras[1].text.rfind("[Body /2] ", 0) == 0, r1.text.paras[1].text.substr(0, 16));
        // A character style keeps covering the same words after the swap.
        core::doc::set_char_style(j, d, "story.1", {1, 18}, {1, 24}, "style.5");   // "harbor" (after the 14-byte prefix + "The ")
        const auto r2 = L::resolve_story(d, "story.1", "");
        const auto& p2 = r2.text.paras[1];
        bool styled = false;
        for (const auto& run : p2.runs) styled = styled || (p2.text.substr(run.start, run.end - run.start) == "harbor");
        check("page frame: character styles follow the words", styled);
        // The thumbnail key follows the folio: move page 2 of the body first.
        L::FlowCache flows;
        const auto k6 = L::page_key(d, flows, "page.6");
        bool two = false;
        for (const auto& l : k6.lines) two = two || (l.size() >= 2 && l.substr(l.size() - 2) == "|2");
        check("key: the folio as drawn", two);
        core::doc::move_pages(j, d, {"page.6"}, "page.4");
        check("key: a page that moves gets a new folio, a new key", !(L::page_key(d, flows, "page.6") == k6));
        // While editing (and in every flow), a marker is three bytes wherever it shows.
        const auto plain = L::flow_story(d, "story.6");
        check("display: the marker's three bytes on one line", !plain.frames.empty() && !plain.frames[0].lines.empty() &&
                                                               plain.frames[0].lines[0].end == 3);
    }

    {
        std::cout << "\n-- preflight --\n";
        namespace L = composix::layout;
        using Lv = L::Issue::Level;
        core::Doc d;
        load_plain(d);
        L::FlowCache flows;
        const auto has = [](const std::vector<L::Issue>& v, const std::string& kind) {
            return std::any_of(v.begin(), v.end(), [&](const L::Issue& i) { return i.kind == kind; });
        };
        auto v = L::preflight_issues(d, flows, {});
        // s089: the sample's contents is out of date on purpose (its Sources line names no heading).
        check("sample: the contents out of date, one warning", L::problem_count(v) == 1 && has(v, "stale_list"));
        {
            core::DocJournal pj{d};
            d.set_door([&pj] { return pj.open(); });
            L::build_list(pj, d, "list.1");
            d.set_door({});
        }
        v = L::preflight_issues(d, flows, {});
        check("clean sample: no problems, the spot plate noted", L::problem_count(v) == 0 && has(v, "spot"));
        {   // overset, at its last frame; a low-res picture; links as told
            core::Doc::Loading load(d);
            auto f1 = *d.as<core::Frame>("frame.1");
            f1.h = 40;
            f1.next.clear();
            d.put("frame.1", f1);
            auto a = *d.as<core::Asset>("asset.4");
            a.px_w = 200; a.px_h = 150;
            d.put("asset.4", a);
            auto f = *d.as<core::Frame>("frame.6");
            f.opacity = 0.5;
            d.put("frame.6", f);
        }
        L::PreflightOpts o;
        o.link = [](const core::Asset& a) { return a.name == "harbor.psd" ? core::LinkStatus::Missing : core::LinkStatus::OK; };
        v = L::preflight_issues(d, flows, o);
        const auto over = std::find_if(v.begin(), v.end(), [](const L::Issue& i) { return i.kind == "overset"; });
        check("overset: an error at its frame and page", over != v.end() && over->level == Lv::Error &&
                                                         over->frame == "frame.1" && over->page == "page.5");
        check("missing picture: an error, named", has(v, "missing"));
        check("low resolution: a warning", has(v, "lowres"));
        check("no standard: opacity is fine", !has(v, "transparency"));
        check("errors first", v.front().level == Lv::Error && v.back().level == Lv::Info);
        o.standard = 1;
        v = L::preflight_issues(d, flows, o);
        check("X-1a, no profile: the profile, RGB, transparency", has(v, "profile") && has(v, "rgb") && has(v, "transparency"));
        o.profile = true;
        v = L::preflight_issues(d, flows, o);
        check("X-1a with a profile: RGB converts (no error), transparency still", !has(v, "rgb") && has(v, "transparency"));
        L::PdfReport rep;
        L::preflight(d, {}, 16, &rep);
        check("export report: the same checks", std::any_of(rep.warnings.begin(), rep.warnings.end(), [](const std::string& w) {
                  return w.rfind("Overset text: Chapters", 0) == 0; }));
    }

    {
        std::cout << "\n-- master overrides --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        check("sides: the right folio on right pages only", !core::master_item_on(d, "frame.13", "page.6") &&
                                                            core::master_item_on(d, "frame.13", "page.7") &&
                                                            core::master_item_on(d, "frame.14", "page.6") &&
                                                            !core::master_item_on(d, "frame.14", "page.7"));
        check("override: the chapter opener's own folio hides the master's", !core::master_item_on(d, "frame.13", "page.5"));
        L::FlowCache flows;
        const auto k5 = L::page_key(d, flows, "page.5");
        const auto has_rec = [](const L::PageKey& k, const std::string& id) {
            return std::any_of(k.recs.begin(), k.recs.end(), [&](const auto& r) { return r.first == id; });
        };
        check("drawn: the page's copy, not the master's", has_rec(k5, "frame.15") && !has_rec(k5, "frame.13") && !has_rec(k5, "frame.14"));
        const auto steps = j.size();
        const auto made = core::doc::override_master_item(j, d, "page.7", "frame.13");
        const auto* mf = d.as<core::Frame>(made);
        check("override: a page copy, one step", mf && mf->owner == "page.7" && mf->overrides == "frame.13" && j.size() == steps + 1);
        check("override: its own story, the same words", mf && mf->story != "story.6" &&
                                                         d.as<core::Story>(mf->story)->paras[0].text == core::kMarkPage);
        check("override: the master's hidden there", !core::master_item_on(d, "frame.13", "page.7"));
        check("override: not twice, not where it doesn't show", core::doc::override_master_item(j, d, "page.7", "frame.13").empty() &&
                                                                core::doc::override_master_item(j, d, "page.6", "frame.13").empty());
        const std::string story = mf ? mf->story : "";
        check("remove override: back to the master's, its story gone", core::doc::remove_overrides(j, d, {made}) &&
                                                                       !d.as<core::Frame>(made) && !d.as<core::Story>(story) &&
                                                                       core::master_item_on(d, "frame.13", "page.7"));
        j.undo();
        check("remove override: undo, the copy back", d.as<core::Frame>(made) && !core::master_item_on(d, "frame.13", "page.7"));
        // Ctrl+Shift+click finds the master's item under the pointer on a page.
        const auto pl = core::layout_pages(d, {"page.9"});
        const auto* r = d.as<core::Frame>("frame.13");
        core::Doc::Id on;
        check("hit: a master item on a page", core::hit_master_item(d, pl, pl[0].r.x + r->x + 5, pl[0].r.y + r->y + 5, 1, &on) == "frame.13" &&
                                              on == "page.9");
        check("hit: not on the wrong side", core::hit_master_item(d, core::layout_pages(d, {"page.8"}),
                                                                  pl[0].r.x + r->x + 5, pl[0].r.y + r->y + 5, 1, &on).empty());
    }

    {
        std::cout << "\n-- indents + tabs --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::load_sample(d);
        // Turn the wrapped picture off: plain lines to measure.
        {
            core::Doc::Loading load(d);
            auto f = *d.as<core::Frame>("frame.11");
            f.wrap = core::Wrap::None;
            d.put("frame.11", f);
        }
        const auto fl = L::flow_story(d, "story.1");
        const auto cx = [&](int p, int b) { return L::caret_at(d, "story.1", fl, {p, b}).x; };
        check("first indent: none after the heading", std::abs(cx(1, 0)) < 0.5);
        check("first indent: 18 pt after a body paragraph", std::abs(cx(2, 0) - 18) < 0.5);
        check("first indent: only the first line", [&] {
            for (const auto& ft : fl.frames)
                for (const auto& l : ft.lines)
                    if (l.para == 2 && l.start > 0) return std::abs(L::caret_at(d, "story.1", fl, {2, l.start}).x) < 0.5;
            return false;
        }());
        {   // s045: the sample's Sources are a numbered list now (their first line in line
            // with the rest, the number hung); without it, the negative first indent hangs.
            auto s4 = *d.as<core::Style>("style.4");
            s4.list = {};
            d.put("style.4", s4);
        }
        const auto src = L::flow_story(d, "story.4");
        bool hang = false;
        for (const auto& ft : src.frames)
            for (const auto& l : ft.lines)
                if (l.para == 0 && l.start > 0) hang = std::abs(L::caret_at(d, "story.4", src, {0, l.start}).x - l.x + l.x - 12) < 0.5 ||
                                                       std::abs(L::caret_at(d, "story.4", src, {0, l.start}).x - 12) < 0.5;
        check("hanging indent: the first line out, the rest in", std::abs(L::caret_at(d, "story.4", src, {0, 0}).x) < 0.5 && hang);
        // Contents: a right tab at the text's edge, with dots.
        const auto con = L::flow_story(d, "story.9");
        const double tw = core::text_width(d);
        const auto one = L::caret_at(d, "story.9", con, {1, 12});   // after "Chapter One\t"
        check("tab: right-aligned at the text's edge", one.ok && one.x > tw - 12 && one.x < tw);
        int leaders = 0;
        L::visit_lines(d, "story.9", con, "frame.16", [&](const L::LineVisit& v) { leaders += v.from < 0; });
        check("tab: a dot leader on each entry", leaders == 3);
        check("tabs: in words", core::tabs_text(d.as<core::Style>("style.8")->tabs) == "Right 4.5 in, dots" &&
                                core::tabs_text({}) == "None");
        check("tabs: presets measure the text width", core::tab_preset(d, "right.").front().pos == tw &&
                                                      core::tab_preset(d, "every").size() == 8);
        const auto o = core::project(d, "");
        bool fi = false, tb = false;
        for (const auto& f : o.find("style.1")->fields) {
            fi = fi || (f.edit == "findent" && f.value == "18 pt");
            tb = tb || (f.edit == "indent_after" && f.value == "Not after a heading");
        }
        check("inspector: First indent and when", fi && tb);
    }

    {
        std::cout << "\n-- spreads --\n";
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto sp = core::spreads(d);
        check("spreads: page i alone, then left + right pairs", sp.front() == std::vector<std::string>{"page.1"} &&
                                                                 sp[1] == (std::vector<std::string>{"page.2", "page.3"}));
        check("spreads: every page once", [&] {
            std::size_t n = 0;
            for (const auto& s : sp) n += s.size();
            return n == d.pages().size();
        }());
        check("spread_of: either page gives the pair", core::spread_of(d, "page.7") == (std::vector<std::string>{"page.6", "page.7"}) &&
                                                       core::spread_of(d, "page.6") == core::spread_of(d, "page.7"));
        check("spread_of: page 1 faces page iv (sections don't break spreads)",
              core::spread_of(d, "page.5") == (std::vector<std::string>{"page.4", "page.5"}));
        {   // single pages: each its own
            core::Doc e;
            core::load_sample(e);
            core::Doc::Loading load(e);
            auto st = e.settings();
            st.facing = false;
            e.put("doc", st);
            for (const auto& mid : e.ids_of<core::Master>()) {   // s147: facing is each master's
                auto m = *e.as<core::Master>(mid);
                m.facing = false;
                e.put(mid, m);
            }
            check("spreads: not facing, one page each", core::spreads(e).size() == e.pages().size());
        }
        // A frame dragged across the spine belongs to the other page.
        const auto pl = core::layout_pages(d, {"page.6", "page.7"});
        const auto box = core::doc::add_frame(j, d, "page.6", core::FrameKind::Shape, {300, 100, 100, 50});
        core::doc::frames_begin(j, d, {box}, "Move");
        core::doc::frames_set(j, d, box, {420, 100, 100, 50});   // centre at 470: page 7 (from 432)
        const int moved = core::doc::frames_rehome(j, d, pl, {box});
        core::doc::frames_end(j);
        const auto* f = d.as<core::Frame>(box);
        check("across the spine: the right page's now, same place", moved == 1 && f->owner == "page.7" &&
                                                                     std::abs(f->x - (420 - 432)) < 1e-9 && f->y == 100);
        j.undo();
        check("across the spine: one undo puts it back", d.as<core::Frame>(box)->owner == "page.6" &&
                                                        d.as<core::Frame>(box)->x == 300);
    }

    {
        std::cout << "\n-- drop caps --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto fl = L::flow_story(d, "story.1");
        const L::Line* cap = nullptr;
        double cap_x1 = 0;
        for (const auto& l : fl.frames.front().lines)
            if (l.para == 1 && l.cap > 0) { cap = &l; cap_x1 = l.x + l.w; }
        check("drop cap: the first letter, three lines tall", cap && cap->start == 0 && cap->end == 1 &&
                                                              std::abs(cap->h - 3 * 14) < 1e-9 && cap->cap > 30);
        bool beside = false;
        for (const auto& l : fl.frames.front().lines)
            if (cap && l.para == 1 && l.cap <= 0 && l.y < cap->y + cap->h) beside = beside || l.x > cap_x1;
        check("drop cap: the lines beside it start past it", beside);
        const auto c0 = L::caret_at(d, "story.1", fl, {1, 0});
        const auto c1 = L::caret_at(d, "story.1", fl, {1, 1});
        check("drop cap: the caret before it is as tall as it, after it beside", c0.ok && std::abs(c0.h - 42) < 1e-9 &&
                                                                                 c1.ok && c1.x > cap_x1);
        double next_y = 0;
        for (const auto& l : fl.frames.front().lines) if (l.para == 2) { next_y = l.y; break; }
        check("drop cap: a short paragraph still clears it", cap && next_y >= cap->y + cap->h - 0.01);
        check("indent: the paragraph after the drop cap's is indented (not a heading)",
              std::abs(L::caret_at(d, "story.1", fl, {2, 0}).x - 18) < 0.5);
        core::doc::edit_styles(j, d, {"style.10"}, "Drop letters", [](core::Style& s) { s.drop_chars = 2; });
        const auto f2 = L::flow_story(d, "story.1");
        bool two = false;
        for (const auto& l : f2.frames.front().lines) two = two || (l.para == 1 && l.cap > 0 && l.end == 2);
        check("drop cap: two letters", two);
        core::doc::edit_styles(j, d, {"style.10"}, "Drop cap", [](core::Style& s) { s.drop_lines = 0; });
        bool none = true;
        const auto f0 = L::flow_story(d, "story.1");   // (kept: .front() of a temporary dangles)
        for (const auto& l : f0.frames.front().lines) none = none && l.cap <= 0;
        check("drop cap: none", none);
        const auto o = core::project(d, "");
        bool field = false;
        for (const auto& f : o.find("style.10")->fields) field = field || (f.edit == "drop_lines" && f.value == "None");
        check("inspector: Drop cap", field);
    }

    {
        std::cout << "\n-- local formatting --\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        load_plain(d);
        const core::TextPos a{1, 4}, b{1, 10};   // "harbor"
        const auto story = [&] { return *d.as<core::Story>("story.1"); };
        check("bold: not yet", !core::all_bold(d, story(), a, b));
        check("bold: one step", D::set_local(j, d, "story.1", a, b, "Bold", [](core::Local& l) { l.bold = 1; }) &&
                                core::all_bold(d, story(), a, b) && !core::all_bold(d, story(), {1, 0}, b));
        const auto s1 = story();
        const auto& runs = s1.paras[1].runs;
        check("bold: a run with local formatting, no style", runs.size() == 1 && runs[0].start == 4 && runs[0].end == 10 &&
                                                              runs[0].style.empty() && runs[0].local.bold == 1);
        const auto at = D::edit_text(j, d, "story.1", {1, 7}, {1, 7}, "x");
        check("typing inside: it's bold too", at == core::TextPos{1, 8} && story().paras[1].runs[0].end == 11);
        D::set_char_style(j, d, "story.1", {1, 4}, {1, 11}, "style.5");
        check("a character style on top keeps it", story().paras[1].runs[0].style == "style.5" &&
                                                    story().paras[1].runs[0].local.bold == 1);
        D::set_local(j, d, "story.1", {1, 4}, {1, 11}, "Colour", [](core::Local& l) { l.color = "swatch.3"; l.size = 14; });
        std::string err;
        core::Doc e;
        check("json: round-trips", core::from_json(e, core::to_json(d), &err) && e.records() == d.records(), err);
        const auto f = core::text_fields(d, "story.1", {1, 5}, {1, 5});
        bool sz = false, co = false, bt = false;
        for (const auto& x : f) {
            sz = sz || (x.edit == "lsize" && x.value == "14 pt");
            co = co || (x.edit == "lcolor" && x.value == "Guild Rust");
            bt = bt || (x.edit == "local" && x.buttons);
        }
        check("inspector: Size, Colour, Bold / Italic / Clear", sz && co && bt);
        D::set_local(j, d, "story.1", {1, 4}, {1, 11}, "Clear overrides", [](core::Local& l) { l = {}; });
        check("clear: back to the style alone", story().paras[1].runs.size() == 1 && story().paras[1].runs[0].local.empty());
        check("italic: the style's own counts", core::all_italic(d, story(), {1, 4}, {1, 11}));   // Emphasis is italic
        namespace L = composix::layout;
        // Drawn: a bold run sets wider than the same words plain.
        core::Doc w;
        load_plain(w);
        core::DocJournal wj{w};
        w.set_door([&wj] { return wj.open(); });
        const auto x0 = L::caret_at(w, "story.1", L::flow_story(w, "story.1"), {1, 10}).x;
        D::set_local(wj, w, "story.1", {1, 4}, {1, 10}, "Bold", [](core::Local& l) { l.bold = 1; });
        const auto x1 = L::caret_at(w, "story.1", L::flow_story(w, "story.1"), {1, 10}).x;
        check("drawn: bold words set wider", x1 > x0 + 0.5);
    }

    {
        std::cout << "\n-- export settings --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        check("defaults: print, bleed, no profile", d.settings().exp.print && d.settings().exp.bleed && d.settings().exp.profile.empty());
        core::ExportPrefs e{true, true, true, "GRACoL2013_CRPC6.icc", true, 1};
        check("kept: one step", core::doc::set_export(j, d, e) && j.size() == 1 && d.settings().exp == e);
        check("kept: the same again, no step", !core::doc::set_export(j, d, e) && j.size() == 1);
        std::string err;
        core::Doc f;
        check("json: with the document", core::from_json(f, core::to_json(d), &err) && f.settings().exp == e, err);
        L::FlowCache flows;
        L::PreflightOpts o;
        o.missing_profile = "Some Press Profile";
        const auto v = L::preflight_issues(d, flows, o);
        check("preflight: a profile not on this computer is named", std::any_of(v.begin(), v.end(), [](const L::Issue& i) {
                  return i.kind == "profile" && i.text.find("Some Press Profile") != std::string::npos; }));
        j.undo();
        check("undo: back to the defaults", d.settings().exp == core::ExportPrefs{});
    }

    {
        std::cout << "\n-- picture placement --\n";
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto* f = d.as<core::Frame>("frame.4");   // harbor.psd, 3600 x 2700, filling 324 x 108
        const auto* a = d.as<core::Asset>("asset.2");
        const auto fill = core::picture_rect({0, 0, f->w, f->h}, *f, a->px_w, a->px_h);
        check("fill: as before", std::abs(fill.w - 324) < 1e-6);
        check("scale: fill is a share of its own size", std::abs(core::picture_scale(*f, *a) - 324 / (3600 * 72.0 / 300) * 100) < 0.01);
        check("set scale: one step, by hand now", core::doc::set_picture_scale(j, d, {"frame.4"}, 50) && j.size() == 1 &&
                                                  d.as<core::Frame>("frame.4")->fit == core::ImageFit::Custom);
        f = d.as<core::Frame>("frame.4");
        const auto half = core::picture_rect({0, 0, f->w, f->h}, *f, a->px_w, a->px_h);
        check("set scale: 50% of 12 x 9 in is 6 x 4.5 in, about the old centre",
              std::abs(half.w - 432) < 1e-6 && std::abs(half.h - 324) < 1e-6 &&
                  std::abs(half.x + half.w / 2 - (fill.x + fill.w / 2)) < 1e-6);
        check("effective ppi: 600 at 50%", std::abs(core::effective_ppi(*f, *a) - 600) < 0.5);
        // A move inside the frame (as Alt+drag does), within a gesture: one step.
        core::doc::frames_begin(j, d, {"frame.4"}, "Move picture");
        core::doc::edit_frames(j, d, {"frame.4"}, "Move picture", [](core::Frame& g) { g.pic_x -= 30; g.pic_y += 12; });
        core::doc::frames_end(j);
        f = d.as<core::Frame>("frame.4");
        const auto moved = core::picture_rect({0, 0, f->w, f->h}, *f, a->px_w, a->px_h);
        check("moved: by the drag, the scale kept", std::abs(moved.x - (half.x - 30)) < 1e-6 && std::abs(moved.w - half.w) < 1e-6 &&
                                                    j.size() == 2);
        check("screen units: the same place at a zoom", [&] {
            const auto z = core::picture_rect({10, 20, f->w * 2, f->h * 2}, *f, a->px_w, a->px_h, 2);
            return std::abs(z.x - (10 + moved.x * 2)) < 1e-6 && std::abs(z.w - moved.w * 2) < 1e-6;
        }());
        std::string err;
        core::Doc e;
        check("json: kept", core::from_json(e, core::to_json(d), &err) && e.records() == d.records(), err);
        check("fill again: by hand gone", core::doc::set_fit(j, d, {"frame.4"}, core::ImageFit::Fill) &&
                                          std::abs(core::picture_rect({0, 0, f->w, f->h}, *d.as<core::Frame>("frame.4"), a->px_w, a->px_h).w - 324) < 1e-6);
    }

    {
        std::cout << "\n-- baseline grid --\n";
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto pages = d.pages();
        int with = 0;
        for (const auto& pid : pages) with += core::baseline_grid(d, pid).step == 14;
        check("sample: the body master has a 14 pt grid from its top margin",
              with > 0 && core::baseline_grid(d, "master.2").step == 14 &&
                  std::abs(core::baseline_grid(d, "master.2").start - d.as<core::Master>("master.2")->margins.top) < 1e-9);
        // Every line's top, story.1, in order.
        const auto tops = [&] {
            std::vector<double> out;
            for (const auto& ft : composix::layout::flow_story(d, "story.1").frames) for (const auto& l : ft.lines) out.push_back(l.y);
            return out;
        };
        const auto before = tops();
        // The baselines of the Body lines on gridded pages: off the grid by how much (worst).
        const auto worst = [&](int* counted) {
            double w = 0;
            *counted = 0;
            const auto* st = d.as<core::Story>("story.1");
            const auto flow = composix::layout::flow_story(d, "story.1");
            for (const auto& ft : flow.frames) {
                const auto* fr = d.as<core::Frame>(ft.frame);
                const auto g = core::baseline_grid(d, fr->owner);
                if (g.step <= 0) continue;
                composix::layout::visit_lines(d, "story.1", flow, ft.frame, [&](const composix::layout::LineVisit& v) {
                    if (v.from < 0) return;
                    const auto& style = st->paras[static_cast<std::size_t>(v.para)].style;
                    if (style != "style.1" && style != "style.10") return;
                    const double r = std::fmod(fr->y + v.baseline - g.start, g.step);
                    w = std::max(w, std::min(std::abs(r), std::abs(g.step - std::abs(r))));
                    ++*counted;
                });
            }
            return w;
        };
        int n0 = 0;
        const double off0 = worst(&n0);
        check("grid off: body baselines are where they were (off the grid)", n0 > 0 && off0 > 0.5, std::to_string(off0));
        check("align to grid: one step", core::doc::edit_styles(j, d, {"style.1", "style.10"}, "Align to grid",
                                                                [](core::Style& st) { st.align_grid = true; }) && j.size() == 1);
        int n1 = 0;
        const double off1 = worst(&n1);
        check("aligned: every body baseline on the grid (drop cap's too)", n1 > 0 && off1 < 0.01, std::to_string(off1) + " over " + std::to_string(n1));
        const auto after = tops();
        bool down = after.size() >= 1;
        // Lines only ever move down (never up past where they would be).
        for (std::size_t i = 0; i < std::min(before.size(), after.size()) && i < 3; ++i) down = down && after[i] >= before[i] - 1e-6;
        check("aligned: lines moved down, not up", down);
        check("master without a grid: nothing to align to", [&] {
            core::doc::edit_masters(j, d, {"master.2", "master.3"}, "Baseline grid", [](core::Master& m) { m.grid = 0; });
            const bool same = tops() == before;
            j.undo();
            return same;
        }());
        std::string err;
        core::Doc e;
        check("json: grid and align kept", core::from_json(e, core::to_json(d), &err) && e.records() == d.records() &&
                                               e.as<core::Style>("style.1")->align_grid && e.as<core::Master>("master.2")->grid == 14, err);
        const auto o = core::project(d, "");
        bool mf = false, sf = false;
        for (const auto& f : o.find("master.2")->fields) mf = mf || (f.edit == "mgrid" && f.value == "14 pt");
        for (const auto& f : o.find("style.1")->fields) sf = sf || (f.edit == "align_grid" && f.value == "Align to grid");
        check("inspector: the master's grid and the style's alignment", mf && sf);
        j.undo();
        check("undo: the flow as it was", tops() == before);
    }

    {
        std::cout << "\n-- styles on styles (s037) --\n";
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto S = [&](const char* id) { return *d.as<core::Style>(id); };
        check("sample: Body First is based on Body, owning its indent and drop cap",
              S("style.10").based_on == "style.1" &&
                  S("style.10").own == std::vector<std::string>{"drop_lines", "first_indent"});
        // A change to Body ripples into Body First (what it doesn't set) -- one step.
        check("edit base: one step", core::doc::edit_styles(j, d, {"style.1"}, "Size",
                                                            [](core::Style& s) { s.size = 12; s.leading = 15; }) && j.size() == 1);
        check("edit base: the based-on style follows", S("style.10").size == 12 && S("style.10").leading == 15);
        check("edit base: its own settings stay", S("style.10").first_indent == 0 && S("style.10").drop_lines == 3);
        core::doc::edit_styles(j, d, {"style.1"}, "First indent", [](core::Style& s) { s.first_indent = 24; });
        check("edit base: an owned setting doesn't follow", S("style.10").first_indent == 0);
        j.undo();
        j.undo();
        check("undo: base and follower as they were", S("style.1").size == 11 && S("style.10").size == 11);
        // A change on the based-on style becomes its own.
        core::doc::edit_styles(j, d, {"style.10"}, "Leading", [](core::Style& s) { s.leading = 16; });
        check("edit follower: the setting is its own now",
              [&] { const auto own = S("style.10").own; return std::count(own.begin(), own.end(), "leading") == 1; }());
        core::doc::edit_styles(j, d, {"style.1"}, "Leading", [](core::Style& s) { s.leading = 13; });
        check("edit follower: then the base's change doesn't override it", S("style.10").leading == 16);
        check("reset to base: back to Body's", core::doc::reset_to_base(j, d, {"style.10"}) &&
                                                  S("style.10").leading == 13 && S("style.10").first_indent == 18 &&
                                                  S("style.10").drop_lines == 0 && S("style.10").own.empty());
        j.undo(); j.undo(); j.undo();
        // Chains: Pull Quote on Body First on Body -- Body's change goes two deep.
        check("base on: the look doesn't change", [&] {
            const auto before = S("style.6");
            core::doc::set_style_base(j, d, "style.6", "style.10");
            auto after = S("style.6");
            after.based_on.clear();
            after.own.clear();
            return after == before && S("style.6").based_on == "style.10" && !S("style.6").own.empty();
        }());
        core::doc::edit_styles(j, d, {"style.1"}, "Font", [](core::Style& s) { s.family = "Sans"; });
        check("chain: a change two levels up reaches the end", S("style.10").family == "Sans" && S("style.6").family == "Sans");
        check("chain: descendants in order",
              core::doc::style_descendants(d, "style.1") == std::vector<core::Doc::Id>{"style.12", "style.10", "style.6"});   // s114: Footnote on Body (by position)
        j.undo();
        check("no loops: Body can't be based on what's based on it",
              !core::doc::can_base_on(d, "style.1", "style.6") && !core::doc::can_base_on(d, "style.1", "style.1") &&
                  !core::doc::set_style_base(j, d, "style.1", "style.10"));
        check("no character bases", !core::doc::can_base_on(d, "style.1", "style.5"));
        // Deleting a base: those on it take its base and look the same.
        {
            const auto pq = S("style.6");
            core::doc::set_style_base(j, d, "style.6", "style.3");   // Caption is used; base on Pull Quote instead
            j.undo();
            (void)pq;
            core::doc::set_style_base(j, d, "style.3", "style.6");   // Caption on Pull Quote (unused)
            const auto cap = S("style.3");
            check("delete a base: one step", core::doc::delete_styles(j, d, {"style.6"}));
            auto now = S("style.3");
            check("delete a base: the style on it moves to its base, unchanged",
                  now.based_on == "style.10" && now.size == cap.size && now.italic == cap.italic && now.align == cap.align);
            j.undo();
            j.undo();
        }
        // Next style: Enter at a heading's end starts Body First; Enter mid-paragraph keeps the style.
        {
            const auto caret = core::doc::edit_text(j, d, "story.1", {0, 11}, {0, 11}, "\n");
            const auto& paras = d.as<core::Story>("story.1")->paras;
            check("next style: Enter after Chapter One starts Body First",
                  caret.para == 1 && paras[1].style == "style.10" && paras[0].style == "style.2");
            j.undo();
            core::doc::edit_text(j, d, "story.1", {0, 4}, {0, 4}, "\n");
            check("next style: Enter inside a heading splits it as Heading",
                  d.as<core::Story>("story.1")->paras[1].style == "style.2");
            j.undo();
            core::doc::edit_text(j, d, "story.1", {0, 11}, {0, 11}, "a\nb");
            // s081 (was "a paste keeps the style"): a paste at a heading's end
            // runs through Next style too -- the backlog's "Next style for a paste".
            check("next style: a paste at the heading's end takes its Next", d.as<core::Story>("story.1")->paras[1].style == "style.10");
            j.undo();
            const auto& b1 = d.as<core::Story>("story.1")->paras[1];
            core::doc::edit_text(j, d, "story.1", {1, static_cast<int>(b1.text.size())}, {1, static_cast<int>(b1.text.size())}, "\n");
            check("next style: Enter after Body First goes on in Body", d.as<core::Story>("story.1")->paras[2].style == "style.1");
            j.undo();
        }
        // Kept in the file; shown in the inspector.
        {
            core::doc::set_style_base(j, d, "style.6", "style.10");
            std::string err;
            core::Doc e;
            check("json: based on, own and next kept", core::from_json(e, core::to_json(d), &err) && e.records() == d.records() &&
                                                         e.as<core::Style>("style.6")->based_on == "style.10" &&
                                                         e.as<core::Style>("style.2")->next == "style.10", err);
            const auto o = core::project(d, "");
            bool base = false, own = false, nx = false, kids = false, reset = false;
            for (const auto& f : o.find("style.10")->fields) {
                base = base || (f.edit == "based_on" && f.value == "Body");
                own = own || (f.label == "Own settings" && f.value == "Drop cap, First indent");
                reset = reset || (f.edit == "style_base" && f.buttons);
                nx = nx || (f.edit == "next_style" && f.value == "Body");
                kids = kids || (f.label == "Styles on it" && f.value == "Pull Quote");
            }
            check("inspector: Based on, Own settings, Reset, Next style, Styles on it", base && own && reset && nx && kids);
            bool loopfree = true;
            for (const auto& f : o.find("style.1")->fields)
                if (f.edit == "based_on")
                    for (const auto& c : f.choices) loopfree = loopfree && c.id != "style.10" && c.id != "style.6" && c.id != "style.1";
            check("inspector: Body's Based on offers no style built on it", loopfree);
            j.undo();
        }
    }

    {
        std::cout << "\n-- hyphenation (s038) --\n";
        check("patterns: hy-phen-ation", core::hyphenated("hyphenation") == "hy-phen-ation", core::hyphenated("hyphenation"));
        check("patterns: an exception (ta-ble)", core::hyphenated("table") == "ta-ble", core::hyphenated("table"));
        check("patterns: equinox, cargo", core::hyphenated("equinox") == "equinox" || core::hyphenated("equinox") == "equi-nox",
              core::hyphenated("equinox"));
        check("patterns: capitals fold", core::hyphenated("Hyphenation") == "Hy-phen-ation");
        check("patterns: 2 before, 3 after", core::hyphen_points("oranges").empty() || core::hyphen_points("oranges").front() >= 2,
              core::hyphenated("oranges"));
        check("patterns: short words and non-letters don't break",
              core::hyphen_points("ship").empty() && core::hyphen_points("caf\xC3\xA9s").empty() && core::hyphen_points("x86-64").empty());
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        // A narrow justified frame of long words: hyphens are certain.
        std::string words;
        for (int k = 0; k < 12; ++k) words += "Uncharacteristically representational communication institutionalization ";
        words.pop_back();
        core::doc::edit_text(j, d, "story.1", {2, 0}, {2, static_cast<int>(d.as<core::Story>("story.1")->paras[2].text.size())}, words);
        const auto count = [&](int* lines = nullptr) {
            int h = 0, n = 0;
            for (const auto& f : L::flow_story(d, "story.1").frames)
                for (const auto& l : f.lines)
                    if (l.para == 2) { h += l.hyphen; ++n; }
            if (lines) *lines = n;
            return h;
        };
        int n_on = 0, n_off = 0;
        const int h = count(&n_on);
        check("flow: Body hyphenates the long words", h > 0, std::to_string(h) + " of " + std::to_string(n_on));
        bool mid = false, between = true;
        const auto& txt = d.as<core::Story>("story.1")->paras[2].text;
        for (const auto& f : L::flow_story(d, "story.1").frames)
            for (const auto& l : f.lines)
                if (l.para == 2 && l.hyphen) {
                    mid = mid || std::isalpha(static_cast<unsigned char>(txt[static_cast<std::size_t>(l.end)]));
                    between = between && std::isalpha(static_cast<unsigned char>(txt[static_cast<std::size_t>(l.end - 1)]));
                }
        check("flow: a hyphenated line ends inside a word", mid && between);
        // Drawn: the hyphen is on the page (the line's glyphs end in '-'), the text unchanged.
        {
            const auto flow = L::flow_story(d, "story.1");
            int dashed = 0;
            for (const auto& f : flow.frames)
                L::visit_lines(d, "story.1", flow, f.frame, [&](const L::LineVisit& v) {
                    if (v.from < 0 || !v.line) return;
                    const char* t = pango_layout_get_text(v.line->layout);
                    const int e = v.line->start_index + v.line->length;
                    if (e > 0 && t[e - 1] == '-') ++dashed;
                });
            check("draw: the hyphenated lines end in a hyphen", dashed >= h, std::to_string(dashed) + " / " + std::to_string(h));
            check("text: no hyphen in the words", d.as<core::Story>("story.1")->paras[2].text == words);
        }
        // Caret at a hyphenated line's end belongs to the next line, as at any soft wrap.
        {
            const auto flow = L::flow_story(d, "story.1");
            const L::Line* hl = nullptr;
            for (const auto& f : flow.frames) for (const auto& l : f.lines) if (!hl && l.para == 2 && l.hyphen) hl = &l;
            const auto c1 = L::caret_at(d, "story.1", flow, {2, hl->end - 1});
            const auto c2 = L::caret_at(d, "story.1", flow, {2, hl->end});
            check("caret: before the break on the line, at it on the next", c1.ok && c2.ok && std::abs(c1.y - hl->y) < 0.5 && c2.y > c1.y + 1,
                  std::to_string(c1.y) + " " + std::to_string(c2.y));
        }
        check("style off: one step", core::doc::edit_styles(j, d, {"style.1"}, "Hyphenate",
                                                            [](core::Style& s) { s.hyphenate = false; }));
        check("style off: no hyphens (and Body First follows Body)", count(&n_off) == 0 && !d.as<core::Style>("style.10")->hyphenate);
        check("style off: hyphens save lines", n_off >= n_on, std::to_string(n_on) + " vs " + std::to_string(n_off));
        j.undo();
        check("undo: hyphens back", count() == h);
        const auto o = core::project(d, "");
        bool row = false;
        for (const auto& f : o.find("style.1")->fields) row = row || (f.edit == "hyphenate" && f.value == "On");
        check("inspector: Hyphenate On", row);
        std::string err;
        core::Doc e;
        check("json: hyphenate kept", core::from_json(e, core::to_json(d), &err) && e.as<core::Style>("style.1")->hyphenate, err);
    }

    {
        std::cout << "\n-- keeps (s039) --\n";
        namespace L = composix::layout;
        // Breaches of the keeps at every column break of story.1: a heading
        // left at a column's foot, a paragraph's first line alone at a foot
        // (orphan), its last alone at a head (widow). `checked`: column breaks seen.
        const auto breaches = [](const core::Doc& d, int* checked, int* splits) {
            const auto& st = *d.as<core::Story>("story.1");
            std::vector<std::vector<L::Line>> cols;
            for (const auto& f : L::flow_story(d, "story.1").frames) {
                int last = -1;
                for (const auto& l : f.lines) {
                    if (l.col != last) { cols.emplace_back(); last = l.col; }
                    cols.back().push_back(l);
                }
            }
            int bad = 0;
            for (std::size_t k = 0; k + 1 < cols.size(); ++k) {
                const auto& A = cols[k];
                const auto& B = cols[k + 1];
                if (A.empty() || B.empty()) continue;
                ++*checked;
                const auto& last = A.back();
                const auto& first = B.front();
                const auto sty = [&](int p) { return d.as<core::Style>(st.paras[static_cast<std::size_t>(p)].style); };
                const int plen = static_cast<int>(st.paras[static_cast<std::size_t>(last.para)].text.size());
                if (last.para == first.para) {   // split
                    ++*splits;
                    int na = 0, nb = 0;
                    bool starts = false, others = false;
                    for (const auto& l : A) if (l.para == last.para) { na += l.cap <= 0; starts = starts || l.start == 0; } else others = true;
                    bool ends = false;
                    for (const auto& l : B) if (l.para == last.para) { nb += l.cap <= 0; ends = ends || l.end == plen; }
                    if (starts && others && na < 2) ++bad;
                    if (ends && nb < 2 && na - (2 - nb) >= 2) ++bad;
                } else if (sty(last.para)->keep_next && first.start == 0) {
                    bool others = false;
                    for (const auto& l : A) others = others || l.para != last.para;
                    if (others) ++bad;
                }
            }
            return bad;
        };
        // Many layouts: n Body paragraphs of different lengths before the
        // second chapter's heading.
        const auto with_filler = [](core::Doc& d, int n, int words) {
            auto s = *d.as<core::Story>("story.1");
            std::vector<core::Para> extra;
            for (int k = 0; k < n; ++k) {
                std::string t;
                for (int w = 0; w < words + (k * 7) % 23; ++w) t += (w ? " " : "") + std::string(w % 3 ? "harbor" : "lanterns");
                extra.push_back({"style.1", t + "."});
            }
            s.paras.insert(s.paras.begin() + 4, extra.begin(), extra.end());
            d.put("story.1", s);
        };
        int bad_on = 0, bad_off = 0, seen = 0, splits = 0, seen_off = 0, splits_off = 0;
        // Wide enough that any fonts meet a breach with keeps off (Fedora's
        // found none in the first, 36-layout sweep).
        for (int n : {1, 3, 5})
            for (int words = 5; words <= 120; words += 3) {
                core::Doc d;
                core::load_sample(d);
                with_filler(d, n, words);
                bad_on += breaches(d, &seen, &splits);
                core::Doc e;
                core::load_sample(e);
                for (const char* id : {"style.1", "style.2", "style.10"}) {
                    auto s = *e.as<core::Style>(id);
                    s.keep_next = false;
                    s.keep_lines = 0;
                    e.put(id, s);
                }
                with_filler(e, n, words);
                bad_off += breaches(e, &seen_off, &splits_off);
            }
        check("keeps off: the sweep meets widows, orphans or stranded headings", bad_off > 0,
              std::to_string(bad_off) + " in " + std::to_string(seen_off) + " breaks");
        check("keeps on: none, across the sweep", bad_on == 0 && seen > 60 && splits > 20,
              std::to_string(bad_on) + " in " + std::to_string(seen) + " breaks, " + std::to_string(splits) + " splits");
        {   // The text is all still there (nothing lost to the moves).
            core::Doc d;
            core::load_sample(d);
            with_filler(d, 6, 41);
            const auto& st = *d.as<core::Story>("story.1");
            const auto flow = L::flow_story(d, "story.1");
            int next_para = 0, next_byte = 0;
            bool contiguous = true;
            for (const auto& f : flow.frames)
                for (const auto& l : f.lines) {   // (a drop cap's line holds the first letters)
                    if (l.para != next_para) {
                        contiguous = contiguous && l.para == next_para + 1 && l.start == 0 &&
                                     next_byte == static_cast<int>(st.paras[static_cast<std::size_t>(next_para)].text.size());
                        next_para = l.para;
                    } else {
                        contiguous = contiguous && l.start == next_byte;
                    }
                    next_byte = l.end;
                }
            check("moves lose nothing: lines run on, paragraph after paragraph", contiguous);
        }
        {   // All together: a paragraph that starts in a column is never split from there.
            core::Doc d;
            core::DocJournal j{d};
            d.set_door([&j] { return j.open(); });
            core::load_sample(d);
            with_filler(d, 7, 41);
            check("all together: one step", core::doc::edit_styles(j, d, {"style.1"}, "Keep lines",
                                                                   [](core::Style& s) { s.keep_lines = -1; }));
            check("all together: Body First follows Body", d.as<core::Style>("style.10")->keep_lines == -1);
            int split_starts = 0, n = 0;
            std::vector<std::vector<L::Line>> cols;
            for (const auto& f : L::flow_story(d, "story.1").frames) {
                int last = -1;
                for (const auto& l : f.lines) { if (l.col != last) { cols.emplace_back(); last = l.col; } cols.back().push_back(l); }
            }
            for (std::size_t k = 0; k + 1 < cols.size(); ++k) {
                if (cols[k].empty() || cols[k + 1].empty()) continue;
                ++n;
                bool others = false, starts = false;
                for (const auto& l : cols[k]) {
                    if (l.para == cols[k].back().para) starts = starts || l.start == 0;
                    else others = true;
                }
                split_starts += cols[k].back().para == cols[k + 1].front().para && starts && others;
            }
            check("all together: no paragraph starts and splits in one column", split_starts == 0 && n > 0,
                  std::to_string(split_starts) + " of " + std::to_string(n));
            const auto o = core::project(d, "");
            bool kl = false, kn = false;
            for (const auto& f : o.find("style.1")->fields) kl = kl || (f.edit == "keep_lines" && f.value == "All together");
            for (const auto& f : o.find("style.2")->fields) kn = kn || (f.edit == "keep_next" && f.value == "On");
            check("inspector: Keep lines, Keep with next", kl && kn);
            std::string err;
            core::Doc e;
            check("json: keeps kept", core::from_json(e, core::to_json(d), &err) && e.as<core::Style>("style.1")->keep_lines == -1 &&
                                          e.as<core::Style>("style.2")->keep_next, err);
        }
    }

    {
        std::cout << "\n-- text insets (s040) --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto lines_of = [&](const std::string& story, const std::string& frame) {
            for (const auto& f : L::flow_story(d, story).frames) if (f.frame == frame) return f.lines;
            return std::vector<L::Line>{};
        };
        const auto cap = lines_of("story.2", "frame.6");
        check("sample: the map caption is inset 6 pt", d.as<core::Frame>("frame.6")->inset == core::Sides{6, 6, 6, 6} &&
                                                           !cap.empty() && std::abs(cap[0].y - 6) < 0.01 && std::abs(cap[0].x - 6) < 0.01,
              cap.empty() ? "no lines" : std::to_string(cap[0].x) + "," + std::to_string(cap[0].y));
        check("columns: inside the insets", std::abs(core::column_x(*d.as<core::Frame>("frame.6"), 0) - 6) < 1e-9 &&
                                                std::abs(core::column_width(*d.as<core::Frame>("frame.6"), 0) - (d.as<core::Frame>("frame.6")->w - 12)) < 1e-9);
        // The opening text: an inset of 18 pt moves its first line in and down, and the
        // bottom inset ends the column sooner (more text left over).
        const auto& tf = *d.as<core::Frame>("frame.1");
        const auto before = lines_of("story.1", "frame.1");
        check("inset: one step", core::doc::edit_frames(j, d, {"frame.1"}, "Text inset",
                                                        [](core::Frame& f) { f.inset = {18, 18, 18, 18}; }) && j.size() == 1);
        const auto after = lines_of("story.1", "frame.1");
        check("inset: the first line starts 18 pt down", !after.empty() && std::abs(after[0].y - (before[0].y + 18)) < 0.01,
              std::to_string(before[0].y) + " -> " + std::to_string(after.empty() ? -1 : after[0].y));
        double maxb = 0;
        for (const auto& l : after) maxb = std::max(maxb, l.y + l.h);
        check("inset: no line below the bottom inset", maxb <= tf.h - 18 + 0.01, std::to_string(maxb));
        bool inside = true;
        for (const auto& l : after) inside = inside && l.x >= 18 - 0.01 && (l.w <= 0 || l.x + l.w <= tf.w - 18 + 0.01);
        check("inset: every line inside the left / right insets", inside);
        // Columns split what is left inside the insets.
        core::doc::edit_frames(j, d, {"frame.1"}, "Columns", [](core::Frame& f) { f.columns = 2; });
        const auto& f2 = *d.as<core::Frame>("frame.1");
        check("inset + columns: the second column ends at the right inset",
              std::abs(core::column_x(f2, 1) + core::column_width(f2, 1) - (f2.w - 18)) < 1e-6 && std::abs(core::column_x(f2, 0) - 18) < 1e-9);
        j.undo();
        j.undo();
        check("undo: as it was", lines_of("story.1", "frame.1").size() == before.size() &&
                                     std::abs(lines_of("story.1", "frame.1")[0].y - before[0].y) < 1e-9);
        bool row = false;
        const auto items1 = core::frame_items(d, {"frame.6"});   // (kept: a range-for over a temporary's member dangles)
        for (const auto& f : items1.at(0).fields) row = row || (f.edit == "inset" && f.value == "6\t6\t6\t6");
        check("inspector: the caption's Inset", row);
        std::string err;
        core::Doc e;
        check("json: inset kept", core::from_json(e, core::to_json(d), &err) && e.records() == d.records() &&
                                      e.as<core::Frame>("frame.6")->inset == core::Sides{6, 6, 6, 6}, err);
    }

    {
        std::cout << "\n-- typed fields (s041) --\n";
        const auto is = [](const char* t, double unit, double want) {
            const auto v = core::parse_length(t, unit);
            return v && std::abs(*v - want) < 1e-9;
        };
        check("parse: points", is("12", 1, 12) && is("12.5 pt", 1, 12.5) && is(" 9PT ", 1, 9) && is("-12", 1, -12) && is(".5", 1, 0.5));
        check("parse: inches", is("1in", 1, 72) && is("1.5 inches", 1, 108) && is("2\"", 1, 144) && is("0.75", 72, 54));
        check("parse: picas", is("3p6", 1, 42) && is("6p", 1, 72) && is("p6", 1, 6) && is("2pc", 1, 24) && is("-1p6", 1, -18));
        check("parse: metric", is("25.4mm", 1, 72) && is("2.54 cm", 1, 72));
        check("parse: not lengths", !core::parse_length("", 1) && !core::parse_length("abc", 1) && !core::parse_length("12 furlongs", 1) &&
                                        !core::parse_length("1.2.3", 1) && !core::parse_length("Auto", 1) && !core::parse_length("p", 1));
        check("num_text: trims", core::num_text(12) == "12" && core::num_text(12.5) == "12.5" && core::num_text(1.0 / 3) == "0.333" &&
                                     core::num_text(-0.0001) == "0");
        core::Doc d;
        core::load_sample(d);
        const auto o = core::project(d, "");
        const auto field = [](const std::vector<core::Field>& fs, const std::string& key) -> const core::Field* {
            for (const auto& f : fs) if (f.edit == key) return &f;
            return nullptr;
        };
        const auto* ss = field(o.find("style.1")->fields, "ssize");
        check("style Size: typed in points, its own key (not the page's \"size\")",
              ss && ss->unit == 1 && ss->hi > 100 && !field(o.find("style.1")->fields, "size"));
        const auto* fi = field(o.find("style.1")->fields, "findent");
        check("First indent: typed, may hang (negative)", fi && fi->unit == 1 && fi->lo < 0);
        const auto* mt = field(o.find("master.2")->fields, "mtop");
        check("master margins: typed, a bare number in inches", mt && mt->unit == 72 && core::parse_length(mt->value, mt->unit));
        const auto fr = core::frame_items(d, {"frame.6"});
        const auto* b = field(fr.at(0).fields, "bounds");
        check("frame bounds: X Y W H typed, inches", b && b->unit == 72 && b->cols.size() == 4);
        check("frame inset: typed", field(fr.at(0).fields, "inset") && field(fr.at(0).fields, "inset")->unit == 1);
        const auto two = core::frame_items(d, {"frame.1", "frame.6"});
        std::vector<const core::Item*> sel{&two[0], &two[1]};
        const auto shared = core::shared_fields(sel);   // (kept: a pointer into a temporary dangles)
        const auto* sb = field(shared, "bounds");
        check("two frames: bounds still typed, Mixed where they differ", sb && sb->unit == 72 &&
                                                                              sb->value.find(core::kMixed) != std::string::npos);
        // Every typed field's shown value reads back as a length (or is a word preset: Auto, Inherit, Mixed).
        bool reads = true;
        std::string bad;
        for (const auto& it : o.items())
            for (const auto& f : it.fields)
                if (f.unit > 0 && f.cols.empty() && f.value != core::kMixed && !core::parse_length(f.value, f.unit) &&
                    std::none_of(f.choices.begin(), f.choices.end(), [&](const core::Choice& c) { return c.label == f.value; })) {
                    reads = false;   // (a word shown must be one of its presets: Auto, Inherit, None)
                    bad = f.label + "=" + f.value;
                }
        check("typed values read back as lengths", reads, bad);
    }

    {
        std::cout << "\n-- vertical justification (s042) --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        // The dedication's box, made tall (room to spare on any machine), three paragraphs.
        core::doc::edit_frames(j, d, {"frame.7"}, "Resize", [](core::Frame& f) { f.h = 400; f.inset = {10, 0, 20, 0}; });
        core::doc::edit_text(j, d, "story.3", {0, 3}, {0, 3}, "\n");
        core::doc::edit_text(j, d, "story.3", {1, 6}, {1, 6}, "\n");
        const auto lines = [&] {
            for (const auto& f : L::flow_story(d, "story.3").frames) if (f.frame == "frame.7") return f.lines;
            return std::vector<L::Line>{};
        };
        struct Span { double a, b; };   // (not std::pair: GCC's psabi note)
        const auto span = [](const std::vector<L::Line>& ls) {
            double a = 1e9, b = -1e9;
            for (const auto& l : ls) { a = std::min(a, l.y); b = std::max(b, l.y + l.h); }
            return Span{a, b};
        };
        check("sample: the dedication is centred", d.as<core::Frame>("frame.7")->valign == core::VAlign::Center);
        const auto set = [&](core::VAlign v) {
            core::doc::edit_frames(j, d, {"frame.7"}, "Vertical justification", [&](core::Frame& f) { f.valign = v; });
            return lines();
        };
        const auto top = set(core::VAlign::Top);
        const auto [t0, t1] = span(top);
        check("top: from the top inset", top.size() >= 3 && std::abs(t0 - 10) < 0.01, std::to_string(t0));
        const auto mid = set(core::VAlign::Center);
        const auto [c0, c1] = span(mid);
        check("centre: equal room above and below (inside the insets)", std::abs((c0 - 10) - (380 - c1)) < 0.01 && c0 > t0 + 1,
              std::to_string(c0) + " / " + std::to_string(c1));
        const auto low = set(core::VAlign::Bottom);
        const auto [b0, b1] = span(low);
        check("bottom: the last line ends at the bottom inset", std::abs(b1 - 380) < 0.01 && std::abs((b1 - b0) - (t1 - t0)) < 0.01);
        const auto just = set(core::VAlign::Justify);
        const auto [j0, j1] = span(just);
        bool kept = just.size() == top.size();
        for (std::size_t i = 0; kept && i < just.size(); ++i)
            kept = just[i].para == top[i].para && just[i].start == top[i].start && just[i].y >= top[i].y - 0.01;
        check("justify: fills from the top inset to the bottom one, the same lines", std::abs(j0 - 10) < 0.01 && std::abs(j1 - 380) < 0.01 && kept,
              std::to_string(j0) + " / " + std::to_string(j1));
        bool para_gaps = true;   // lines of one paragraph keep their spacing; the room goes between paragraphs
        for (std::size_t i = 1; i < just.size(); ++i)
            if (just[i].para == just[i - 1].para)
                para_gaps = para_gaps && std::abs((just[i].y - just[i - 1].y) - (top[i].y - top[i - 1].y)) < 0.01;
        check("justify: the room goes between paragraphs", para_gaps);
        // Caret follows the moved lines.
        const auto flow = L::flow_story(d, "story.3");
        const auto cr = L::caret_at(d, "story.3", flow, {2, 0});
        const L::Line* l2 = nullptr;
        for (const auto& l : just) if (!l2 && l.para == 2) l2 = &l;
        check("caret: on the moved line", cr.ok && l2 && std::abs(cr.y - l2->y) < l2->h, std::to_string(cr.y));
        // Text wrap leaves it at the top.
        core::doc::edit_frames(j, d, {"frame.7"}, "Vertical justification", [](core::Frame& f) { f.valign = core::VAlign::Bottom; });
        check("inspector: Vertical", [&] {
            const auto items2 = core::frame_items(d, {"frame.7"});   // (kept: a range-for over a temporary's member dangles)
            for (const auto& f : items2.at(0).fields) if (f.edit == "valign" && f.value == "Bottom") return true;
            return false;
        }());
        std::string err;
        core::Doc e;
        check("json: valign kept", core::from_json(e, core::to_json(d), &err) && e.records() == d.records(), err);
        const auto opening = [&](core::Doc& doc) {
            for (const auto& f : L::flow_story(doc, "story.1").frames) if (f.frame == "frame.1") return f.lines;
            return std::vector<L::Line>{};
        };
        const auto before = opening(d);
        core::doc::edit_frames(j, d, {"frame.1"}, "Vertical justification", [](core::Frame& f) { f.valign = core::VAlign::Bottom; });
        const auto after = opening(d);
        check("text wrap: the opening text (round a picture) stays at the top",
              !before.empty() && after.size() == before.size() && std::abs(after[0].y - before[0].y) < 1e-9);
    }

    {
        std::cout << "\n-- paragraph start (s043) --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        // Where each paragraph of story.1 begins: frame, column, and that frame's page.
        const auto where = [&](int para) {
            const auto flow = L::flow_story(d, "story.1");
            for (const auto& f : flow.frames)
                for (const auto& l : f.lines)
                    if (l.para == para && l.start == 0) return std::tuple{f.frame, l.col, d.as<core::Frame>(f.frame)->owner};
            return std::tuple{std::string{}, -1, std::string{}};
        };
        const auto first_line = [&](const std::string& frame) {
            for (const auto& f : L::flow_story(d, "story.1").frames)
                if (f.frame == frame && !f.lines.empty()) return f.lines.front().para;
            return -1;
        };
        check("sample: Heading 1 starts a right-hand page", d.as<core::Style>("style.2")->start == core::ParaStart::NextRightPage);
        const auto [f1, c1, p1] = where(0);
        check("story start: Chapter One where it is (nothing before it)", f1 == "frame.1");
        const auto [f4, c4, p4] = where(4);
        check("Chapter Two: on a right-hand page after Chapter One's",
              !f4.empty() && p4 != p1 && d.side_of(p4) != core::Side::Left && d.page_index(p4) > d.page_index(p1),
              f4 + " on " + p4);
        check("Chapter Two: first in its frame", first_line(f4) == 4);
        bool between_empty = true;   // frames on pages it passed hold nothing of it
        for (const auto& f : L::flow_story(d, "story.1").frames) {
            const auto pg = d.as<core::Frame>(f.frame)->owner;
            if (d.page_index(pg) > d.page_index(p1) && d.page_index(pg) < d.page_index(p4)) between_empty = between_empty && f.lines.empty();
        }
        check("the pages between: left empty", between_empty);
        // Next page (any side): the very next page with a frame of the thread.
        core::doc::edit_styles(j, d, {"style.2"}, "Start", [](core::Style& s) { s.start = core::ParaStart::NextPage; });
        const auto [g4, gc, gp] = where(4);
        check("next page: a later page, first in its frame", d.page_index(gp) > d.page_index(p1) && first_line(g4) == 4);
        // Next frame: the next frame of the thread.
        core::doc::edit_styles(j, d, {"style.2"}, "Start", [](core::Style& s) { s.start = core::ParaStart::NextFrame; });
        const auto [h4, hc, hp] = where(4);
        check("next frame: frame.2, first in it", h4 == "frame.2" && first_line("frame.2") == 4, h4);
        // Next column: two columns in frame.1 -- Chapter Two heads the second.
        core::doc::edit_styles(j, d, {"style.2"}, "Start", [](core::Style& s) { s.start = core::ParaStart::NextColumn; });
        core::doc::edit_frames(j, d, {"frame.1"}, "Columns", [](core::Frame& f) { f.columns = 2; });
        const auto [k4, kc, kp] = where(4);
        bool heads = true;
        for (const auto& f : L::flow_story(d, "story.1").frames)
            for (const auto& l : f.lines)
                if (f.frame == k4 && l.col == kc && l.para < 4) heads = false;
        check("next column: a fresh column, nothing before it there", !k4.empty() && (kc > 0 || k4 != "frame.1") && heads,
              k4 + " col " + std::to_string(kc));
        // Anywhere: back on page 1 with the rest.
        core::doc::edit_styles(j, d, {"style.2"}, "Start", [](core::Style& s) { s.start = core::ParaStart::Anywhere; });
        core::doc::edit_frames(j, d, {"frame.1"}, "Columns", [](core::Frame& f) { f.columns = 1; });
        check("anywhere: Chapter Two follows on", std::get<0>(where(4)) == "frame.1");
        check("no text lost: nothing overset", !L::flow_story(d, "story.1").overset);
        j.undo(); j.undo();
        const auto o = core::project(d, "");
        bool row = false;
        for (const auto& f : o.find("style.2")->fields) row = row || (f.edit == "pstart" && f.value == "Next column");
        check("inspector: Start", row);
        std::string err;
        core::Doc e;
        check("json: start kept", core::from_json(e, core::to_json(d), &err) && e.records() == d.records(), err);
    }

    {
        std::cout << "\n-- paragraph rules (s044) --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto rules = [&] {
            const auto flow = L::flow_story(d, "story.1");
            return L::paragraph_rules(d, "story.1", flow, "frame.1");
        };
        // The baseline of a paragraph's first / last line in frame.1, as drawn.
        const auto base_of = [&](int para, bool last) {
            const auto flow = L::flow_story(d, "story.1");
            double b = -1;
            L::visit_lines(d, "story.1", flow, "frame.1", [&](const L::LineVisit& v) {
                if (v.para == para && v.from >= 0 && (b < 0 || last)) b = v.baseline;
            });
            return b;
        };
        const auto& fr = *d.as<core::Frame>("frame.1");
        const auto r0 = rules();
        check("sample: one rule under Chapter One (Guild Rust, 1 pt)", r0.size() == 1 && r0[0].color == "swatch.3" &&
                                                                         std::abs(r0[0].r.h - 1) < 1e-9);
        check("below: its top 4 pt under the heading's baseline", !r0.empty() && std::abs(r0[0].r.y - (base_of(0, true) + 4)) < 0.01,
              r0.empty() ? "" : std::to_string(r0[0].r.y) + " vs " + std::to_string(base_of(0, true)));
        check("below: across the column", !r0.empty() && std::abs(r0[0].r.x - core::column_x(fr, 0)) < 0.01 &&
                                             std::abs(r0[0].r.w - core::column_width(fr, 0)) < 0.01);
        // Above, on Body First (a drop cap: its first line is the one beside the cap), text width.
        check("above: one step", core::doc::edit_styles(j, d, {"style.10"}, "Rule", [](core::Style& s) {
            s.rule_above = {2, "", 0.5, 12, true};
        }) && j.size() == 1);
        const auto r1 = rules();
        const core::Rect* above = nullptr;
        for (const auto& m : r1) if (m.color.empty()) above = &m.r;
        const auto flow = L::flow_story(d, "story.1");
        double first_base = -1;   // the line beside the cap: the first non-cap line of para 1
        L::visit_lines(d, "story.1", flow, "frame.1", [&](const L::LineVisit& v) {
            if (v.para == 1 && v.from > 0 && first_base < 0) first_base = v.baseline;
        });
        check("above: its bottom 12 pt over the first line's baseline", above && std::abs(above->y + above->h - (first_base - 12)) < 0.01,
              above ? std::to_string(above->y + above->h) + " vs " + std::to_string(first_base - 12) : "none");
        check("above: text width -- inside the column", above && above->x >= core::column_x(fr, 0) - 0.01 &&
                                                         above->x + above->w <= core::column_x(fr, 0) + core::column_width(fr, 0) + 0.01);
        check("above: Body First's own (Body has none)", d.as<core::Style>("style.1")->rule_above.weight == 0 &&
                                                         std::count(d.as<core::Style>("style.10")->own.begin(),
                                                                    d.as<core::Style>("style.10")->own.end(), "rule_above") == 1);
        // Rules don't move text.
        j.undo();
        const auto before = L::flow_story(d, "story.1");
        core::doc::edit_styles(j, d, {"style.2"}, "Rule", [](core::Style& s) { s.rule_above = {6, "", 1, 30, false}; });
        const auto after = L::flow_story(d, "story.1");
        bool same = before.frames.size() == after.frames.size();
        for (std::size_t i = 0; same && i < before.frames.size(); ++i) {
            same = before.frames[i].lines.size() == after.frames[i].lines.size();
            for (std::size_t k = 0; same && k < before.frames[i].lines.size(); ++k)
                same = before.frames[i].lines[k].y == after.frames[i].lines[k].y && before.frames[i].lines[k].start == after.frames[i].lines[k].start;
        }
        check("rules take no room: the text where it was", same);
        j.undo();
        const auto o = core::project(d, "");
        bool w = false, c = false, ofs = false;
        for (const auto& f : o.find("style.2")->fields) {
            w = w || (f.edit == "rule_b_w" && f.value == "1 pt");
            c = c || (f.edit == "rule_b_c" && f.value == "Guild Rust");
            ofs = ofs || (f.edit == "rule_b_o" && f.value == "4 pt" && f.unit == 1);
        }
        check("inspector: Rule below, its colour and offset (typed)", w && c && ofs);
        std::string err;
        core::Doc e;
        check("json: rules kept", core::from_json(e, core::to_json(d), &err) && e.records() == d.records() &&
                                      e.as<core::Style>("style.2")->rule_below.weight == 1, err);
    }

    {
        std::cout << "\n-- bullets and numbering (s045) --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto& sources = *d.as<core::Story>("story.4");
        check("sample: the Sources are numbered 1. 2. 3.",
              core::doc::list_marker(d, sources, 0) == "1." && core::doc::list_marker(d, sources, 1) == "2." &&
                  core::doc::list_marker(d, sources, 2) == "3.");
        check("not a list: no marker", core::doc::list_marker(d, *d.as<core::Story>("story.1"), 2).empty());
        // The markers as drawn: one per paragraph, left of its first line, on its baseline.
        const auto marks = [&](const std::string& story) {
            std::vector<std::pair<std::string, int>> out;
            const auto flow = L::flow_story(d, story);
            for (const auto& f : flow.frames) {
                std::map<int, double> first_x, first_base, mark_x, mark_base;
                L::visit_lines(d, story, flow, f.frame, [&](const L::LineVisit& v) {
                    if (v.from == -2) {
                        out.push_back({pango_layout_get_text(v.line->layout), v.para});
                        mark_x[v.para] = v.x;
                        mark_base[v.para] = v.baseline;
                    } else if (v.from == 0 && !first_x.count(v.para)) {
                        first_x[v.para] = v.x;
                        first_base[v.para] = v.baseline;
                    }
                });
                for (const auto& [p, x] : mark_x)
                    if (!(x < first_x[p] && std::abs(mark_base[p] - first_base[p]) < 1e-9)) out.push_back({"BAD", p});
            }
            return out;
        };
        const auto m = marks("story.4");
        check("drawn: 1. 2. 3., each left of its text, on its first baseline",
              m.size() == 3 && m[0].first == "1." && m[1].first == "2." && m[2].first == "3.",
              std::to_string(m.size()) + (m.empty() ? "" : " first " + m[0].first));
        // Another style between restarts the count.
        core::doc::set_para_style(j, d, "story.4", {1, 0}, {1, 0}, "style.1");
        check("an interruption restarts: 1. (Body) 1.", core::doc::list_marker(d, *d.as<core::Story>("story.4"), 2) == "1.");
        j.undo();
        // Bullets, letters, romans.
        core::doc::edit_styles(j, d, {"style.4"}, "Numbers", [](core::Style& s) { s.list.numbering = core::Numbering::RomanLower; });
        check("roman: i. ii. iii.", core::doc::list_marker(d, *d.as<core::Story>("story.4"), 2) == "iii.");
        core::doc::edit_styles(j, d, {"style.4"}, "Numbers", [](core::Style& s) { s.list.numbering = core::Numbering::AlphaUpper; });
        check("letters: A. B. C.", core::doc::list_marker(d, *d.as<core::Story>("story.4"), 1) == "B.");
        core::doc::edit_styles(j, d, {"style.4"}, "List", [](core::Style& s) { s.list.type = core::ListType::Bullet; });
        check("bullets: every paragraph a bullet", core::doc::list_marker(d, *d.as<core::Story>("story.4"), 2) == "•");
        // The text's first line lines up with the rest (the marker hangs, not the words).
        {
            const auto flow = L::flow_story(d, "story.4");
            double x0 = -1, x1 = -1;
            for (const auto& f : flow.frames)
                for (const auto& l : f.lines)
                    if (l.para == 0) { if (l.start == 0) x0 = l.x; else if (x1 < 0) x1 = l.x; }
            check("a list's first line starts with the rest", x0 >= 0 && (x1 < 0 || std::abs(x0 - x1) < 0.01));
        }
        // Off: no markers drawn.
        core::doc::edit_styles(j, d, {"style.4"}, "List", [](core::Style& s) { s.list.type = core::ListType::None; });
        check("off: no markers", marks("story.4").empty());
        j.undo(); j.undo(); j.undo(); j.undo();
        const auto o = core::project(d, "");
        bool lt = false, nm = false, hg = false;
        for (const auto& f : o.find("style.4")->fields) {
            lt = lt || (f.edit == "list_type" && f.value == "Numbers");
            nm = nm || (f.edit == "list_num" && f.value == "1. 2. 3.");
            hg = hg || (f.edit == "list_hang" && f.value == "12 pt" && f.unit == 1);
        }
        check("inspector: List, Numbers, Hang (typed)", lt && nm && hg);
        std::string err;
        core::Doc e;
        check("json: list kept", core::from_json(e, core::to_json(d), &err) && e.records() == d.records() &&
                                     e.as<core::Style>("style.4")->list.type == core::ListType::Number, err);
    }

    {
        std::cout << "\n-- character styles on character styles (s046) --\n";
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto S = [&](const char* id) { return *d.as<core::Style>(id); };
        check("sample: Strong Emphasis on Emphasis, owning only its weight",
              S("style.11").based_on == "style.5" && S("style.11").own == std::vector<std::string>{"bold"} &&
                  S("style.11").bold && S("style.11").italic);
        check("kinds don't mix", !core::doc::can_base_on(d, "style.11", "style.1") && !core::doc::can_base_on(d, "style.1", "style.5") &&
                                     core::doc::can_base_on(d, "style.5", "style.11") == false);   // (a loop)
        core::doc::edit_styles(j, d, {"style.5"}, "Font", [](core::Style& s) { s.family = "Sans"; s.color = "swatch.3"; });
        check("edit the base: the font and colour follow", S("style.11").family == "Sans" && S("style.11").color == "swatch.3");
        core::doc::edit_styles(j, d, {"style.5"}, "Slant", [](core::Style& s) { s.italic = false; });
        check("edit the base: roman now, still bold (its own)", !S("style.11").italic && S("style.11").bold);
        j.undo(); j.undo();
        check("undo: as it was", S("style.11").family.empty() && S("style.11").italic);
        // Rebased onto None: it keeps its look.
        const auto before = S("style.11");
        check("based on None: one step, the look kept", core::doc::set_style_base(j, d, "style.11", "") && [&] {
            auto now = S("style.11");
            now.based_on = before.based_on;
            now.own = before.own;
            return now == before;
        }());
        j.undo();
        const auto o = core::project(d, "");
        bool base = false, next = false, own = false;
        for (const auto& f : o.find("style.11")->fields) {
            base = base || (f.edit == "based_on" && f.value == "Emphasis");
            next = next || f.edit == "next_style";
            own = own || (f.label == "Own settings" && f.value == "Weight");
        }
        check("inspector: Based on and Own settings for a character style (no Next style)", base && own && !next);
        bool para_only = true;   // a character style's Based on offers character styles only
        for (const auto& f : o.find("style.11")->fields)
            if (f.edit == "based_on")
                for (const auto& c : f.choices)
                    if (!c.id.empty()) para_only = para_only && d.as<core::Style>(c.id)->kind == core::StyleKind::Character;
        check("inspector: its Based on lists character styles only", para_only);
    }

    {
        std::cout << "\n-- list numbering options (s047) --\n";
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto marks = [&] {
            std::string out;
            const auto& st = *d.as<core::Story>("story.4");
            for (int p = 0; p < static_cast<int>(st.paras.size()); ++p) {
                const auto m = core::doc::list_marker(d, st, p);
                out += (p ? " " : "") + (m.empty() ? std::string("-") : m);
            }
            return out;
        };
        check("sample: 1. 2. 3.", marks() == "1. 2. 3.", marks());
        // A Body paragraph in the middle.
        core::doc::set_para_style(j, d, "story.4", {1, 0}, {1, 0}, "style.1");
        check("other paragraphs restart the count (as s045)", marks() == "1. - 1.", marks());
        core::doc::edit_styles(j, d, {"style.4"}, "Numbering", [](core::Style& s) { s.list.continues = true; });
        check("carries on: past the Body paragraph", marks() == "1. - 2.", marks());
        j.undo(); j.undo();
        // Five paragraphs, a second level between the first.
        const auto end2 = static_cast<int>(d.as<core::Story>("story.4")->paras[2].text.size());
        core::doc::edit_text(j, d, "story.4", {2, end2}, {2, end2}, "\nFour\nFive");
        const auto sub = core::doc::duplicate_style(j, d, "style.4");
        core::doc::edit_styles(j, d, {sub}, "Level", [](core::Style& s) {
            s.list.level = 2;
            s.list.numbering = core::Numbering::AlphaLower;
            s.left_indent = 24;
        });
        core::doc::set_para_style(j, d, "story.4", {1, 0}, {2, 0}, sub);
        core::doc::set_para_style(j, d, "story.4", {4, 0}, {4, 0}, sub);
        check("level 2: a. b. under 1., a. again under 2.", marks() == "1. a. b. 2. a.", marks());
        core::doc::edit_styles(j, d, {"style.4"}, "Numbering", [](core::Style& s) { s.list.continues = true; });
        check("level 1 carrying on past its level-2 items", marks() == "1. a. b. 2. a.", marks());
        // Restart at, by hand.
        check("restart: one step", core::doc::set_list_restart(j, d, "story.4", {3, 0}, {3, 0}, 7));
        check("restart at 7: the next level-1 is 7.", marks() == "1. a. b. 7. a.", marks());
        core::doc::edit_text(j, d, "story.4", {3, 0}, {3, 0}, "The ");
        check("typing in it keeps the restart", d.as<core::Story>("story.4")->paras[3].restart == 7 && marks() == "1. a. b. 7. a.");
        const auto e3 = static_cast<int>(d.as<core::Story>("story.4")->paras[3].text.size());
        core::doc::edit_text(j, d, "story.4", {3, e3}, {3, e3}, "\n");
        check("Enter after it: the new paragraph counts on (8.)", d.as<core::Story>("story.4")->paras[4].restart == 0 &&
                                                                 core::doc::list_marker(d, *d.as<core::Story>("story.4"), 4) == "8.",
              marks());
        bool row = false;
        for (const auto& f : core::text_fields(d, "story.4", {3, 0}, {3, 0})) row = row || (f.edit == "list_from" && f.value == "Restart at 7");
        check("inspector (typing): Number -- Restart at 7", row);
        std::string err;
        core::Doc e;
        check("json: restart, carries on, level kept", core::from_json(e, core::to_json(d), &err) && e.records() == d.records(), err);
        core::doc::set_list_restart(j, d, "story.4", {3, 0}, {3, 0}, 0);
        check("counted again", core::doc::list_marker(d, *d.as<core::Story>("story.4"), 3) == "2.", marks());
    }

    {
        std::cout << "\n-- auto-size frames (s048) --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        j.set_settle([&d] { return L::fit_frames(d); });   // as the app does
        core::load_sample(d);
        const auto bottom = [&](const char* story, const char* frame) {
            double b = -1;
            for (const auto& f : L::flow_story(d, story).frames)
                if (f.frame == frame) for (const auto& l : f.lines) b = std::max(b, l.y + l.h);
            return b;
        };
        const auto F = [&](const char* id) { return *d.as<core::Frame>(id); };
        const double h0 = F("frame.6").h;
        check("auto-size on: one step", core::doc::edit_frames(j, d, {"frame.6"}, "Auto-size", [](core::Frame& f) { f.autosize = true; }) &&
                                            j.size() == 1);
        check("the caption's height fits its text (+ its 6 pt bottom inset)",
              std::abs(F("frame.6").h - (bottom("story.2", "frame.6") + 6)) < 0.01, std::to_string(F("frame.6").h));
        const double fitted = F("frame.6").h;
        // Typing grows it -- in the same step as the typing.
        std::string more;
        for (int k = 0; k < 30; ++k) more += " The routes run east along the coast.";
        const auto end = static_cast<int>(d.as<core::Story>("story.2")->paras[0].text.size());
        core::doc::edit_text(j, d, "story.2", {0, end}, {0, end}, more);
        check("typing: the frame grows with it, one step", F("frame.6").h > fitted + 10 && j.size() == 2 &&
                                                               std::abs(F("frame.6").h - (bottom("story.2", "frame.6") + 6)) < 0.01);
        check("typing: nothing overset", !L::flow_story(d, "story.2").overset);
        j.undo();
        check("undo the typing: back to its fitted height", std::abs(F("frame.6").h - fitted) < 1e-9);
        j.undo();
        check("undo auto-size: its height as it was", std::abs(F("frame.6").h - h0) < 1e-9 && !F("frame.6").autosize);
        j.redo();
        check("redo: fitted again", std::abs(F("frame.6").h - fitted) < 1e-9);
        // A frame that threads on is left alone.
        const double h1 = F("frame.1").h;
        core::doc::edit_frames(j, d, {"frame.1"}, "Auto-size", [](core::Frame& f) { f.autosize = true; });
        check("a threaded frame: not resized", std::abs(F("frame.1").h - h1) < 1e-9);
        bool row = false;
        const auto items3 = core::frame_items(d, {"frame.1"});   // (kept: a range-for over a temporary's member dangles)
        for (const auto& f : items3.at(0).fields) row = row || (f.edit == "autosize" && f.value == "Height (not here)");
        check("inspector: says when it can't", row);
        std::string err;
        core::Doc e;
        check("json: autosize kept", core::from_json(e, core::to_json(d), &err) && e.records() == d.records(), err);
    }

    {
        std::cout << "\n-- story editor formatting (s049) --\n";
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto looks = [&](const char* story, int p) { return core::word_looks(d, d.as<core::Story>(story)->paras[static_cast<std::size_t>(p)]); };
        const auto w0 = looks("story.4", 0);
        check("Sources: the title in Emphasis -- italic, its character style", w0.size() == 1 && w0[0].start == 0 && w0[0].end == 26 &&
                                                                               w0[0].italic && !w0[0].bold && w0[0].cstyle == "style.5");
        check("plain words: nothing", looks("story.4", 1).empty() && looks("story.1", 2).empty());
        check("a bold heading's own weight is the paragraph style's, not shown", looks("story.1", 0).empty());
        core::doc::set_local(j, d, "story.4", {1, 0}, {1, 6}, "Bold", [](core::Local& l) { l.bold = 1; });
        const auto w1 = looks("story.4", 1);
        check("local bold: its span, bold, no character style", w1.size() == 1 && w1[0].start == 0 && w1[0].end == 6 && w1[0].bold &&
                                                                 w1[0].cstyle.empty());
        core::doc::set_local(j, d, "story.4", {0, 0}, {0, 7}, "Italic", [](core::Local& l) { l.italic = 0; });
        const auto w2 = looks("story.4", 0);
        check("local roman over italic Emphasis: that part not italic, still Emphasis",
              w2.size() == 2 && !w2[0].italic && w2[0].cstyle == "style.5" && w2[1].italic, std::to_string(w2.size()));
        j.undo(); j.undo();
        check("undo: as it was", looks("story.4", 0).size() == 1 && looks("story.4", 1).empty());
    }

    {
        std::cout << "\n-- the long test book (s100) --\n";
        namespace L = composix::layout;
        // A short one (the full ~400 pages is composix_bench's): the rules
        // are the same at any length. No picture folder: missing links.
        core::Doc d;
        const auto info = L::load_big_book(d, {.pages = 20});
        const auto flow = L::flow_story(d, "story.1");
        check("big book: the long story fits (nothing overset)", !flow.overset);
        const auto body = d.pages_of(d.ids_of<core::Section>().back());
        const auto thread = L::thread_frames(d, "story.1");
        check("big book: one thread through every body page", thread.size() == body.size() && !body.empty(),
              std::to_string(thread.size()) + " frames, " + std::to_string(body.size()) + " pages");
        check("big book: about the length asked", info.pages >= 12 && info.pages <= 40, std::to_string(info.pages) + " pages");
        check("big book: the last page has text (no empty pages at the end)",
              !flow.frames.empty() && flow.frames.back().frame == thread.back() && !flow.frames.back().lines.empty());
        // Every chapter opens a right-hand page set on the chapter master,
        // with its picture in the sink; no other page is.
        const auto& paras = d.as<core::Story>("story.1")->paras;
        int heads = 0, opened = 0, bad = 0;
        for (const auto& p : paras) heads += p.style == "style.2";
        for (const auto& ft : flow.frames) {
            const auto* f = d.as<core::Frame>(ft.frame);
            const bool opens = !ft.lines.empty() && ft.lines.front().start == 0 &&
                               paras[static_cast<std::size_t>(ft.lines.front().para)].style == "style.2";
            const auto* pg = d.as<core::Page>(f->owner);
            int pics = 0;
            for (const auto& id : d.frames_on(f->owner)) pics += d.as<core::Frame>(id)->kind == core::FrameKind::Image;
            if (opens) {
                ++opened;
                if (d.side_of(f->owner) != core::Side::Right || pg->master != "master.3" || pics < 1) ++bad;
            } else if (pg->master == "master.3") {
                ++bad;
            }
        }
        check("big book: every chapter opens a right page on the chapter master, its picture over it",
              heads == info.chapters && opened == heads && bad == 0,
              std::to_string(heads) + " heads, " + std::to_string(opened) + " opened, " + std::to_string(bad) + " wrong");
        check("big book: section heads and pictures in it", info.sections > 0 && info.pictures > info.chapters);
        L::FlowCache flows;
        check("big book: its contents are current, one entry a chapter",
              L::stale_lists(d, flows).empty() && d.as<core::GenList>("list.1")->built_entries == info.chapters);
        core::Doc d2;
        (void)L::load_big_book(d2, {.pages = 20});
        check("big book: the same every time (seeded greeking)", d2.records() == d.records());
    }

    {
        std::cout << "\n-- page order, kept (s101) --\n";
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto scan = [&] {   // what page_index was: a count, record by record
            bool ok = true;
            const auto ps = d.pages();
            for (std::size_t i = 0; i < ps.size(); ++i) ok = ok && d.page_index(ps[i]) == static_cast<int>(i);
            return ok && d.page_index("page.none") == -1;
        };
        check("page order: every page at its place", scan());
        const auto added = core::doc::add_pages(j, d, "page.2", 2, "master.1");
        check("page order: follows pages added in the middle", scan() && added.size() == 2 &&
                                                               d.page_index(added[0]) == 2 && d.page_index("page.3") == 4);
        core::doc::move_pages(j, d, {"page.1"}, "page.5");
        check("page order: follows a move", scan());
        j.undo(); j.undo();
        check("page order: and undo", scan() && d.page_index("page.3") == 2 && d.page_index(added[0]) == -1);
    }

    {
        std::cout << "\n-- layout off the UI thread (s102) --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::load_sample(d);
        const auto here = L::flow_story(d, "story.1");
        L::StoryFlow there;
        std::thread([&] { there = L::flow_story(d, "story.1"); }).join();   // its own Pango context
        bool same = here.frames.size() == there.frames.size() && here.overset == there.overset;
        for (std::size_t i = 0; same && i < here.frames.size(); ++i) {
            const auto& a = here.frames[i].lines;
            const auto& b = there.frames[i].lines;
            same = a.size() == b.size();
            for (std::size_t k = 0; same && k < a.size(); ++k)
                same = a[k].para == b[k].para && a[k].start == b[k].start && a[k].end == b[k].end && a[k].y == b[k].y;
        }
        check("a worker thread breaks the lines exactly as the UI thread", same && here.lines > 0);
        int reports = 0;
        double last = -1;
        bool rising = true;
        core::Doc b;
        (void)L::load_big_book(b, {.pages = 20, .progress = [&](double f, const std::string&) {
            ++reports;
            rising = rising && f >= last;
            last = f;
        }});
        check("big book: says how far along, rising", reports > 3 && rising && last > 0.9);
    }

    {
        std::cout << "\n-- progress you can see (s102) --\n";
        namespace L = composix::layout;
        core::Doc d;
        (void)L::load_big_book(d, {.pages = 20});
        const auto dir = std::filesystem::temp_directory_path();
        for (const bool print : {false, true}) {
            L::PdfOptions o;
            int calls = 0;
            double last = -1;
            bool rising = true, wrote = false;
            o.progress = [&](double f, const std::string& line) {
                ++calls;
                rising = rising && f >= last;
                last = f;
                wrote = wrote || line.rfind("Writing page", 0) == 0;
            };
            std::string err;
            const auto path = (dir / (print ? "cx_progress_print.pdf" : "cx_progress_screen.pdf")).string();
            const bool ok = print ? L::export_print_pdf(d, path, o, {}, nullptr, &err) : L::export_pdf(d, path, o, {}, nullptr, &err);
            check(std::string(print ? "print" : "screen") + " PDF: progress page by page, rising, near the end",
                  ok && calls > static_cast<int>(d.pages().size()) && rising && wrote && last > 0.85, err);
            std::filesystem::remove(path);
        }
        // A layout made from a copy, handed to the document it was copied from.
        core::Doc copy;
        {
            core::Doc::Loading load(copy);
            for (const auto& [id, r] : d.records()) copy.put(id, r);
        }
        L::FlowCache made;
        int ticks = 0, total = 0;
        L::flow_all(copy, made, [&](int done, int t) { ticks = done; total = t; });
        check("flow_all: counts every frame of every story", total > 0 && ticks == total);
        const auto* before = &made.get(copy, "story.1");
        const int nlines = before->lines;
        made.rebase(d);
        const auto* after = &made.get(d, "story.1");
        check("a handed-over layout is the document's own (kept, not flowed again)", after == before && after->lines == nlines && nlines > 0);
    }


    {
        std::cout << "\n-- thumbnails on a worker (s103) --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        L::FlowCache flows;
        const L::ImageSource none;
        const auto same_pixels = [](const L::Surface& a, const L::Surface& b) {
            if (!a || !b) return false;
            const int w = cairo_image_surface_get_width(a.get()), h = cairo_image_surface_get_height(a.get());
            if (w != cairo_image_surface_get_width(b.get()) || h != cairo_image_surface_get_height(b.get())) return false;
            const int st = cairo_image_surface_get_stride(a.get());
            return std::memcmp(cairo_image_surface_get_data(a.get()), cairo_image_surface_get_data(b.get()),
                               static_cast<std::size_t>(st) * h) == 0;
        };
        // What the UI thread would draw, page 5, 120 x 180.
        const auto here = L::render_thumb(d, flows, "page.5", 120, 180, false, none);

        L::ThumbStore st;
        st.set_threaded(true);
        auto lk = st.look(d, flows, none, "page.5", "page.5", 120, 180, false);
        check("worker: a miss is asked for, nothing to paint yet", !lk.surf && !lk.current && st.asking());
        (void)st.look(d, flows, none, "page.5", "page.5", 120, 180, false);
        (void)st.look(d, flows, none, "page.6", "page.6", 120, 180, false);
        check("worker: asked once per page", st.flush(d, flows, none) && !st.asking());
        st.wait_idle();
        auto ids = st.collect();
        lk = st.look(d, flows, none, "page.5", "page.5", 120, 180, false);
        check("worker: back, and current", ids.count("page.5") && ids.count("page.6") && lk.current && st.renders() == 2);
        check("worker: draws the page exactly as the UI thread", same_pixels(lk.surf, here));
        check("worker: one snapshot for the revision", st.snapshots() == 1);

        // A change: the page's last picture shows (stale) until the new one.
        core::doc::edit_text(j, d, "story.1", {1, 0}, {1, 0}, "Then ", "Typing", "");
        lk = st.look(d, flows, none, "page.5", "page.5", 120, 180, false);
        check("worker: after a change the old picture stands in", lk.surf && !lk.current);
        // The worker held: two asks for the page, the document changed between.
        st.hold(true);
        (void)st.flush(d, flows, none);
        const auto old_key = L::page_key(d, flows, "page.5");
        const auto at_old = L::render_thumb(d, flows, "page.5", 120, 180, false, none);
        core::doc::edit_text(j, d, "story.1", {1, 0}, {1, 0}, "And ", "Typing", "");
        (void)st.look(d, flows, none, "page.5", "page.5", 120, 180, false);
        (void)st.flush(d, flows, none);
        const int before = st.renders();
        st.hold(false);
        st.wait_idle();
        ids = st.collect();
        lk = st.look(d, flows, none, "page.5", "page.5", 120, 180, false);
        check("worker: the newest ask wins, the older is dropped", st.renders() == before + 1 && lk.current);
        check("worker: drawn from the newest revision", !same_pixels(lk.surf, at_old) &&
                                                       same_pixels(lk.surf, L::render_thumb(d, flows, "page.5", 120, 180, false, none)));
        // The snapshot is the document as it was: changes after it don't reach it.
        st.hold(true);
        core::doc::edit_text(j, d, "story.1", {1, 0}, {1, 0}, "So ", "Typing", "");
        (void)st.look(d, flows, none, "page.5", "page.5", 120, 180, false);
        (void)st.flush(d, flows, none);
        const auto snap_pic = L::render_thumb(d, flows, "page.5", 120, 180, false, none);
        j.undo();   // the live document moves on while the worker waits
        st.hold(false);
        st.wait_idle();
        (void)st.collect();
        lk = st.look(d, flows, none, "page.5", "page.5", 120, 180, false);
        check("worker: reads its snapshot, not the live document", !lk.current && lk.surf && same_pixels(lk.surf, snap_pic));
        (void)old_key;
        // Another document: what was asked doesn't come back.
        st.hold(true);
        (void)st.flush(d, flows, none);
        st.forget();
        st.hold(false);
        st.wait_idle();
        check("worker: forget drops what's queued and finished", st.collect().empty() && st.kept() == 0 && st.bytes() == 0);

        // The cap: the least recently looked at go first.
        L::ThumbStore lru(3 * 120 * 180 * 4);
        for (const char* p : {"page.1", "page.2", "page.3", "page.4"})
            (void)lru.look(d, flows, none, p, p, 120, 180, false);
        check("cap: kept within its bytes", lru.bytes() <= lru.cap() && lru.kept() == 3);
        check("cap: the oldest let go, the newest kept",
              lru.look(d, flows, none, "page.4", "page.4", 120, 180, false).current && lru.renders() == 4);
        (void)lru.look(d, flows, none, "page.1", "page.1", 120, 180, false);
        check("cap: a page let go is drawn again when looked at", lru.renders() == 5);
    }

    {
        std::cout << "\n-- the store's general form: a variant, a renderer, a mode (s104) --\n";
        namespace L = composix::layout;
        using Miss = L::ThumbStore::Miss;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        L::FlowCache flows;
        int drawn = 0;
        const auto at = [&](int w) {
            return L::ThumbStore::Render([&drawn, w](const core::Doc& doc, L::FlowCache& fl, const L::ImageSource& im) {
                ++drawn;
                return L::render_thumb(doc, fl, "page.5", w, w * 3 / 2, false, im);
            });
        };
        L::ThumbStore st;
        auto lk = st.look(d, flows, {}, "p5", "page.5", "z1", 100, 150, at(100), Miss::NowIfNone);
        check("NowIfNone: nothing yet -> drawn on the spot, current", lk.current && lk.surf && drawn == 1 && !st.asking());
        lk = st.look(d, flows, {}, "p5", "page.5", "z2", 120, 180, at(120), Miss::NowIfNone);
        check("NowIfNone: another variant -> the last stands in, the new is asked",
              !lk.current && lk.surf && cairo_image_surface_get_width(lk.surf.get()) == 100 && st.asking() && drawn == 1);
        st.hold(true);
        (void)st.flush(d, flows, {});
        // Meanwhile the page is drawn on the spot (an edit, say) for z2: the
        // worker's answer, asked before, must not replace it.
        lk = st.look(d, flows, {}, "p5", "page.5", "z3", 140, 210, at(140), Miss::Now);
        check("Now: drawn on the spot", lk.current && drawn == 2);
        st.hold(false);
        st.wait_idle();
        (void)st.collect();
        lk = st.look(d, flows, {}, "p5", "page.5", "z3", 140, 210, at(140), Miss::NowIfNone);
        check("an older answer doesn't replace a newer picture", lk.current && drawn == 3 && st.renders() == 3);
        lk = st.look(d, flows, {}, "p5", "page.5", "z1", 100, 150, at(100), Miss::Ask);
        check("Ask: a variant back again is asked, not drawn here", !lk.current && st.asking() && drawn == 3);
    }

    {
        std::cout << "\n-- a long book's tiles off the UI thread (s103) --\n";
        namespace L = composix::layout;
        core::Doc d;
        (void)L::load_big_book(d, {.pages = 40});
        L::FlowCache flows;
        L::flow_all(d, flows, {});
        L::ThumbStore st;
        st.set_threaded(true);
        const auto pages = d.pages();
        for (const auto& p : pages) (void)st.look(d, flows, {}, p, p, 80, 120, false);
        (void)st.flush(d, flows, {});
        st.wait_idle();
        const auto ids = st.collect();
        bool all = ids.size() == pages.size();
        for (const auto& p : pages) all = all && st.look(d, flows, {}, p, p, 80, 120, false).current;
        check("every page of a 40-page book drawn on the worker, current", all && st.renders() == static_cast<int>(pages.size()));
    }
    {
        std::cout << "\n-- CMYK drawn through the press profile (s107) --\n";
        core::Swatch cm{"Ink", 1, core::ColorModel::CMYK, {10, 20, 30, 40}, "", 0x123456};
        core::Swatch rgb{"Sky", 2, core::ColorModel::RGB, {10, 20, 30, 0}, "", 0x0a141e};
        const auto g0 = core::doc::screen_gen();
        check("no profile: the preview", core::doc::screen_rgb(cm) == 0x123456 && core::doc::screen_rgb(rgb) == 0x0a141e);
        core::doc::set_screen_cmyk([](const double v[4]) { return static_cast<std::uint32_t>(v[0] * 1000 + v[3]); });
        check("a profile: CMYK through it, RGB as it is, and caches told",
              core::doc::screen_rgb(cm) == 10040 && core::doc::screen_rgb(rgb) == 0x0a141e && core::doc::screen_gen() > g0);
        core::Swatch spot = cm;
        spot.model = core::ColorModel::Spot;
        check("a spot: its CMYK alternate through it", core::doc::screen_rgb(spot) == 10040);
        core::doc::set_screen_cmyk({});
        check("back to the ink model", core::doc::screen_rgb(cm) == 0x123456);
    }

    {
        std::cout << "\n-- a keystroke re-flows from the edit (s105) --\n";
        namespace L = composix::layout;
        const auto same_flow = [](const L::StoryFlow& a, const L::StoryFlow& b, std::string& why) {
            if (a.overset != b.overset) { why = "overset"; return false; }
            if (a.frames.size() != b.frames.size()) { why = "frames"; return false; }
            for (std::size_t i = 0; i < a.frames.size(); ++i) {
                const auto& x = a.frames[i].lines;
                const auto& y = b.frames[i].lines;
                if (x.size() != y.size()) { why = "frame " + std::to_string(i) + " lines " + std::to_string(x.size()) + " vs " + std::to_string(y.size()); return false; }
                for (std::size_t k = 0; k < x.size(); ++k) {
                    const auto& l = x[k];
                    const auto& m = y[k];
                    if (l.para != m.para || l.start != m.start || l.end != m.end || std::abs(l.y - m.y) > 1e-9 ||
                        l.h != m.h || l.col != m.col || std::abs(l.x - m.x) > 1e-9 || std::abs(l.w - m.w) > 1e-9 ||
                        l.cap != m.cap || std::abs(l.base - m.base) > 1e-9 || l.hyphen != m.hyphen) {
                        why = "frame " + std::to_string(i) + " line " + std::to_string(k);
                        return false;
                    }
                }
                if (a.frames[i].from_p != b.frames[i].from_p || a.frames[i].from_b != b.frames[i].from_b) {
                    why = "frame " + std::to_string(i) + " start";
                    return false;
                }
                const auto& na = a.frames[i].notes;   // s109
                const auto& nb = b.frames[i].notes;
                bool notes_same = na.size() == nb.size();
                for (std::size_t k = 0; notes_same && k < na.size(); ++k)
                    notes_same = na[k].para == nb[k].para && na[k].k == nb[k].k && na[k].number == nb[k].number &&
                                 na[k].col == nb[k].col && std::abs(na[k].y - nb[k].y) < 1e-9 && std::abs(na[k].h - nb[k].h) < 1e-9;
                if (!notes_same) {
                    why = "frame " + std::to_string(i) + " notes";
                    return false;
                }
            }
            return true;
        };
        {
            core::Doc d;
            core::DocJournal j{d};
            d.set_door([&j] { return j.open(); });
            (void)L::load_big_book(d, {.pages = 24});
            L::FlowCache cache;
            (void)cache.get(d, "story.1");
            std::vector<std::string> ids;
            core::doc::edit_text(j, d, "story.1", {5, 0}, {5, 0}, "x", "Typing", "");
            check("a keystroke writes only its story", d.changes_since(d.revision() - 1, ids) && ids.size() == 1 && ids[0] == "story.1");
            // Edits of every kind, at places through the book: each re-flowed
            // flow must be the whole flow, line for line.
            std::uint32_t seed = 7;
            const auto rnd = [&](std::uint32_t n) { seed = seed * 1103515245u + 12345u; return (seed >> 8) % std::max(1u, n); };
            const char* inserts[] = {"x", "Lorem ipsum dolor sit amet, ", "\n", "a much longer run of words to push a line or two on, and more words still to make it so ", core::kMarkNote, "\nA new paragraph.\n"};
            int bad = 0, reflowed = 0, kept = 0, partial = 0;
            std::string why;
            for (int n = 0; n < 40; ++n) {
                const auto* st = d.as<core::Story>("story.1");
                const int np = static_cast<int>(st->paras.size());
                const int p = static_cast<int>(rnd(static_cast<std::uint32_t>(np)));
                const auto& t = st->paras[static_cast<std::size_t>(p)].text;
                // A word boundary (ASCII greeking: any space).
                int at = 0;
                for (int tries = 0; tries < 8; ++tries) {
                    const int k = static_cast<int>(rnd(static_cast<std::uint32_t>(t.size() + 1)));
                    if (k == 0 || k == static_cast<int>(t.size()) || t[static_cast<std::size_t>(k - 1)] == ' ') { at = k; break; }
                }
                core::TextPos a{p, at}, b = a;
                const int kind = static_cast<int>(rnd(6));
                if (n % 5 == 3 && p + 1 < np) b = {p + 1, 0};                                   // join with the next
                else if (n % 7 == 4 && at + 5 <= static_cast<int>(t.size()) && t.find(' ', static_cast<std::size_t>(at)) != std::string::npos)
                    b = {p, static_cast<int>(t.find(' ', static_cast<std::size_t>(at)))};       // a word taken out
                core::doc::edit_text(j, d, "story.1", a, b, inserts[kind], "Typing", "");
                const auto& inc = cache.get(d, "story.1");
                reflowed += cache.last_how() == L::FlowCache::How::Reflowed;
                kept += cache.last_how() == L::FlowCache::How::Kept;   // an edit that changed nothing
                partial += inc.filled < static_cast<int>(inc.frames.size()) / 2;
                const auto whole = L::flow_story(d, "story.1");
                if (!same_flow(inc, whole, why)) {
                    if (bad++ == 0) check("re-flow == whole flow (first difference)", false, "edit " + std::to_string(n) + ": " + why);
                }
            }
            check("40 edits through a 24-page book: every re-flow is the whole flow", bad == 0, std::to_string(bad) + " differ");
            check("each was re-flowed, not flowed whole", reflowed + kept == 40 && reflowed >= 30,
                  std::to_string(reflowed) + " re-flowed, " + std::to_string(kept) + " no change");
            check("most set fewer than half the frames", partial >= 10, std::to_string(partial) + " of 40");
            // Undo and redo are edits too.
            j.undo();
            const bool u = same_flow(cache.get(d, "story.1"), L::flow_story(d, "story.1"), why);
            j.redo();
            const bool r = same_flow(cache.get(d, "story.1"), L::flow_story(d, "story.1"), why);
            check("undo / redo: re-flowed as the whole", u && r, why);
            // Another story's flow is kept through an edit of this one.
            const auto* title = &cache.get(d, "story.2");
            core::doc::edit_text(j, d, "story.1", {3, 0}, {3, 0}, "y", "Typing", "");
            (void)cache.get(d, "story.1");
            check("an edit leaves other stories' flows as they were", &cache.get(d, "story.2") == title &&
                                                                     cache.last_how() == L::FlowCache::How::Kept);
            // Anything but text: the whole again.
            core::doc::edit_frames(j, d, {d.as<core::Story>("story.1") ? L::thread_frames(d, "story.1").front() : ""}, "Gutter",
                                   [](core::Frame& f) { f.gutter += 1; });
            (void)cache.get(d, "story.1");
            check("a frame changed: flowed whole", cache.last_how() == L::FlowCache::How::Whole);
        }
        {   // s106: a budget -- the far end provisional, then the whole handed in.
            core::Doc d;
            core::DocJournal j{d};
            d.set_door([&j] { return j.open(); });
            (void)L::load_big_book(d, {.pages = 24});
            L::FlowCache cache;
            cache.set_reflow_budget(2);
            (void)cache.get(d, "story.1");
            const auto valid = [&](const L::StoryFlow& f) {   // every line's text is there
                const auto* st = d.as<core::Story>("story.1");
                for (const auto& ft : f.frames)
                    for (const auto& l : ft.lines)
                        if (l.para < 0 || l.para >= static_cast<int>(st->paras.size()) || l.start < 0 ||
                            l.end > static_cast<int>(st->paras[static_cast<std::size_t>(l.para)].text.size()) || l.start > l.end)
                            return false;
                return true;
            };
            const auto head_same = [&](const L::StoryFlow& f, const L::StoryFlow& whole) {
                if (f.tail_from < 0 || f.tail_from > static_cast<int>(f.frames.size())) return false;
                std::string w;
                L::StoryFlow a = f, b = whole;
                a.frames.resize(static_cast<std::size_t>(f.tail_from));
                b.frames.resize(static_cast<std::size_t>(f.tail_from));
                a.overset = b.overset;
                return same_flow(a, b, w);
            };
            // Pages of text pasted in: no 2-frame re-flow can resettle after
            // that, whatever the fonts (a chapter's last page has < 1 page free).
            std::string paste;
            for (int k = 0; k < 40; ++k)
                paste += "\nPasted paragraph, long enough to take several lines in a column of this book, so forty of them run on for pages and pages.";
            core::doc::edit_text(j, d, "story.1", {5, 3}, {5, 3}, paste, "Paste", "");
            const auto rev1 = d.revision();
            const auto p1 = cache.get(d, "story.1");
            const auto whole1 = L::flow_story(d, "story.1");
            check("budget: the re-flow stops, provisional", p1.provisional && p1.tail_from > 0 && p1.filled <= 2 &&
                                                          cache.any_unfinished(), std::to_string(p1.filled) + " set");
            check("budget: the frames it set are the whole flow's", p1.provisional && head_same(p1, whole1));
            check("budget: the stand-in's lines are all real text", valid(p1));
            std::string why;
            {   // the worker's part, then handed in
                const auto u = cache.unfinished();
                const bool one = u.size() == 1 && u[0].story == "story.1";
                const auto full = one ? L::reflow_story(d, "story.1", u[0].basis_text, u[0].basis) : L::flow_story(d, "story.1");
                cache.complete(d, "story.1", rev1, full, *d.as<core::Story>("story.1"));
                const auto& got = cache.get(d, "story.1");
                check("complete: the whole taken, no longer provisional", one && !got.provisional && !cache.any_unfinished() &&
                                                                          same_flow(got, whole1, why), why);
            }
            // Typed on while the worker works: its answer becomes the basis.
            core::doc::edit_text(j, d, "story.1", {7, 2}, {7, 2}, paste, "Paste", "");
            const auto rev2 = d.revision();
            (void)cache.get(d, "story.1");
            const auto u2 = cache.unfinished();
            const auto text2 = *d.as<core::Story>("story.1");
            const auto full2 = u2.empty() ? L::StoryFlow{} : L::reflow_story(d, "story.1", u2[0].basis_text, u2[0].basis);
            core::doc::edit_text(j, d, "story.1", {9, 2}, {9, 2}, "and more \n", "Typing", "");
            const auto& p3 = cache.get(d, "story.1");
            const bool p3ok = valid(p3) && (!p3.provisional || head_same(p3, L::flow_story(d, "story.1")));
            if (!u2.empty()) cache.complete(d, "story.1", rev2, full2, text2);   // late: the document moved on
            const auto& p4 = cache.get(d, "story.1");
            check("typed on meanwhile: re-flowed from the late answer", !u2.empty() && p3ok && valid(p4) &&
                                                                     cache.last_how() == L::FlowCache::How::Reflowed);
            cache.finish_now(d);
            check("finish_now: the whole, here and now", same_flow(cache.get(d, "story.1"), L::flow_story(d, "story.1"), why) &&
                                                         !cache.any_unfinished(), why);
            // Undo through it all, unfinished or not, ends at the whole.
            j.undo(); j.undo(); j.undo(); j.undo();
            (void)cache.get(d, "story.1");
            cache.finish_now(d);
            check("undo: the whole again", same_flow(cache.get(d, "story.1"), L::flow_story(d, "story.1"), why), why);
        }
        {   // The sample: keeps, drop caps, wraps, columns, balance, breaks.
            core::Doc d;
            core::DocJournal j{d};
            d.set_door([&j] { return j.open(); });
            core::load_sample(d);
            L::FlowCache cache;
            int bad = 0;
            std::string why, first;
            for (const auto& sid : d.ids_of<core::Story>()) {
                (void)cache.get(d, sid);
                const auto* st = d.as<core::Story>(sid);
                for (int p = 0; p < static_cast<int>(st->paras.size()); ++p) {
                    for (const char* ins : {"Then ", "\n", "some words to push the paragraph a line further on, it may be, here "}) {
                        core::doc::edit_text(j, d, sid, {p, 0}, {p, 0}, ins, "Typing", "");
                        if (!same_flow(cache.get(d, sid), L::flow_story(d, sid), why) && bad++ == 0) first = sid + " para " + std::to_string(p) + ": " + why;
                        j.undo();
                        if (!same_flow(cache.get(d, sid), L::flow_story(d, sid), why) && bad++ == 0) first = sid + " para " + std::to_string(p) + " (undone): " + why;
                    }
                }
            }
            check("the sample: every paragraph of every story edited -- re-flow == whole", bad == 0, first);
        }
    }

    {
        std::cout << "\n-- footnotes: the model (s109) --\n";
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto before = d.records();
        const auto p1 = d.as<core::Story>("story.1")->paras[1].text;
        const auto after = core::doc::insert_note(j, d, "story.1", {1, 4}, "A first note.");
        const auto* st = d.as<core::Story>("story.1");
        const auto& para = st->paras[1];
        check("insert: the mark in the text, its note with it",
              core::note_at(para.text, 4) && para.notes.size() == 1 && para.notes[0].text == "A first note." &&
              after.para == 1 && after.byte == 7 && para.text.size() == p1.size() + 3);
        check("insert: one step", j.can_undo() && d.strays() == 0);
        j.undo();
        check("insert: undone exactly", d.records() == before);
        j.redo();
        core::doc::insert_note(j, d, "story.1", {3, 0}, "A later note.");
        core::doc::insert_note(j, d, "story.1", {1, 0}, "An earlier one.");
        st = d.as<core::Story>("story.1");
        check("numbered through the story, in order",
              core::note_number(*st, 1, 0) == 1 && core::note_number(*st, 1, 1) == 2 && core::note_number(*st, 3, 0) == 3 &&
              st->paras[1].notes[0].text == "An earlier one." && st->paras[1].notes[1].text == "A first note.");
        check("set_note_text: changes the words",
              core::doc::set_note_text(j, d, "story.1", 1, 1, "Changed.") && d.as<core::Story>("story.1")->paras[1].notes[1].text == "Changed.");
        // Enter before the second mark: that note goes with its mark to the new paragraph.
        core::doc::edit_text(j, d, "story.1", {1, 5}, {1, 5}, "\n", "Typing", "");
        st = d.as<core::Story>("story.1");
        check("a paragraph split: each note goes with its mark",
              st->paras[1].notes.size() == 1 && st->paras[1].notes[0].text == "An earlier one." &&
              st->paras[2].notes.size() == 1 && st->paras[2].notes[0].text == "Changed.");
        // Deleting the mark deletes its note; the others keep theirs.
        const int at = static_cast<int>(st->paras[2].text.find(core::kMarkNote));
        core::doc::edit_text(j, d, "story.1", {2, at}, {2, at + 3}, "", "Delete", "");
        st = d.as<core::Story>("story.1");
        check("a mark deleted: its note goes", st->paras[2].notes.empty() && st->paras[1].notes.size() == 1 &&
                                                core::note_number(*st, 4, 0) == 2);
        // A mark pasted in: an empty note.
        core::doc::edit_text(j, d, "story.1", {0, 0}, {0, 0}, std::string("x") + core::kMarkNote, "Paste", "");
        st = d.as<core::Story>("story.1");
        check("a pasted mark: an empty note, numbered first", st->paras[0].notes.size() == 1 && st->paras[0].notes[0].text.empty() &&
                                                              core::note_number(*st, 1, 0) == 2);
        // Saved and read back.
        core::Doc e;
        std::string err;
        check("json: notes round-trip", core::from_json(e, core::to_json(d), &err) && e.records() == d.records(), err);
        // Every step undone: the document as it was.
        while (j.can_undo()) j.undo();
        check("undo all: as it was", d.records() == before && d.strays() == 0);
        core::Para lone{"", std::string("a") + core::kMarkNote + "b" + core::kMarkNote, {}};
        core::fit_notes(lone);
        check("fit_notes: a note for each mark", lone.notes.size() == 2);
    }

    {
        std::cout << "\n-- footnotes: set at the column's foot (s109) --\n";
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        core::doc::insert_note(j, d, "story.1", {1, 4}, "The harbour master's log for that year is lost.");
        core::doc::insert_note(j, d, "story.1", {2, 6}, "See the chart of 1871, in the second drawer.");
        L::FlowCache flows;
        const auto& f = flows.get(d, "story.1");
        int placed = 0;
        bool clear = true, inside = true, numbered = true;
        std::string why;
        for (const auto& ft : f.frames) {
            const auto* fr = d.as<core::Frame>(ft.frame);
            for (const auto& np : ft.notes) {
                ++placed;
                numbered = numbered && np.number == placed;
                inside = inside && np.y >= 0 && np.y + np.h <= fr->h - std::max(0.0, fr->inset.bottom) + 0.01 && np.h > 0;
                // No line of the column runs into the notes (or their rule).
                for (const auto& l : ft.lines)
                    if (l.col == np.col && l.y + l.h > np.y + 0.01) { clear = false; why = ft.frame; }
            }
        }
        check("two notes placed (and the sample's), numbered 1, 2, 3", placed == 3 && numbered, std::to_string(placed));
        check("each inside its frame, at the foot", inside);
        check("no line runs into the notes", clear, why);
        check("a note's reference is in its column",
              [&] {
                  for (const auto& ft : f.frames)
                      for (const auto& np : ft.notes) {
                          bool found = false;
                          for (const auto& l : ft.lines) {
                              if (l.para != np.para || l.col != np.col) continue;
                              const auto& t = d.as<core::Story>("story.1")->paras[static_cast<std::size_t>(l.para)].text;
                              for (std::size_t i = t.find(core::kMarkNote); i != std::string::npos; i = t.find(core::kMarkNote, i + 3))
                                  if (core::notes_in(t, 0, i) == np.k && static_cast<int>(i) >= l.start && static_cast<int>(i) < l.end) found = true;
                          }
                          if (!found) return false;
                      }
                  return true;
              }());
        // A note too long for the room left: its line goes on (to where it fits), never under the notes.
        std::string longer;
        for (int k = 0; k < 30; ++k) longer += "A long note, set in the smaller size, running on for many lines. ";
        core::doc::set_note_text(j, d, "story.1", 2, 0, longer);
        const auto& g = flows.get(d, "story.1");
        bool ok = true;
        for (const auto& ft : g.frames)
            for (const auto& np : ft.notes)
                for (const auto& l : ft.lines)
                    if (l.col == np.col && l.y + l.h > np.y + 0.01) {
                        // allowed only when the line is the column's first (it always goes)
                        bool first = true;
                        for (const auto& m : ft.lines) if (m.col == l.col && m.y < l.y) first = false;
                        ok = ok && first;
                    }
        check("a long note: the lines stay above it", ok);
        {   // The page's key moves with its note's words (its picture is drawn again).
            std::string page;
            for (const auto& ft : g.frames) if (!ft.notes.empty()) { page = d.as<core::Frame>(ft.frame)->owner; break; }
            const auto k0 = L::page_key(d, flows, page);
            core::doc::set_note_text(j, d, "story.1", 1, 0, "Reworded.");
            check("page key: a note's words change it", !page.empty() && !(L::page_key(d, flows, page) == k0));
        }
        // Re-flowed after an edit near a note: the whole flow, notes and all.
        L::FlowCache inc;
        (void)inc.get(d, "story.1");
        core::doc::edit_text(j, d, "story.1", {1, 0}, {1, 0}, "Then, ", "Typing", "");
        const auto& r = inc.get(d, "story.1");
        const auto w = L::flow_story(d, "story.1");
        bool same = r.frames.size() == w.frames.size();
        for (std::size_t i = 0; same && i < r.frames.size(); ++i) {
            same = r.frames[i].notes.size() == w.frames[i].notes.size() && r.frames[i].lines.size() == w.frames[i].lines.size();
            for (std::size_t k = 0; same && k < r.frames[i].notes.size(); ++k)
                same = r.frames[i].notes[k].para == w.frames[i].notes[k].para && r.frames[i].notes[k].number == w.frames[i].notes[k].number &&
                       std::abs(r.frames[i].notes[k].y - w.frames[i].notes[k].y) < 1e-9;
        }
        check("re-flow with notes == whole flow", same);
        // Drawn and exported: the notes' lines are visited; both PDFs write.
        int note_lines = 0, refs = 0;
        for (const auto& ft : w.frames)
            L::visit_lines(d, "story.1", w, ft.frame, [&](const L::LineVisit& v) {
                note_lines += v.from == -4;
                refs += v.from == -3;
            });
        check("visited: the numbers at their marks, the notes' lines", refs == 3 && note_lines >= 4,
              std::to_string(refs) + " / " + std::to_string(note_lines));
        const auto dir = std::filesystem::temp_directory_path();
        for (const bool print : {false, true}) {
            std::string err;
            const auto path = (dir / (print ? "cx_notes_print.pdf" : "cx_notes_screen.pdf")).string();
            const bool done = print ? L::export_print_pdf(d, path, {}, {}, nullptr, &err) : L::export_pdf(d, path, {}, {}, nullptr, &err);
            check(std::string(print ? "print" : "screen") + " PDF with footnotes: written",
                  done && std::filesystem::file_size(path) > 1000, err);
            std::filesystem::remove(path);
        }
    }

    {
        std::cout << "\n-- footnote numbering (s111) --\n";
        namespace L = composix::layout;
        using NS = core::NoteStyle;
        check("labels: 1, iv, c, *, †, **", core::note_label(1, NS::Arabic) == "1" && core::note_label(4, NS::RomanLower) == "iv" &&
                                                 core::note_label(3, NS::AlphaLower) == "c" && core::note_label(1, NS::Symbols) == "*" &&
                                                 core::note_label(2, NS::Symbols) == "†" && core::note_label(7, NS::Symbols) == "**");
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        (void)L::load_big_book(d, {.pages = 24});
        // Notes in several paragraphs, two in one, far apart.
        for (const int p : {40, 12, 12, 5})
            core::doc::insert_note(j, d, "story.1", {p, 0}, "A note in paragraph " + std::to_string(p) + ".");
        const auto per_page = [&](const L::StoryFlow& f) {   // page -> its notes' numbers in order
            std::map<std::string, std::vector<int>> m;
            for (const auto& ft : f.frames)
                for (const auto& np : ft.notes) m[d.as<core::Frame>(ft.frame)->owner].push_back(np.number);
            return m;
        };
        L::FlowCache flows;
        auto whole = per_page(flows.get(d, "story.1"));
        std::vector<int> all;
        for (const auto& pg : d.pages()) for (int n : whole[pg]) all.push_back(n);
        std::size_t pages_with = 0;
        for (const auto& [pg, ns] : whole) pages_with += !ns.empty();
        check("through the story: 1, 2, 3, 4 in page order, on several pages", all == std::vector<int>{1, 2, 3, 4} && pages_with >= 2,
              std::to_string(pages_with) + " pages");
        check("numbering: one step", core::doc::set_note_numbering(j, d, core::NoteRestart::Page, NS::Arabic) &&
                                     !core::doc::set_note_numbering(j, d, core::NoteRestart::Page, NS::Arabic));
        const auto& fp = flows.get(d, "story.1");
        bool restarts = true;
        int ones = 0;
        for (const auto& [pg, ns] : per_page(fp))
            for (std::size_t i = 0; i < ns.size(); ++i) {
                restarts = restarts && ns[i] == static_cast<int>(i) + 1;
                ones += ns[i] == 1;
            }
        check("each page: its notes 1, 2, ...", restarts && ones >= 2);
        // Re-flowed under page numbering: still the whole flow.
        L::FlowCache inc;
        (void)inc.get(d, "story.1");
        bool same = true;
        for (const auto& [p, b] : std::vector<std::pair<int, int>>{{3, 0}, {12, 5}, {20, 0}}) {
            core::doc::edit_text(j, d, "story.1", {p, b}, {p, b}, "some words here to move a line on ", "Typing", "");
            const auto a = per_page(inc.get(d, "story.1"));
            same = same && a == per_page(L::flow_story(d, "story.1"));
        }
        check("re-flowed by page: the same numbers as a whole flow", same);
        {   // By section: numbers count on across pages, starting again in each section.
            core::doc::set_note_numbering(j, d, core::NoteRestart::Section, NS::Arabic);
            const auto& fsec = flows.get(d, "story.1");
            std::map<std::string, std::vector<int>> by;
            for (const auto& pg : d.pages())
                for (const auto& ft : fsec.frames)
                    if (d.as<core::Frame>(ft.frame)->owner == pg)
                        for (const auto& np : ft.notes) by[d.section_of(pg)].push_back(np.number);
            bool ok = !by.empty();
            for (const auto& [sec, ns] : by)
                for (std::size_t i = 0; i < ns.size(); ++i) ok = ok && ns[i] == static_cast<int>(i) + 1;
            check("each section: its notes 1, 2, ... across its pages", ok);
        }
        // The look: symbols in the notes and at the marks.
        core::doc::set_note_numbering(j, d, core::NoteRestart::Page, NS::Symbols);
        const auto& fs = flows.get(d, "story.1");
        int stars = 0;
        for (const auto& ft : fs.frames)
            if (!ft.notes.empty())
                L::visit_lines(d, "story.1", fs, ft.frame, [&](const L::LineVisit& v) {
                    if (v.from != -3 && v.from != -4) return;
                    const int n = pango_layout_line_get_length(v.line) > 0 ? 1 : 0;
                    stars += n;
                });
        check("symbols: the numbers are drawn", stars >= 8);
        core::Doc e;
        std::string err;
        check("json: numbering kept", core::from_json(e, core::to_json(d), &err) &&
                                      e.settings().note_restart == core::NoteRestart::Page && e.settings().note_style == NS::Symbols, err);
    }

    {
        std::cout << "\n-- find / change in footnotes (s113) --\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto before = d.records();
        // "qqx" at the paragraph's start, in a note at byte 4+, and at its end.
        D::edit_text(j, d, "story.1", {1, 0}, {1, 0}, "qqx ", "Typing", "");
        D::insert_note(j, d, "story.1", {1, 8}, "Logged by the zebrafinch qqx master.");
        const int end1 = static_cast<int>(d.as<core::Story>("story.1")->paras[1].text.size());
        D::edit_text(j, d, "story.1", {1, end1}, {1, end1}, " qqx", "Typing", "");
        const auto* st = d.as<core::Story>("story.1");
        check("note_mark_byte: where the mark is", core::doc::note_mark_byte(*st, 1, 0) == 8 && core::doc::note_mark_byte(*st, 1, 1) < 0);
        const auto zh = D::find_all(d, {"zebrafinch"});
        const auto& nt = st->paras[1].notes[0].text;
        check("find: a word only in a note -- one hit, in it",
              zh.size() == 1 && zh[0].note == 0 && zh[0].a.para == 1 &&
                  nt.substr(static_cast<std::size_t>(zh[0].a.byte), static_cast<std::size_t>(zh[0].b.byte - zh[0].a.byte)) == "zebrafinch");
        const auto qh = D::find_all(d, {"qqx"});
        check("find: text, note, text -- in reading order at the mark",
              qh.size() == 3 && qh[0].note < 0 && qh[0].a.byte == 0 && qh[1].note == 0 && qh[2].note < 0 && qh[2].a.byte > 8);
        check("find: case and whole word reach notes", D::find_all(d, {"ZEBRAFINCH", true}).empty() &&
                                                       D::find_all(d, {"zebra", false, true}).empty() && D::find_all(d, {"ZEBRA"}).size() == 1);
        D::FindOpts rx{"zebra(\\w+)"};
        rx.mode = D::FindOpts::Mode::Regex;
        const auto rh = D::find_all(d, rx);
        check("find: a pattern in a note, its groups", rh.size() == 1 && rh[0].note == 0 && rh[0].groups.size() == 2 && rh[0].groups[1] == "finch");
        check("find: a paragraph style asked (not Footnote) -- notes not looked in",
              D::find_all(d, {"zebrafinch", false, false, "style.2"}).empty());
        D::FindOpts in{"zebrafinch"};
        in.story = "story.1";
        in.in_range = true;
        in.from = {1, 0};
        in.to = {1, 8};
        check("find: a range short of the mark -- not its note", D::find_all(d, in).empty());
        in.to = {1, 11};
        check("find: a range holding the mark -- its note", D::find_all(d, in).size() == 1);
        // Change one, in the note.
        const auto pre = d.records();
        check("change: in a note, one hit", D::change_hits(j, d, zh, {true, "heron"}, "Change") == 1 &&
                                            d.as<core::Story>("story.1")->paras[1].notes[0].text == "Logged by the heron qqx master.");
        j.undo();
        check("change: one step, undone exactly", d.records() == pre);
        // Change All across text and notes: one step; the note kept with its mark.
        check("change all: text and note, one step",
              D::change_hits(j, d, qh, {true, "yy"}, "Change All (3)") == 3 && D::find_all(d, {"qqx"}).empty() &&
                  D::find_all(d, {"yy", false, true}).size() == 3 && d.as<core::Story>("story.1")->paras[1].notes.size() == 1);
        j.undo();
        check("change all: undone in one", d.records() == pre);
        check("change: a line break in a note is a new paragraph (s116)",
              D::change_hits(j, d, zh, {true, "big\r\nheron"}, "Change") == 1 &&
                  d.as<core::Story>("story.1")->paras[1].notes[0].text == "Logged by the big\nheron qqx master.");
        j.undo();
        // A character style onto a note's words.
        D::ChangeOpts cs{false, "", "", "style.5"};
        st = d.as<core::Story>("story.1");
        const auto& note = st->paras[1].notes[0];
        check("change: a character style onto words in a note",
              D::change_hits(j, d, zh, cs, "Change") == 1 && d.as<core::Story>("story.1")->paras[1].notes[0].text == note.text &&
                  [&] {
                      const auto& r = d.as<core::Story>("story.1")->paras[1].notes[0].runs;
                      return r.size() == 1 && r[0].style == "style.5" && r[0].start == zh[0].a.byte && r[0].end == zh[0].b.byte;
                  }());
        check("find: in a character style -- in the note", D::find_all(d, {"zebrafinch", false, false, "", "style.5"}).size() == 1);
        j.undo();
        // s113 fix: a character style (or bold) over text with a note kept the note's words.
        const auto words = d.as<core::Story>("story.1")->paras[1].notes[0].text;
        D::set_char_style(j, d, "story.1", {1, 0}, {1, 12}, "style.5");
        check("fix: a character style over a footnote's mark keeps its words",
              d.as<core::Story>("story.1")->paras[1].notes.size() == 1 && d.as<core::Story>("story.1")->paras[1].notes[0].text == words);
        D::set_local(j, d, "story.1", {1, 0}, {1, 12}, "Bold", [](core::Local& l) { l.bold = 1; });
        check("fix: bold over a footnote's mark keeps its words", d.as<core::Story>("story.1")->paras[1].notes[0].text == words);
        while (j.can_undo()) j.undo();
        check("undo all: as it was", d.records() == before && d.strays() == 0);
    }

    {
        std::cout << "\n-- footnote defaults (s114) --\n";
        namespace L = composix::layout;
        core::Doc n;
        core::new_document(n, core::NewDoc{});
        bool has = false;
        for (const auto& id : n.ids_of<core::Style>()) {
            const auto* s = n.as<core::Style>(id);
            has = has || (s->name == "Footnote" && s->kind == core::StyleKind::Paragraph && s->based_on == "style.1");
        }
        check("a new document has a Footnote style (on Body)", has);
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto* st = d.as<core::Story>("story.1");
        check("the sample has a note (Chapter Two's map)", st->paras[5].notes.size() == 1 && !st->paras[5].notes[0].text.empty() &&
                                                         core::doc::note_mark_byte(*st, 5, 0) >= 0);
        L::FlowCache flows;
        const auto& f = flows.get(d, "story.1");
        const L::NotePlace* np = nullptr;
        std::string fr;
        for (const auto& ft : f.frames)
            for (const auto& x : ft.notes) { np = &x; fr = ft.frame; }
        check("the sample's note is set at a foot", np != nullptr);
        if (np) {
            const auto* hit = L::note_hit(f, fr, np->x + np->w / 2, np->y + np->h / 2);
            check("note_hit: on the note -- it", hit && hit->para == 5 && hit->k == 0);
            check("note_hit: above it -- none", L::note_hit(f, fr, np->x + np->w / 2, np->y - 2) == nullptr);
            // Set in the Footnote style: its size, not the paragraph's.
            int lines = 0;
            double h1 = 0, body = 0;
            for (const auto& ft : f.frames)
                L::visit_lines(d, "story.1", f, ft.frame, [&](const L::LineVisit& v) {
                    PangoRectangle r;
                    pango_layout_line_get_extents(v.line, nullptr, &r);
                    const double h = r.height / static_cast<double>(PANGO_SCALE);
                    if (v.from == -4) { ++lines; h1 = std::max(h1, h); }
                    else if (v.from >= 0 && v.para == 2) body = std::max(body, h);   // Body, 11 pt
                });
            check("the note's lines drawn, smaller than Body's", lines >= 1 && h1 > 0 && body > 0 && h1 < body * 0.9,
                  std::to_string(h1) + " / " + std::to_string(body));
        }
    }

    {
        std::cout << "\n-- the Footnotes section (s115) --\n";
        std::string st;
        int p = -1, k = -1;
        const auto id = core::note_item_id("story.1", 5, 0);
        check("note item id: made and read back", core::parse_note_item_id(id, st, p, k) && st == "story.1" && p == 5 && k == 0);
        check("note item id: not one -- refused", !core::parse_note_item_id("story.1", st, p, k) &&
                                                  !core::parse_note_item_id("note:story.1:x:0", st, p, k) &&
                                                  !core::parse_note_item_id("note::1:0", st, p, k) &&
                                                  !core::parse_note_item_id("note:story.1:1:2z", st, p, k));
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        auto o = core::project(d, "");
        auto notes = o.in(core::Segment::Notes);
        const auto words = [](const core::Item* it) {
            for (const auto& f : it->fields) if (f.edit == "note_words") return f.value;
            return std::string("?");
        };
        check("the sample's note listed, its words editable",
              notes.size() == 1 && notes[0]->id == id && words(notes[0]) == d.as<core::Story>("story.1")->paras[5].notes[0].text &&
                  notes[0]->thumb.sample == words(notes[0]));
        core::doc::insert_note(j, d, "story.1", {1, 4}, "");
        core::doc::insert_note(j, d, "story.2", {0, 0}, "Surveyed again in 1872.");
        o = core::project(d, "");
        notes = o.in(core::Segment::Notes);
        check("each note, in story order, numbered in its story",
              notes.size() == 3 && notes[0]->id == core::note_item_id("story.1", 1, 0) && notes[0]->title.rfind("Note 1", 0) == 0 &&
                  notes[1]->id == id && notes[1]->title.rfind("Note 2", 0) == 0 && notes[2]->title.rfind("Note 1", 0) == 0);
        check("an empty note is badged", notes[0]->badge == "Empty" && notes[1]->badge.empty());
        bool show = false;
        for (const auto& f : notes[1]->fields) show = show || (f.buttons && f.edit == "note" && !f.choices.empty());
        check("Show in the Text offered", show);
        check("ids unique across the outline", [&] {
            std::set<std::string> ids;
            for (const auto& it : o.items()) if (!ids.insert(it.id).second) return false;
            return true;
        }());
    }

    {
        std::cout << "\n-- footnotes in paragraphs (s116) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto note = [&] { return d.as<core::Story>("story.1")->paras[5].notes[0].text; };
        const auto foot = [&] {   // the sample note's place: its height, its lines drawn
            L::FlowCache flows;
            const auto& f = flows.get(d, "story.1");
            double h = 0;
            int lines = 0;
            for (const auto& ft : f.frames) {
                for (const auto& np : ft.notes) h += np.h;
                if (!ft.notes.empty())
                    L::visit_lines(d, "story.1", f, ft.frame, [&](const L::LineVisit& v) { lines += v.from == -4; });
            }
            return std::pair<double, int>{h, lines};
        };
        const auto one = foot();
        check("set_note_text: paragraphs kept, \\r\\n read as one break",
              D::set_note_text(j, d, "story.1", 5, 0, "First, the foldout.\r\nSecond, the key.\rThird, the scale.") &&
                  note() == "First, the foldout.\nSecond, the key.\nThird, the scale.");
        const auto three = foot();
        check("three paragraphs: taller at the foot, each its own line(s)", three.first > one.first + 1 && three.second >= 3,
              std::to_string(one.first) + " -> " + std::to_string(three.first) + ", " + std::to_string(three.second) + " lines");
        core::Doc e;
        std::string err;
        check("json: a note's paragraphs kept", core::from_json(e, core::to_json(d), &err) &&
                                                e.as<core::Story>("story.1")->paras[5].notes[0].text == note(), err);
        const auto hits = D::find_all(d, {"key.^pThird"});
        check("find ^p: across a note's paragraphs", hits.size() == 1 && hits[0].note == 0 && hits[0].a.para == 5);
        D::FindOpts rx{"\\n"};
        rx.mode = D::FindOpts::Mode::Regex;
        const auto in_note = [](std::vector<D::Hit> h) { std::erase_if(h, [](const D::Hit& x) { return x.note < 0; }); return h; };
        check("find \\n (regex): each break in the note", in_note(D::find_all(d, rx)).size() == 2);
        check("change ^p in a note: joined, one step",
              D::change_hits(j, d, in_note(D::find_all(d, {"^p"})), {true, " "}, "Change All") == 2 &&
                  note() == "First, the foldout. Second, the key. Third, the scale.");
        j.undo();
        check("undo: the paragraphs back", note() == "First, the foldout.\nSecond, the key.\nThird, the scale.");
        const auto o = core::project(d, "");
        const auto ns = o.in(core::Segment::Notes);
        bool ro = false;
        for (const auto& f : ns.front()->fields) ro = ro || (f.label.rfind("Words (", 0) == 0 && f.edit.empty());
        check("the Footnotes section: paragraphs shown, not typed in one line", ns.size() == 1 && ro);
    }

    {
        std::cout << "\n-- footnotes run on; the footnote area (s117) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        check("footnote area: one step, kept", D::set_note_area(j, d, 50) && !D::set_note_area(j, d, 50) &&
                                               d.settings().note_area == 50 && D::set_note_area(j, d, 100));
        {
            core::Doc e;
            std::string err;
            D::set_note_area(j, d, 33);
            check("json: the area kept", core::from_json(e, core::to_json(d), &err) && e.settings().note_area == 33, err);
            D::set_note_area(j, d, 100);
        }
        // A note far longer than a column, on Chapter One's first paragraph.
        std::string longw;
        for (int i = 1; i <= 60; ++i)
            longw += "Clause " + std::to_string(i) + " of the harbour ordinance, as amended, applies to every vessel entering the port.";
        longw += "\nA second paragraph closes the note.";
        D::insert_note(j, d, "story.1", {1, 4}, longw);
        const auto* st = d.as<core::Story>("story.1");
        // Every piece of the note, in thread order, and the checks on each frame.
        struct Piece { std::size_t frame; int from, to; bool cont; double y, h; };
        const auto pieces = [&](const L::StoryFlow& f, bool* inside, bool* clear, double* most) {
            std::vector<Piece> out;
            *inside = *clear = true;
            *most = 0;
            for (std::size_t i = 0; i < f.frames.size(); ++i) {
                const auto& ft = f.frames[i];
                const auto* fr = d.as<core::Frame>(ft.frame);
                std::map<int, double> per_col;
                for (const auto& np : ft.notes) {
                    if (np.para == 1 && np.k == 0) out.push_back({i, np.from, np.to, np.cont, np.y, np.h});
                    *inside = *inside && np.y >= -0.01 && np.y + np.h <= fr->h - std::max(0.0, fr->inset.bottom) + 0.01;
                    per_col[np.col] += np.h;
                    for (const auto& l : ft.lines)
                        if (l.col == np.col && l.y + l.h > np.y + 0.01) *clear = false;
                }
                const double colh = fr->h - std::max(0.0, fr->inset.bottom) - std::max(0.0, fr->inset.top);
                for (const auto& [c, h] : per_col) *most = std::max(*most, h / colh);
            }
            return out;
        };
        const auto joined = [&](const std::vector<Piece>& ps, bool overset = false) {   // the pieces cover the words, in order, once
            if (ps.empty() || ps.front().from != 0 || ps.front().cont || (ps.back().to != -1 && !overset)) return false;
            for (std::size_t i = 1; i < ps.size(); ++i)
                if (!ps[i].cont || ps[i].from != ps[i - 1].to || ps[i].frame <= ps[i - 1].frame) return false;
            return true;
        };
        bool inside = false, clear = false;
        double most = 0;
        L::FlowCache flows;
        const auto& whole = flows.get(d, "story.1");
        auto ps = pieces(whole, &inside, &clear, &most);
        check("a note longer than a column: cut, run on", ps.size() >= 2 && joined(ps), std::to_string(ps.size()) + " pieces");
        check("each piece inside its frame; no line runs into the notes", inside && clear);
        const auto n100 = ps.size();
        // Its mark's line stays where it was: the note is cut instead (not the line sent on).
        bool mark_on_first = false;
        for (const auto& l : whole.frames[ps.front().frame].lines)
            mark_on_first = mark_on_first || (l.para == 1 && l.start <= 4 && l.end > 4);
        check("the mark's line keeps its frame, with the note's first piece", mark_on_first);
        // The rule over a run-on piece spans the column.
        bool wide = false;
        for (const auto& r : L::paragraph_rules(d, "story.1", whole, whole.frames[ps[1].frame].frame))
            for (const auto& np : whole.frames[ps[1].frame].notes)
                if (np.cont && std::abs(r.r.w - np.w) < 0.01) wide = true;
        check("a run-on piece: the rule across the column", wide);
        int cont_lines = 0;
        L::visit_lines(d, "story.1", whole, whole.frames[ps[1].frame].frame, [&](const L::LineVisit& v) { cont_lines += v.from == -4; });
        check("a run-on piece is drawn", cont_lines >= 1);
        {   // Cut with two lines of the note or more on each side.
            bool two = true;
            for (const auto& ft : whole.frames)
                for (const auto& np : ft.notes)
                    if (np.para == 1 && np.k == 0) {
                        int lines = 0;
                        L::visit_lines(d, "story.1", whole, ft.frame, [&](const L::LineVisit& v) { lines += v.from == -4; });
                        two = two && lines >= 2;
                    }
            check("each piece two lines or more", two);
        }
        // The area at 50%: no column's notes past half of it; more pieces.
        D::set_note_area(j, d, 50);
        const auto& half = flows.get(d, "story.1");
        ps = pieces(half, &inside, &clear, &most);
        check("area 50%: the notes keep to half the column", most <= 0.5 + 0.01, std::to_string(most));
        check("area 50%: the note runs on further (past the last frame: overset)", joined(ps, half.overset) && ps.size() > n100, std::to_string(n100) + " -> " + std::to_string(ps.size()));
        check("area 50%: inside, clear", inside && clear);
        check("a frame the text passes (a chapter starts right) still takes the note running on", [&] {
            for (const auto& ft : half.frames)
                if (ft.lines.empty() && !ft.notes.empty() && ft.notes.front().cont) return true;
            return false;
        }());
        // Numbered by page: a run-on piece takes no number -- the next page's own note is 1.
        D::set_note_numbering(j, d, core::NoteRestart::Page, core::NoteStyle::Arabic);
        {
            const auto& fp = flows.get(d, "story.1");
            const auto pp = pieces(fp, &inside, &clear, &most);
            bool ok = !pp.empty();
            for (const auto& ft : fp.frames) {
                int own = 0;
                for (const auto& np : ft.notes)
                    if (!np.cont) ok = ok && np.number == ++own;
            }
            check("by page: run-on pieces take no number", ok);
        }
        D::set_note_numbering(j, d, core::NoteRestart::Story, core::NoteStyle::Arabic);
        // A re-flow after an edit past the note: the same as a whole flow.
        {
            const core::Story before = *d.as<core::Story>("story.1");
            const auto old = L::flow_story(d, "story.1");
            D::edit_text(j, d, "story.1", {3, 0}, {3, 0}, "Late that night ", "Typing", "");
            const auto re = L::reflow_story(d, "story.1", before, old);
            const auto wf = L::flow_story(d, "story.1");
            bool same = re.frames.size() == wf.frames.size() && re.overset == wf.overset;
            for (std::size_t i = 0; same && i < re.frames.size(); ++i) {
                const auto& a = re.frames[i].notes;
                const auto& b = wf.frames[i].notes;
                same = a.size() == b.size() && re.frames[i].lines.size() == wf.frames[i].lines.size() && re.frames[i].carry_in == wf.frames[i].carry_in;
                for (std::size_t k = 0; same && k < a.size(); ++k)
                    same = a[k].para == b[k].para && a[k].k == b[k].k && a[k].from == b[k].from && a[k].to == b[k].to && a[k].cont == b[k].cont &&
                           std::abs(a[k].y - b[k].y) < 1e-9;
            }
            check("re-flow with a note running on == whole flow", same);
            // And an edit before the note (its line moves): still the same.
            const core::Story before2 = *d.as<core::Story>("story.1");
            const auto old2 = L::flow_story(d, "story.1");
            D::edit_text(j, d, "story.1", {1, 0}, {1, 0}, "At first light ", "Typing", "");
            const auto re2 = L::reflow_story(d, "story.1", before2, old2);
            const auto wf2 = L::flow_story(d, "story.1");
            bool same2 = re2.frames.size() == wf2.frames.size();
            for (std::size_t i = 0; same2 && i < re2.frames.size(); ++i)
                same2 = re2.frames[i].notes.size() == wf2.frames[i].notes.size() && re2.frames[i].carry_in == wf2.frames[i].carry_in &&
                        re2.frames[i].lines.size() == wf2.frames[i].lines.size();
            check("re-flow, an edit before the note == whole flow", same2);
        }
        while (j.can_undo()) j.undo();
        // The text ends, a note still runs on: into the next frame.
        {
            core::Doc e;
            core::DocJournal je{e};
            e.set_door([&je] { return je.open(); });
            core::load_sample(e);
            // The Sources story is short: a long note on its last paragraph.
            const auto& src = *e.as<core::Story>("story.4");
            const int last = static_cast<int>(src.paras.size()) - 1;
            D::insert_note(je, e, "story.4", {last, static_cast<int>(src.paras[static_cast<std::size_t>(last)].text.size())}, longw);
            const auto f = L::flow_story(e, "story.4");
            int with_notes = 0;
            for (const auto& ft : f.frames) with_notes += ft.notes.empty() ? 0 : 1;
            check("the text ended, the note ran on (or is overset): never lost",
                  f.overset || with_notes >= 2 || f.frames.size() == 1, std::to_string(with_notes) + " frames with notes, overset " + std::to_string(f.overset));
        }
    }

    {
        std::cout << "\n-- the foot's look (s118) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        check("footnote look: one step, the same again none",
              D::set_note_look(j, d, 50, 1, 3, "(continued)") && !D::set_note_look(j, d, 50, 1, 3, "(continued)") && j.size() >= 1);
        {
            core::Doc e;
            std::string err;
            check("json: the look kept", core::from_json(e, core::to_json(d), &err) && e.settings().note_rule == 50 &&
                                         e.settings().note_rule_w == 1 && e.settings().note_space == 3 && e.settings().note_cont == "(continued)", err);
        }
        // Two short notes in Chapter One's first paragraph, a long one in its second.
        D::insert_note(j, d, "story.1", {1, 4}, "The first note.");
        D::insert_note(j, d, "story.1", {1, 20}, "The second note.");
        std::string longw;
        for (int i = 1; i <= 40; ++i) longw += "Clause " + std::to_string(i) + " of the harbour ordinance, as amended, applies to every vessel. ";
        D::insert_note(j, d, "story.1", {2, 6}, longw);
        D::set_note_area(j, d, 50);
        const auto f = L::flow_story(d, "story.1");
        const L::FrameText* ft = nullptr;
        for (const auto& x : f.frames) if (x.notes.size() >= 2 && !x.notes.front().cont) { ft = &x; break; }
        check("two notes at a foot", ft != nullptr);
        if (ft) {
            const auto& a = ft->notes[0];
            const auto& b = ft->notes[1];
            check("3 pt between notes", std::abs(b.y - (a.y + a.h) - 3) < 0.01, std::to_string(b.y - (a.y + a.h)));
            bool rule = false;
            for (const auto& r : L::paragraph_rules(d, "story.1", f, ft->frame))
                if (r.r.y < a.y && r.r.y > a.y - 20 && std::abs(r.r.w - a.w * 0.5) < 0.01 && std::abs(r.r.h - 1) < 0.01) rule = true;
            check("the rule: half the column, 1 pt", rule);
            D::set_note_look(j, d, 0, 1, 3, "(continued)");
            const auto f0 = L::flow_story(d, "story.1");
            bool none = true;
            for (const auto& x : f0.frames)
                if (x.frame == ft->frame)
                    for (const auto& r : L::paragraph_rules(d, "story.1", f0, x.frame))
                        if (r.r.y < x.notes.front().y && r.r.y > x.notes.front().y - 20) none = false;
            check("rule None: no rule over the notes", none);
            D::set_note_look(j, d, 50, 1, 3, "(continued)");
        }
        // The continued line: the last line of a cut piece, and nothing overlaps.
        bool cut_seen = false, cont_line = false, inside = true, clear = true;
        for (const auto& x : f.frames) {
            const auto* fr = d.as<core::Frame>(x.frame);
            for (const auto& np : x.notes) {
                inside = inside && np.y >= -0.01 && np.y + np.h <= fr->h - std::max(0.0, fr->inset.bottom) + 0.01;
                for (const auto& l : x.lines) if (l.col == np.col && l.y + l.h > np.y + 0.01) clear = false;
                if (np.to >= 0) cut_seen = true;
            }
            std::string last;
            L::visit_lines(d, "story.1", f, x.frame, [&](const L::LineVisit& v) {
                if (v.from != -4) return;
                const char* t = pango_layout_get_text(v.line->layout);
                last = std::string(t + v.line->start_index, static_cast<std::size_t>(v.line->length));
                if (last == "(continued)") cont_line = true;
            });
        }
        check("a cut note ends in the continued line", cut_seen && cont_line);
        check("with the look: inside the frames, no line into the notes", inside && clear);
        D::set_note_look(j, d, 33, 0.5, 0, "");
        const auto g = L::flow_story(d, "story.1");
        bool any = false;
        for (const auto& x : g.frames)
            L::visit_lines(d, "story.1", g, x.frame, [&](const L::LineVisit& v) {
                if (v.from != -4) return;
                const char* t = pango_layout_get_text(v.line->layout);
                if (std::string(t + v.line->start_index, static_cast<std::size_t>(v.line->length)) == "(continued)") any = true;
            });
        check("no continued line set: none drawn", !any);
    }

    {
        std::cout << "\n-- the Story Editor's numbers (s119) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        D::insert_note(j, d, "story.1", {1, 4}, "On page 1.");   // the sample's own is on Chapter Two's page
        L::FlowCache flows;
        check("through the story: 1 and 2", L::laid_note_number(flows.get(d, "story.1"), 1, 0) == 1 &&
                                            L::laid_note_number(flows.get(d, "story.1"), 5, 0) == 2);
        D::set_note_numbering(j, d, core::NoteRestart::Page, core::NoteStyle::Arabic);
        check("by page: each the first on its page -- 1 and 1", L::laid_note_number(flows.get(d, "story.1"), 1, 0) == 1 &&
                                                               L::laid_note_number(flows.get(d, "story.1"), 5, 0) == 1);
        check("a note not there: 0", L::laid_note_number(flows.get(d, "story.1"), 1, 3) == 0);
        {
            const auto o = core::project(d, "");
            const auto* it = o.find("story.1");
            check("the story's tile: no footnote mark in its words", it && it->thumb.sample.find(core::kMarkNote) == std::string::npos &&
                                                                     it->thumb.sample.find("The harbor") != std::string::npos);
        }
        // A note running on: its run-on piece isn't another number.
        std::string longw;
        for (int i = 1; i <= 40; ++i) longw += "Clause " + std::to_string(i) + " applies to every vessel entering the port. ";
        D::set_note_text(j, d, "story.1", 1, 0, longw);
        D::set_note_area(j, d, 25);
        int pieces = 0;
        for (const auto& ft : flows.get(d, "story.1").frames)
            for (const auto& n : ft.notes) pieces += n.para == 1 && n.k == 0;
        check("running on: the number of its first piece", pieces >= 2 && L::laid_note_number(flows.get(d, "story.1"), 1, 0) == 1);
    }

    {
        std::cout << "\n-- endnotes (s120) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        D::insert_note(j, d, "story.1", {1, 4}, "The first, on page 1.");   // the sample's own (Chapter Two) is second
        check("notes at: one step, kept", D::set_note_place(j, d, core::NoteAt::StoryEnd, "Notes") &&
                                          !D::set_note_place(j, d, core::NoteAt::StoryEnd, "Notes"));
        {
            core::Doc e;
            std::string err;
            D::set_note_place(j, d, core::NoteAt::StoryEnd, "Endnotes");
            check("json: endnotes and their title kept", core::from_json(e, core::to_json(d), &err) &&
                                                         e.settings().note_at == core::NoteAt::StoryEnd && e.settings().note_title == "Endnotes", err);
            D::set_note_place(j, d, core::NoteAt::StoryEnd, "Notes");
        }
        D::set_note_numbering(j, d, core::NoteRestart::Page, core::NoteStyle::Arabic);   // endnotes count through the story anyway
        const auto check_flow = [&](const L::StoryFlow& f, std::vector<const L::NotePlace*>* all, bool* inside, bool* after, bool* clear) {
            *inside = *after = *clear = true;
            for (const auto& ft : f.frames) {
                const auto* fr = d.as<core::Frame>(ft.frame);
                double last = 0;
                for (const auto& l : ft.lines) last = std::max(last, l.y + l.h);
                for (const auto& np : ft.notes) {
                    all->push_back(&np);
                    *inside = *inside && np.y >= -0.01 && np.y + np.h <= fr->h - std::max(0.0, fr->inset.bottom) + 0.01;
                    *after = *after && np.y >= last - 0.01;
                    for (const auto& l : ft.lines) if (l.col == np.col && l.y + l.h > np.y + 0.01 && l.y < np.y + np.h - 0.01) *clear = false;
                }
            }
        };
        L::FlowCache flows;
        std::vector<const L::NotePlace*> all;
        bool inside = false, after = false, clear = false;
        check_flow(flows.get(d, "story.1"), &all, &inside, &after, &clear);
        check("endnotes: the title, then each note in story order, numbered 1, 2",
              all.size() == 3 && all[0]->k == -1 && all[1]->para == 1 && all[1]->number == 1 && all[2]->para == 5 && all[2]->number == 2 &&
                  std::all_of(all.begin(), all.end(), [](const L::NotePlace* n) { return n->end; }));
        check("endnotes: after the text, inside the frame, clear of it", inside && after && clear);
        check("the marks: numbered through the story", L::laid_note_number(flows.get(d, "story.1"), 1, 0) == 1 &&
                                                       L::laid_note_number(flows.get(d, "story.1"), 5, 0) == 2);
        {
            int drawn = 0;
            const auto& f = flows.get(d, "story.1");
            for (const auto& ft : f.frames) L::visit_lines(d, "story.1", f, ft.frame, [&](const L::LineVisit& v) { drawn += v.from == -4; });
            check("endnotes drawn (title and notes)", drawn >= 3);
            bool foot_rule = false;
            for (const auto& ft : f.frames)
                for (const auto& r : L::paragraph_rules(d, "story.1", f, ft.frame))
                    for (const auto& np : ft.notes) if (r.r.y < np.y && r.r.y > np.y - 12 && r.r.h <= 0.6) foot_rule = true;
            check("no footnote rule over endnotes", !foot_rule);
        }
        D::set_note_place(j, d, core::NoteAt::StoryEnd, "");
        all.clear();
        check_flow(flows.get(d, "story.1"), &all, &inside, &after, &clear);
        check("no title set: none", all.size() == 2 && all[0]->k == 0);
        // A long endnote runs on: its pieces chained, in order (overset past the last frame).
        std::string longw;
        for (int i = 1; i <= 60; ++i) longw += "Clause " + std::to_string(i) + " of the harbour ordinance applies to every vessel. ";
        D::set_note_text(j, d, "story.1", 5, 0, longw);
        all.clear();
        const auto& lf = flows.get(d, "story.1");
        check_flow(lf, &all, &inside, &after, &clear);
        std::vector<const L::NotePlace*> pcs;
        for (auto* n : all) if (n->para == 5 && n->k == 0) pcs.push_back(n);
        bool chain = !pcs.empty() && pcs.front()->from == 0;
        for (std::size_t i = 1; i < pcs.size(); ++i) chain = chain && pcs[i]->from == pcs[i - 1]->to && pcs[i]->cont;
        check("a long endnote: cut where the last frame ends, the rest overset (chained)",
              !pcs.empty() && chain && pcs.back()->to > 0 && lf.overset, std::to_string(pcs.size()) + " pieces");
        check("long: inside, after the text, clear", inside && after && clear);
        {   // The last frame in two columns: the endnote runs on into the second.
            const auto last = L::thread_frames(d, "story.1").back();
            core::doc::edit_frames(j, d, {last}, "Columns", [](core::Frame& f) { f.columns = 2; });
            std::vector<const L::NotePlace*> two;
            bool in2 = false, af2 = false, cl2 = false;
            const auto& f2 = flows.get(d, "story.1");
            check_flow(f2, &two, &in2, &af2, &cl2);
            std::vector<const L::NotePlace*> p2;
            for (auto* n : two) if (n->para == 5 && n->k == 0) p2.push_back(n);
            bool ch = !p2.empty() && p2.front()->from == 0;
            for (std::size_t i = 1; i < p2.size(); ++i) ch = ch && p2[i]->from == p2[i - 1]->to && p2[i]->cont && p2[i]->col > p2[i - 1]->col;
            check("two columns: the endnote runs on into the next column", p2.size() >= 2 && ch && in2 && cl2, std::to_string(p2.size()));
            j.undo();
        }
        // Re-flow after an edit == whole.
        {
            const core::Story before = *d.as<core::Story>("story.1");
            const auto old = L::flow_story(d, "story.1");
            D::edit_text(j, d, "story.1", {3, 0}, {3, 0}, "Late that night ", "Typing", "");
            const auto re = L::reflow_story(d, "story.1", before, old);
            const auto wf = L::flow_story(d, "story.1");
            bool same = re.frames.size() == wf.frames.size() && re.overset == wf.overset;
            for (std::size_t i = 0; same && i < re.frames.size(); ++i) {
                same = re.frames[i].notes.size() == wf.frames[i].notes.size() && re.frames[i].lines.size() == wf.frames[i].lines.size() &&
                       re.frames[i].ending_in == wf.frames[i].ending_in;
                for (std::size_t k = 0; same && k < re.frames[i].notes.size(); ++k)
                    same = re.frames[i].notes[k].from == wf.frames[i].notes[k].from && std::abs(re.frames[i].notes[k].y - wf.frames[i].notes[k].y) < 1e-9;
            }
            check("re-flow with endnotes == whole flow", same);
        }
        // Back to the foot: no endnotes left behind.
        D::set_note_place(j, d, core::NoteAt::Foot, "Notes");
        bool any_end = false;
        for (const auto& ft : flows.get(d, "story.1").frames) for (const auto& np : ft.notes) any_end = any_end || np.end;
        check("back to the foot: none set as endnotes", !any_end);
        // A story with no notes: no title.
        D::set_note_place(j, d, core::NoteAt::StoryEnd, "Notes");
        bool none = true;
        for (const auto& ft : flows.get(d, "story.2").frames) none = none && ft.notes.empty();
        check("no notes: no endnotes title", none);
    }

    {
        std::cout << "\n-- chapter endnotes (s122) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        D::insert_note(j, d, "story.1", {1, 4}, "Chapter One's note.");   // the sample's own is in Chapter Two
        check("notes at: End of each chapter, one step", D::set_note_place(j, d, core::NoteAt::ChapterEnd, "Notes"));
        {
            core::Doc e;
            std::string err;
            check("json: chapters kept", core::from_json(e, core::to_json(d), &err) && e.settings().note_at == core::NoteAt::ChapterEnd, err);
        }
        // Where each piece is: the frame index; and where each paragraph's lines are.
        struct At { std::size_t fi; const L::NotePlace* np; };
        const auto places = [&](const L::StoryFlow& f) {
            std::vector<At> out;
            for (std::size_t i = 0; i < f.frames.size(); ++i)
                for (const auto& np : f.frames[i].notes) out.push_back({i, &np});
            return out;
        };
        const auto frame_of_para = [&](const L::StoryFlow& f, int p) {
            for (std::size_t i = 0; i < f.frames.size(); ++i)
                for (const auto& l : f.frames[i].lines) if (l.para == p) return static_cast<int>(i);
            return -1;
        };
        L::FlowCache flows;
        {
            const auto& f = flows.get(d, "story.1");
            const auto ps = places(f);
            const int ch2 = frame_of_para(f, 4), ch1end = frame_of_para(f, 3);
            bool ok = ps.size() == 4 && ps[0].np->k == -1 && ps[1].np->para == 1 && ps[1].np->number == 1 && ps[2].np->k == -1 &&
                      ps[3].np->para == 5 && ps[3].np->number == 2;
            check("each chapter: its title and notes; numbered through the story", ok, std::to_string(ps.size()));
            if (ok) {
                check("Chapter One's notes before Chapter Two, after its text",
                      static_cast<int>(ps[1].fi) >= ch1end && static_cast<int>(ps[1].fi) < ch2);
                check("Chapter Two's at the story's end", static_cast<int>(ps[3].fi) >= frame_of_para(f, 5));
                bool clear = true;
                for (const auto& a : ps)
                    for (const auto& l : f.frames[a.fi].lines)
                        if (l.col == a.np->col && l.y + l.h > a.np->y + 0.01 && l.y < a.np->y + a.np->h - 0.01) clear = false;
                check("chapter endnotes clear of the text", clear);
            }
        }
        // A long note in Chapter One: runs on into the page the chapter break passes (page 2), Chapter Two still on its right page.
        std::string longw;
        for (int i = 1; i <= 45; ++i) longw += "Clause " + std::to_string(i) + " of the harbour ordinance applies to every vessel. ";
        D::set_note_text(j, d, "story.1", 1, 0, longw);
        {
            const auto& f = flows.get(d, "story.1");
            std::vector<std::size_t> fis;
            for (const auto& a : places(f)) if (a.np->para == 1 && a.np->k == 0) fis.push_back(a.fi);
            const int ch2 = frame_of_para(f, 4);
            bool before = !fis.empty();
            for (auto fi : fis) before = before && static_cast<int>(fi) < ch2;
            const auto* p4 = ch2 >= 0 ? d.as<core::Frame>(f.frames[static_cast<std::size_t>(ch2)].frame) : nullptr;
            check("a long chapter note runs on, all of it before Chapter Two", fis.size() >= 2 && before, std::to_string(fis.size()) + " pieces");
            check("Chapter Two still opens a right-hand page", p4 && d.side_of(p4->owner) == core::Side::Right);
        }
        // Re-flow after an edit in Chapter One == whole.
        {
            const core::Story before = *d.as<core::Story>("story.1");
            const auto old = L::flow_story(d, "story.1");
            D::edit_text(j, d, "story.1", {2, 0}, {2, 0}, "Even so, ", "Typing", "");
            const auto re = L::reflow_story(d, "story.1", before, old);
            const auto wf = L::flow_story(d, "story.1");
            bool same = re.frames.size() == wf.frames.size() && re.overset == wf.overset;
            for (std::size_t i = 0; same && i < re.frames.size(); ++i) {
                same = re.frames[i].notes.size() == wf.frames[i].notes.size() && re.frames[i].lines.size() == wf.frames[i].lines.size() &&
                       re.frames[i].end_from_in == wf.frames[i].end_from_in;
                for (std::size_t k = 0; same && k < re.frames[i].notes.size(); ++k)
                    same = re.frames[i].notes[k].from == wf.frames[i].notes[k].from && std::abs(re.frames[i].notes[k].y - wf.frames[i].notes[k].y) < 1e-9;
            }
            check("re-flow with chapter endnotes == whole flow", same);
        }
        D::set_note_place(j, d, core::NoteAt::Foot, "Notes");
        bool any = false;
        for (const auto& ft : flows.get(d, "story.1").frames) for (const auto& np : ft.notes) any = any || np.end;
        check("back to the foot: no endnotes", !any);
    }

    {
        std::cout << "\n-- note links in the PDF (s123) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        namespace fs = std::filesystem;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        D::insert_note(j, d, "story.1", {1, 4}, "On page 1.");
        const auto slurp = [](const std::string& f) {
            std::ifstream in(f, std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            return ss.str();
        };
        const auto count = [](const std::string& hay, const std::string& n) {
            int k = 0;
            for (auto at = hay.find(n); at != std::string::npos; at = hay.find(n, at + 1)) ++k;
            return k;
        };
        L::FlowCache flows;
        auto links = L::note_links(d, flows);
        const auto page_of_note = [&](int p) -> std::string {
            for (const auto& ft : flows.get(d, "story.1").frames)
                for (const auto& np : ft.notes)
                    if (np.para == p && !np.cont) return d.as<core::Frame>(ft.frame)->owner;
            return {};
        };
        check("two links a note: to it, and back", links.size() == 4, std::to_string(links.size()));
        bool ok = links.size() == 4;
        for (std::size_t i = 0; ok && i < links.size(); i += 2) {
            const auto& to = links[i];
            const auto& back = links[i + 1];
            const auto* pg = d.as<core::Page>(to.page);
            ok = ok && pg && to.box.w > 0 && to.box.h > 0 && to.box.x >= 0 && to.box.r() <= pg->w && to.box.y >= 0 && to.box.b() <= pg->h &&
                 back.page == to.to && back.to == to.page;
        }
        check("each box on its page; back goes where the mark is", ok);
        check("the first note's mark on page 1 links to its own page's foot", !links.empty() && links[0].to == page_of_note(1) &&
                                                                              links[0].page == links[0].to);
        // As endnotes: the first note's link crosses to where the endnotes are.
        D::set_note_place(j, d, core::NoteAt::StoryEnd, "Notes");
        links = L::note_links(d, flows);
        check("endnotes: the mark on page 1 links to the endnote's page", links.size() == 4 && links[0].to == page_of_note(1) &&
                                                                          links[0].to != links[0].page);
        // Written: the screen PDF and the print PDF carry them (plus the contents' two).
        const auto base = fs::temp_directory_path() / "composix_s123";
        fs::create_directories(base);
        const L::ImageSource src = [](const core::Asset&, double, double) -> cairo_surface_t* { return nullptr; };
        L::PdfOptions o;
        o.compress = false;
        L::PdfReport rep;
        std::string err;
        const auto contents = static_cast<int>(L::contents_links(d, flows).size());
        const auto pout = (base / "print.pdf").string();
        check("print PDF: the note links written", L::export_print_pdf(d, pout, o, src, &rep, &err) &&
                                                   count(slurp(pout), "/Subtype /Link") == contents + 4, err);
        const auto sout = (base / "screen.pdf").string();
        check("screen PDF: the note links written", L::export_pdf(d, sout, o, src, &rep, &err) &&
                                                    count(slurp(sout), "/Subtype /Link") == contents + 4, err);
        std::error_code ec;
        if (!std::getenv("KEEP_PDF")) fs::remove_all(base, ec);
    }

    {
        std::cout << "\n-- numbers by chapter (s124) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        D::insert_note(j, d, "story.1", {1, 4}, "One.");     // Chapter One
        D::insert_note(j, d, "story.1", {2, 6}, "Two.");     // Chapter One
        const auto* st = d.as<core::Story>("story.1");       // the sample's own: Chapter Two (paragraph 5)
        check("through the story: 1 2 3", core::note_number(d, *st, 1, 0) == 1 && core::note_number(d, *st, 2, 0) == 2 &&
                                          core::note_number(d, *st, 5, 0) == 3);
        D::set_note_numbering(j, d, core::NoteRestart::Chapter, core::NoteStyle::Arabic);
        st = d.as<core::Story>("story.1");
        check("by chapter: 1 2, then 1 again", core::note_number(d, *st, 1, 0) == 1 && core::note_number(d, *st, 2, 0) == 2 &&
                                               core::note_number(d, *st, 5, 0) == 1);
        {
            core::Doc e;
            std::string err;
            check("json: by chapter kept", core::from_json(e, core::to_json(d), &err) && e.settings().note_restart == core::NoteRestart::Chapter, err);
        }
        L::FlowCache flows;
        const auto nums = [&] {
            std::vector<int> v;
            for (const auto& ft : flows.get(d, "story.1").frames)
                for (const auto& np : ft.notes) if (np.k >= 0 && !np.cont) v.push_back(np.number);
            return v;
        };
        check("at the foot: 1 2 / 1", nums() == std::vector<int>{1, 2, 1});
        check("the marks: as set", L::laid_note_number(flows.get(d, "story.1"), 5, 0) == 1 && L::laid_note_number(flows.get(d, "story.1"), 2, 0) == 2);
        D::set_note_place(j, d, core::NoteAt::ChapterEnd, "Notes");
        check("chapter endnotes: 1 2 / 1", nums() == std::vector<int>{1, 2, 1});
        D::set_note_place(j, d, core::NoteAt::StoryEnd, "Notes");
        check("story endnotes, by chapter: still each chapter's own", nums() == std::vector<int>{1, 2, 1});
        D::set_note_place(j, d, core::NoteAt::Foot, "Notes");
        {   // An edit -- re-flow == whole.
            const core::Story before = *d.as<core::Story>("story.1");
            const auto old = L::flow_story(d, "story.1");
            D::edit_text(j, d, "story.1", {3, 0}, {3, 0}, "Late that night ", "Typing", "");
            const auto re = L::reflow_story(d, "story.1", before, old);
            const auto wf = L::flow_story(d, "story.1");
            bool same = re.frames.size() == wf.frames.size();
            for (std::size_t i = 0; same && i < re.frames.size(); ++i) {
                same = re.frames[i].notes.size() == wf.frames[i].notes.size();
                for (std::size_t k = 0; same && k < re.frames[i].notes.size(); ++k) same = re.frames[i].notes[k].number == wf.frames[i].notes[k].number;
            }
            check("re-flow by chapter == whole", same);
        }
        {   // A new chapter start (Heading 1 starts a right page) before the second note: re-flow == whole, renumbered.
            const core::Story before = *d.as<core::Story>("story.1");
            const auto old = L::flow_story(d, "story.1");
            D::set_para_style(j, d, "story.1", {2, 0}, {2, 0}, "style.2");
            const auto re = L::reflow_story(d, "story.1", before, old);
            const auto wf = L::flow_story(d, "story.1");
            bool same = re.frames.size() == wf.frames.size();
            std::vector<int> a, b;
            for (std::size_t i = 0; same && i < re.frames.size(); ++i) {
                for (const auto& np : re.frames[i].notes) if (!np.cont) a.push_back(np.number);
                for (const auto& np : wf.frames[i].notes) if (!np.cont) b.push_back(np.number);
            }
            check("a chapter start added: re-flow == whole, the notes after it from 1", same && a == b && !b.empty() && std::all_of(b.begin(), b.end(), [](int n) { return n == 1; }),
                  std::to_string(b.size()));
        }
    }

    {
        std::cout << "\n-- chapter heads among endnotes (s125) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        D::insert_note(j, d, "story.1", {1, 4}, "One.");
        D::insert_note(j, d, "story.1", {2, 6}, "Two.");
        D::set_note_place(j, d, core::NoteAt::StoryEnd, "Notes");
        D::set_note_numbering(j, d, core::NoteRestart::Chapter, core::NoteStyle::Arabic);
        L::FlowCache flows;
        const auto order = [&] {
            std::vector<std::pair<int, int>> v;   // (k, para)
            for (const auto& ft : flows.get(d, "story.1").frames)
                for (const auto& np : ft.notes) if (!np.cont) v.push_back({np.k, np.para});
            return v;
        };
        using V = std::vector<std::pair<int, int>>;
        check("story endnotes by chapter: the title, each chapter's head, its notes",
              order() == V{{-1, 1}, {-2, 0}, {0, 1}, {0, 2}, {-2, 4}, {0, 5}});
        {
            std::vector<std::string> heads;
            const auto& f = flows.get(d, "story.1");
            for (const auto& ft : f.frames) {
                std::vector<double> ys;
                for (const auto& np : ft.notes) if (np.k == -2) ys.push_back(np.y);
                if (ys.empty()) continue;
                L::visit_lines(d, "story.1", f, ft.frame, [&](const L::LineVisit& v) {
                    if (v.from != -4) return;
                    const char* t = pango_layout_get_text(v.line->layout);
                    const std::string line(t + v.line->start_index, static_cast<std::size_t>(v.line->length));
                    if (line.rfind("Chapter", 0) == 0) heads.push_back(line);
                });
            }
            check("the heads read the chapters' first words", heads == std::vector<std::string>{"Chapter One", "Chapter Two"},
                  std::to_string(heads.size()));
        }
        D::set_note_numbering(j, d, core::NoteRestart::Story, core::NoteStyle::Arabic);
        check("through the story: no heads", order() == V{{-1, 1}, {0, 1}, {0, 2}, {0, 5}});
        D::set_note_numbering(j, d, core::NoteRestart::Chapter, core::NoteStyle::Arabic);
        D::set_note_place(j, d, core::NoteAt::ChapterEnd, "Notes");
        bool none = true;
        for (const auto& kp : order()) none = none && kp.first != -2;
        check("chapter endnotes: no heads (each group follows its chapter)", none);
    }

    {
        std::cout << "\n-- the number in the note (s126) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);   // its note: Chapter Two, page 3
        using NN = core::Settings::NoteNum;
        check("in the note: one step, the same again none", D::set_note_num(j, d, NN::Period) && !D::set_note_num(j, d, NN::Period));
        {
            core::Doc e;
            std::string err;
            check("json: kept", core::from_json(e, core::to_json(d), &err) && e.settings().note_num == NN::Period, err);
        }
        // The note's first line, as drawn.
        const auto first_line = [&](bool* raised) {
            const auto f = L::flow_story(d, "story.1");
            std::string out;
            *raised = false;
            for (const auto& ft : f.frames) {
                if (ft.notes.empty()) continue;
                L::visit_lines(d, "story.1", f, ft.frame, [&](const L::LineVisit& v) {
                    if (v.from != -4 || !out.empty()) return;
                    const char* t = pango_layout_get_text(v.line->layout);
                    out.assign(t + v.line->start_index, static_cast<std::size_t>(v.line->length));
                    if (PangoAttrList* al = pango_layout_get_attributes(v.line->layout)) {
                        PangoAttrIterator* it = pango_attr_list_get_iterator(al);
                        do {
                            if (pango_attr_iterator_get(it, PANGO_ATTR_RISE)) *raised = true;
                        } while (pango_attr_iterator_next(it));
                        pango_attr_iterator_destroy(it);
                    }
                });
            }
            return out;
        };
        bool raised = false;
        check("\"1.\" before the words", first_line(&raised).rfind("1. The foldout", 0) == 0 && !raised);
        L::FlowCache flows;
        const auto page = d.as<core::Frame>(L::flow_story(d, "story.1").frames.back().frame)->owner;
        const auto k1 = L::page_key(d, flows, page);
        D::set_note_num(j, d, NN::Paren);
        check("\"1)\" before the words", first_line(&raised).rfind("1) The foldout", 0) == 0 && !raised);
        check("the page's key moves with it (its picture redrawn)", !(L::page_key(d, flows, page) == k1));
        D::set_note_num(j, d, NN::Raised);
        check("raised: \"1\" raised and smaller", first_line(&raised).rfind("1 The foldout", 0) == 0 && raised);
        D::set_note_num(j, d, NN::Plain);
        check("plain: \"1\", as before", first_line(&raised).rfind("1 The foldout", 0) == 0 && !raised);
    }

    {
        std::cout << "\n-- cross-references (s128) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto before = d.records();
        const auto P = [&](int p) -> const core::Para& { return d.as<core::Story>("story.1")->paras[static_cast<std::size_t>(p)]; };
        // A reference in Chapter One to Chapter Two's heading (paragraph 4).
        check("a reference to a heading: one step", D::insert_ref_to(j, d, "story.1", {1, 4}, "story.1", 4, core::RefShow::Page) && j.size() == 1);
        check("the heading got an anchor at its start, the reference its target",
              P(4).anchors.size() == 1 && P(4).text.rfind(core::kMarkAnchor, 0) == 0 && P(1).refs.size() == 1 &&
                  P(1).refs[0].target == P(4).anchors[0] && P(1).text.substr(4, 3) == core::kMarkRef);
        check("find_anchor: where it is", [&] {
            const auto a = D::find_anchor(d, P(4).anchors[0]);
            return a.story == "story.1" && a.at.para == 4 && a.at.byte == 0;
        }());
        D::insert_ref_to(j, d, "story.1", {2, 0}, "story.1", 4, core::RefShow::Text);
        check("a second reference: the same anchor", P(4).anchors.size() == 1 && P(2).refs.size() == 1 && P(2).refs[0].target == P(4).anchors[0]);
        // Shown on the page: the page's number, the words, both; the anchor unseen.
        const auto shown = [&](int p) {
            const auto r = L::resolve_story(d, "story.1", "");
            return r.any ? r.text.paras[static_cast<std::size_t>(p)].text : std::string{};
        };
        const auto folio3 = d.folio(d.as<core::Frame>(L::flow_story(d, "story.1").frames.back().frame)->owner);
        check("shows: the heading's page", shown(1).substr(4, folio3.size()) == folio3, shown(1).substr(0, 20));
        check("shows: the heading's words", shown(2).rfind("Chapter Two", 0) == 0, shown(2).substr(0, 20));
        check("the anchor: nothing to see", shown(4).rfind("Chapter Two", 0) == 0);
        D::insert_ref_to(j, d, "story.1", {3, 0}, "story.1", 4, core::RefShow::TextPage);
        check("shows: both", shown(3).rfind("“Chapter Two” on page " + folio3, 0) == 0, shown(3).substr(0, 30));
        // Editing carries them.
        D::edit_text(j, d, "story.1", {1, 2}, {1, 2}, "\n", "Typing", "");
        check("a split: the reference goes with its mark", P(1).refs.empty() && P(2).refs.size() == 1);
        const int m = static_cast<int>(P(2).text.find(core::kMarkRef));
        D::edit_text(j, d, "story.1", {2, m}, {2, m + 3}, "", "Delete", "");
        check("its mark deleted: the reference goes", P(2).refs.empty());
        D::edit_text(j, d, "story.1", {0, 0}, {0, 0}, std::string(core::kMarkRef) + core::kMarkAnchor, "Paste", "");
        check("pasted marks: a reference with no target, an anchor with no id",
              P(0).refs.size() == 1 && P(0).refs[0].target.empty() && P(0).anchors.size() == 1 && P(0).anchors[0].empty());
        check("no target: \"??\"", shown(0).rfind("??", 0) == 0);
        D::set_char_style(j, d, "story.1", {5, 0}, {5, 4}, "style.5");
        check("a character style over the anchor keeps it", P(5).anchors.size() == 1 && !P(5).anchors[0].empty());
        {
            core::Doc e;
            std::string err;
            check("json: refs and anchors kept", core::from_json(e, core::to_json(d), &err) && e.records() == d.records(), err);
        }
        // The heading goes: references to it show "??".
        D::edit_text(j, d, "story.1", {5, 0}, {5, static_cast<int>(P(5).text.size())}, "", "Delete", "");
        check("the target gone: \"??\"", shown(4).find("??") != std::string::npos, shown(4).substr(0, 30));
        // The page's key follows a reference's value.
        while (j.can_undo()) j.undo();
        check("undo all: as it was", d.records() == before && d.strays() == 0);
        D::insert_ref_to(j, d, "story.1", {1, 4}, "story.1", 4, core::RefShow::Text);
        L::FlowCache flows;
        const auto page1 = d.as<core::Frame>(L::flow_story(d, "story.1").frames.front().frame)->owner;
        std::string page_with_ref;
        for (const auto& ft : L::flow_story(d, "story.1").frames)
            for (const auto& l : ft.lines)
                if (l.para == 1 && l.start <= 4 && l.end > 4) page_with_ref = d.as<core::Frame>(ft.frame)->owner;
        (void)page1;
        const auto k1 = L::page_key(d, flows, page_with_ref);
        D::edit_text(j, d, "story.1", {4, 3}, {4, 3}, "The ", "Typing", "");   // the heading's words change
        check("the referring page's key moves when the heading's words do", !(L::page_key(d, flows, page_with_ref) == k1));
    }

    {
        std::cout << "\n-- cross-reference words; to notes (s129) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);   // its note: Chapter Two (paragraph 5)
        check("patterns: page, words, both, note", core::ref_pattern({"", core::RefShow::Page, {}}) == "#" &&
                                                   core::ref_pattern({"", core::RefShow::Text, {}}) == "%" &&
                                                   core::ref_pattern({"", core::RefShow::Note, {}}) == "*" &&
                                                   core::ref_pattern({"", core::RefShow::Custom, "see p. #"}) == "see p. #");
        const auto P = [&](int p) -> const core::Para& { return d.as<core::Story>("story.1")->paras[static_cast<std::size_t>(p)]; };
        const auto shown = [&](int p) {
            const auto r = L::resolve_story(d, "story.1", "");
            return r.any ? r.text.paras[static_cast<std::size_t>(p)].text : std::string{};
        };
        check("a reference to a note: one step", D::insert_ref_to_note(j, d, "story.1", {1, 4}, "story.1", 5, 0, core::RefShow::Note) && j.size() == 1);
        const int m = D::note_mark_byte(*d.as<core::Story>("story.1"), 5, 0);
        check("the anchor right before the note's mark", m >= 3 && P(5).text.compare(static_cast<std::size_t>(m - 3), 3, core::kMarkAnchor) == 0 &&
                                                           P(1).refs.size() == 1 && P(1).refs[0].target == P(5).anchors[0]);
        check("shows the note's number", shown(1).substr(4, 1) == "1", shown(1).substr(0, 12));
        D::insert_ref_to_note(j, d, "story.1", {2, 0}, "story.1", 5, 0, core::RefShow::Custom, "see note *, page #");
        const auto folio3 = d.folio(d.as<core::Frame>(L::flow_story(d, "story.1").frames.back().frame)->owner);
        check("words of its own: \"see note 1, page 3\"", P(5).anchors.size() == 1 && shown(2).rfind("see note 1, page " + folio3, 0) == 0,
              shown(2).substr(0, 24));
        D::insert_note(j, d, "story.1", {3, 0}, "Earlier.");   // a note before it: it becomes 2
        check("the note renumbered: the reference follows", shown(1).substr(4, 1) == "2" && shown(2).rfind("see note 2", 0) == 0);
        D::insert_ref_to(j, d, "story.1", {3, 0}, "story.1", 4, core::RefShow::Custom, "“%” (p. #)");
        check("words of its own, a heading: “Chapter Two” (p. 3)", shown(3).rfind("“Chapter Two” (p. " + folio3 + ")", 0) == 0,
              shown(3).substr(0, 30));
        {
            core::Doc e;
            std::string err;
            check("json: the pattern kept", core::from_json(e, core::to_json(d), &err) && e.records() == d.records(), err);
        }
        // Preflight: all well -- nothing; the heading gone -- a warning.
        L::FlowCache flows;
        const auto xrefs = [&] {
            int n = 0;
            for (const auto& i : L::preflight_issues(d, flows, {})) n += i.kind == "xref";
            return n;
        };
        check("preflight: references that resolve -- nothing", xrefs() == 0);
        D::edit_text(j, d, "story.1", {4, 0}, {4, static_cast<int>(P(4).text.size())}, "", "Delete", "");
        check("preflight: the target gone -- a warning", xrefs() == 1, std::to_string(xrefs()));
        j.undo();
        // A target in a story not placed: a warning.
        const auto ded = D::insert_anchor(j, d, "story.4", {0, 0});
        D::insert_ref(j, d, "story.1", {2, 0}, ded, core::RefShow::Page);
        D::delete_frames(j, d, {[&] {
            for (const auto& id : d.ids_of<core::Frame>()) if (d.as<core::Frame>(id)->story == "story.4") return id;
            return std::string{};
        }()});
        check("preflight: the target not on a page -- a warning", xrefs() == 1, std::to_string(xrefs()));
    }

    {
        std::cout << "\n-- editing a cross-reference (s130) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto S = [&]() -> const core::Story& { return *d.as<core::Story>("story.1"); };
        const auto P = [&](int p) -> const core::Para& { return S().paras[static_cast<std::size_t>(p)]; };
        const auto shown = [&](int p) {
            const auto r = L::resolve_story(d, "story.1", "");
            return r.any ? r.text.paras[static_cast<std::size_t>(p)].text : std::string{};
        };
        D::insert_ref_to(j, d, "story.1", {2, 0}, "story.1", 0, core::RefShow::Page);
        D::insert_ref_to(j, d, "story.1", {2, 3}, "story.1", 4, core::RefShow::Text);   // after the first: k 1
        check("found: the mark at the cursor", D::ref_near(S(), {2, 3}).k == 1 && D::ref_near(S(), {2, 3}).byte == 3);
        check("found: the mark just before it", D::ref_near(S(), {2, 6}).k == 1 && D::ref_near(S(), {2, 0}).k == 0);
        check("none away from a mark", D::ref_near(S(), {2, 9}).k < 0 && D::ref_near(S(), {1, 0}).k < 0);
        const auto n0 = j.size();
        check("what it shows changed: one step", D::set_ref(j, d, "story.1", 2, 1, core::RefShow::Custom, "see %") && j.size() == n0 + 1);
        check("  its words: \"see Chapter Two\"", shown(2).find("see Chapter Two") != std::string::npos, shown(2).substr(0, 40));
        check("the same again: nothing", !D::set_ref(j, d, "story.1", 2, 1, core::RefShow::Custom, "see %") && j.size() == n0 + 1);
        check("the other reference untouched", P(2).refs[0].show == core::RefShow::Page && P(2).refs[0].target == P(0).anchors[0]);
        j.undo();
        check("undone: as it was", P(2).refs[1].show == core::RefShow::Text && P(2).refs[1].pattern.empty());
        // Pointed elsewhere: at a heading with no anchor yet -- made, one step.
        const auto anchors_before = P(3).anchors.size();
        check("pointed elsewhere: one step", D::retarget_ref(j, d, "story.1", 2, 0, "story.1", 3, -1, core::RefShow::Text) && j.size() == n0 + 1);
        check("  the anchor made there; the reference points at it", P(3).anchors.size() == anchors_before + 1 && P(2).refs[0].target == P(3).anchors[0]);
        check("  the anchor where it was kept", !P(0).anchors.empty());
        check("  its words now", shown(2).rfind("By noon", 0) == 0, shown(2).substr(0, 20));
        check("the same target and showing: nothing", !D::retarget_ref(j, d, "story.1", 2, 0, "story.1", 3, -1, core::RefShow::Text));
        // In the same paragraph, before the reference: the reference is still k 0.
        check("pointed at its own paragraph", D::retarget_ref(j, d, "story.1", 2, 1, "story.1", 2, -1, core::RefShow::Page) &&
                                              P(2).refs.size() == 2 && P(2).refs[1].target == P(2).anchors[0]);
        check("pointed at the note", D::retarget_ref(j, d, "story.1", 2, 1, "story.1", 5, 0, core::RefShow::Note) &&
                                     shown(2).find("1") != std::string::npos);
        check("a note that isn't there: nothing", !D::retarget_ref(j, d, "story.1", 2, 1, "story.1", 5, 3, core::RefShow::Note));
        check("no reference there: nothing", !D::set_ref(j, d, "story.1", 2, 5, core::RefShow::Page) &&
                                             !D::retarget_ref(j, d, "story.1", 1, 0, "story.1", 4, -1, core::RefShow::Page));
        {   // The hover's words.
            L::FlowCache fc;
            const auto w = L::ref_where(d, P(2).refs[1], fc, "story.1");
            check("hover: \"points at note 1, page \u2026\"", w.find("points at note 1, page ") != std::string::npos && w.find("(Chapters)") == std::string::npos, w);
            check("hover: from another story -- its name", L::ref_where(d, P(2).refs[1], fc, "story.2").find("(Chapters)") != std::string::npos);
            check("hover: a heading's words", L::ref_where(d, P(2).refs[0], fc, "story.1").find("\u201cBy noon") != std::string::npos);
            check("hover: gone", L::ref_where(d, core::XRef{"anchor.99", core::RefShow::Page, {}}, fc).find("gone") != std::string::npos);
        }
        while (j.can_undo()) j.undo();
        check("undo all: no references, no anchors", P(2).refs.empty() && P(0).anchors.empty() && d.strays() == 0);
    }

    {
        std::cout << "\n-- cross-reference links in the PDF (s131) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        namespace fs = std::filesystem;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        L::FlowCache flows;
        check("no references: no links", L::xref_links(d, flows).empty());
        const auto P = [&](int p) -> const core::Para& { return d.as<core::Story>("story.1")->paras[static_cast<std::size_t>(p)]; };
        // On page 1 (Chapter One's paragraph 1), to Chapter Two's heading.
        D::insert_ref_to(j, d, "story.1", {1, 4}, "story.1", 4, core::RefShow::TextPage);
        const auto& flow = flows.get(d, "story.1");
        const auto page_of = [&](int p, int b) -> std::string {
            const auto c = L::caret_at(d, "story.1", flow, {p, b});
            return c.ok ? d.as<core::Frame>(c.frame)->owner : std::string{};
        };
        const auto from_page = page_of(1, 4), head_page = page_of(4, 0);
        auto links = L::xref_links(d, flows);
        bool ok = !links.empty();
        for (const auto& l : links) {
            const auto* pg = d.as<core::Page>(l.page);
            ok = ok && pg && l.page == from_page && l.to == head_page && l.box.w > 0 && l.box.h > 0 && l.box.x >= 0 &&
                 l.box.r() <= pg->w + 0.5 && l.box.y >= 0 && l.box.b() <= pg->h;
        }
        check("a reference: a box on its page, to the heading's page", ok && from_page != head_page, std::to_string(links.size()));
        {   // The box is as wide as the words shown ("Chapter Two" on page 3 is wider than a bare number).
            double w = 0;
            for (const auto& l : links) w += l.box.w;
            D::set_ref(j, d, "story.1", 1, 0, core::RefShow::Page);
            double w2 = 0;
            for (const auto& l : L::xref_links(d, flows)) w2 += l.box.w;
            check("the box: the words it shows (page alone: narrower)", w2 > 0 && w2 < w, std::to_string(w) + " / " + std::to_string(w2));
        }
        // To a note: where the note is set.
        D::insert_ref_to_note(j, d, "story.1", {2, 0}, "story.1", 5, 0, core::RefShow::Note);
        std::string note_page;
        double note_y = 0;
        for (const auto& ft : flows.get(d, "story.1").frames)
            for (const auto& np : ft.notes)
                if (np.para == 5 && !np.cont && note_page.empty()) {
                    note_page = d.as<core::Frame>(ft.frame)->owner;
                    note_y = d.as<core::Frame>(ft.frame)->y + np.y;
                }
        links = L::xref_links(d, flows);
        bool to_note = false;
        for (const auto& l : links) to_note = to_note || (l.to == note_page && std::abs(l.to_y - (note_y - 4)) < 0.5);
        check("to a note: where the note is set", to_note && !note_page.empty());
        // Gone: no link.
        const auto n = links.size();
        D::edit_text(j, d, "story.1", {4, 0}, {4, static_cast<int>(P(4).text.size())}, "", "Delete", "");
        check("its place gone: its link gone", L::xref_links(d, flows).size() < n);
        j.undo();
        // Written: in both PDFs, and counted on the Export sheet.
        const auto base = fs::temp_directory_path() / "composix_s131";
        fs::create_directories(base);
        const L::ImageSource src = [](const core::Asset&, double, double) -> cairo_surface_t* { return nullptr; };
        L::PdfOptions o;
        o.compress = false;
        L::PdfReport rep;
        std::string err;
        const auto count = [](const std::string& f) {
            std::ifstream in(f, std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            const auto hay = ss.str();
            int k = 0;
            for (auto at = hay.find("/Subtype /Link"); at != std::string::npos; at = hay.find("/Subtype /Link", at + 1)) ++k;
            return k;
        };
        const int want = static_cast<int>(L::contents_links(d, flows).size() + L::note_links(d, flows).size() + L::xref_links(d, flows).size());
        const auto pout = (base / "print.pdf").string(), sout = (base / "screen.pdf").string();
        check("print PDF: the cross-reference links written", L::export_print_pdf(d, pout, o, src, &rep, &err) && count(pout) == want, err);
        check("screen PDF: the cross-reference links written", L::export_pdf(d, sout, o, src, &rep, &err) && count(sout) == want, err);
        const auto sum = L::export_summary(d, flows, o, false, "");
        check("an anchored heading keeps its contents link", L::contents_links(d, flows).size() == 2 &&
                                                              L::bookmarks(d, flows).size() == 2 &&
                                                              L::bookmarks(d, flows)[1].title == "Chapter Two");
        check("the Export sheet counts them", sum.xref_links == static_cast<int>(L::xref_links(d, flows).size()) &&
                                              sum.nav.find("cross-reference link") != std::string::npos, sum.nav);
        std::error_code ec;
        if (!std::getenv("KEEP_PDF")) fs::remove_all(base, ec);
    }

    {
        std::cout << "\n-- copy and paste: notes, references, anchors (s132) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto S = [&]() -> const core::Story& { return *d.as<core::Story>("story.1"); };
        const auto P = [&](int p) -> const core::Para& { return S().paras[static_cast<std::size_t>(p)]; };
        const auto shown = [&](int p) {
            const auto r = L::resolve_story(d, "story.1", "");
            return r.any ? r.text.paras[static_cast<std::size_t>(p)].text : std::string{};
        };
        // Paragraph 1: "The" + a reference to Chapter Two, a note, an anchor + the rest.
        D::insert_ref_to(j, d, "story.1", {1, 3}, "story.1", 4, core::RefShow::Text);
        D::insert_note(j, d, "story.1", {1, 6}, "A note.");
        const auto aid = D::insert_anchor(j, d, "story.1", {1, 9});
        D::insert_ref(j, d, "story.1", {3, 0}, aid, core::RefShow::Page);   // something points at that anchor
        const core::TextPos a{1, 0}, b{1, 12};
        const auto text = core::text_between(S(), a, b);
        const auto c = D::carried_between(S(), a, b);
        check("carried: the note's words, the reference, the anchor", c.notes.size() == 1 && c.notes[0].text == "A note." &&
                                                                       c.refs.size() == 1 && c.refs[0].target == P(4).anchors[0] &&
                                                                       c.anchors == std::vector<std::string>{aid});
        check("carried: none outside", D::carried_between(S(), {0, 0}, {0, 5}) == D::Carried{});
        // Copy, paste into paragraph 2: the note's words, the reference's target; the anchor a new id.
        const auto n0 = j.size();
        D::edit_text(j, d, "story.1", {2, 0}, {2, 0}, text, "Paste", "", &c);
        check("paste: one step", j.size() == n0 + 1);
        check("paste: the note keeps its words", P(2).notes.size() == 1 && P(2).notes[0].text == "A note.");
        check("paste: the reference points where it did", P(2).refs.size() == 1 && P(2).refs[0] == P(1).refs[0] &&
                                                         shown(2).find("Chapter Two") != std::string::npos);
        check("paste of a copy: the anchor a fresh id", P(2).anchors.size() == 1 && !P(2).anchors[0].empty() &&
                                                        P(2).anchors[0] != aid && P(1).anchors[0] == aid);
        check("  the reference to the first still finds it", D::find_anchor(d, aid).at.para == 1);
        // Without what it carried (another app's text): blanks, as before.
        D::edit_text(j, d, "story.1", {2, 0}, {2, 0}, text, "Paste");
        check("a plain paste: an empty note, a reference to nothing, no id",
              P(2).notes.size() == 2 && P(2).notes[0].text.empty() && P(2).refs[0].target.empty() && P(2).anchors[0].empty());
        j.undo();
        j.undo();
        // Cut, then paste elsewhere: the anchor keeps its id; what pointed at it still does.
        D::edit_text(j, d, "story.1", a, b, "", "Cut");
        check("cut: the anchor gone", D::find_anchor(d, aid).story.empty());
        D::edit_text(j, d, "story.1", {2, 0}, {2, 0}, text, "Paste", "", &c);
        check("paste after a cut: the anchor keeps its id", D::find_anchor(d, aid).at.para == 2);
        check("  the reference to it still resolves", shown(3).rfind("??", 0) != 0, shown(3).substr(0, 10));
        // Pasted twice: the second a fresh id.
        D::edit_text(j, d, "story.1", {3, 0}, {3, 0}, text, "Paste", "", &c);
        int with_aid = 0;
        for (const auto& p : S().paras) with_aid += static_cast<int>(std::count(p.anchors.begin(), p.anchors.end(), aid));
        check("pasted twice: one place has the id", with_aid == 1);
        // In another story: the id taken there too.
        D::edit_text(j, d, "story.2", {0, 0}, {0, 0}, text, "Paste", "", &c);
        const auto& cap = d.as<core::Story>("story.2")->paras[0];
        check("into another story: a fresh id there", cap.anchors.size() == 1 && cap.anchors[0] != aid && cap.notes.size() == 1);
        while (j.can_undo()) j.undo();
        check("undo all: as loaded", P(1).anchors.empty() && P(2).notes.empty() && d.strays() == 0);
    }

    {
        std::cout << "\n-- the cross-references, listed (s133) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        L::FlowCache flows;
        check("none: an empty list", L::xref_list(d, flows).empty());
        D::insert_ref_to(j, d, "story.1", {3, 0}, "story.1", 4, core::RefShow::Text);           // paragraph 3
        D::insert_ref_to(j, d, "story.1", {1, 4}, "story.1", 5, core::RefShow::Page);           // paragraph 1 -- listed first
        D::insert_ref_to_note(j, d, "story.1", {1, 0}, "story.1", 5, 0, core::RefShow::Note);   // before it in paragraph 1
        D::insert_ref(j, d, "story.2", {0, 0}, "anchor.99", core::RefShow::Page);              // the caption's: points nowhere
        auto rows = L::xref_list(d, flows);
        check("four, in story then reading order", rows.size() == 4 && rows[0].story == "story.1" && rows[0].para == 1 && rows[0].k == 0 &&
                                                   rows[1].para == 1 && rows[1].k == 1 && rows[2].para == 3 && rows[3].story == "story.2",
              std::to_string(rows.size()));
        if (rows.size() == 4) {
            check("its page's folio", !rows[0].page.empty() && d.as<core::Page>(rows[0].page_id) != nullptr);
            check("what it shows; the words round it, marks left out",
                  rows[2].shows == "Chapter Two" && rows[2].context.find("Chapter Two") != std::string::npos &&
                  rows[2].context.find("\xEE\x80") == std::string::npos, rows[2].context);
            check("where it points", rows[0].where.find("points at note 1") != std::string::npos, rows[0].where);
            check("broken: the one pointing nowhere only", !rows[0].broken && !rows[1].broken && !rows[2].broken && rows[3].broken);
        }
        // The heading deleted: its reference broken too.
        D::edit_text(j, d, "story.1", {4, 0}, {4, static_cast<int>(d.as<core::Story>("story.1")->paras[4].text.size())}, "", "Delete", "");
        rows = L::xref_list(d, flows);
        int broken = 0;
        for (const auto& r : rows) broken += r.broken;
        check("its place deleted: broken", broken == 2, std::to_string(broken));
    }

    {
        std::cout << "\n-- index marks (s134) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto S = [&]() -> const core::Story& { return *d.as<core::Story>("story.1"); };
        const auto P = [&](int p) -> const core::Para& { return S().paras[static_cast<std::size_t>(p)]; };
        L::FlowCache flows;
        check("none: an empty index", L::index_entries(d, flows).empty());
        const auto n0 = j.size();
        const auto after = D::insert_index_mark(j, d, "story.1", {1, 4}, "  harbor  ", "smells");
        check("a mark: one step, after it", j.size() == n0 + 1 && after == core::TextPos{1, 7});
        check("  its term and sub-entry, trimmed", P(1).index.size() == 1 && P(1).index[0] == core::IndexMark{"harbor", "smells"} &&
                                                   P(1).text.compare(4, 3, core::kMarkIndex) == 0);
        check("an empty term: nothing", D::insert_index_mark(j, d, "story.1", {1, 0}, "   ") == core::TextPos{1, 0} && j.size() == n0 + 1);
        check("found at / just before the cursor", D::index_near(S(), {1, 4}).k == 0 && D::index_near(S(), {1, 7}).k == 0 &&
                                                   D::index_near(S(), {1, 8}).k < 0);
        {   // Nothing shows on the page: the words as they were.
            const auto r = L::resolve_story(d, "story.1", "");
            check("on the page: nothing to see", r.any && r.text.paras[1].text == "The harbor smelled of diesel and oranges the morning the ship came in.");
        }
        // More: the same term on Chapter Two's page, another case; a term of its own; one in the caption.
        D::insert_index_mark(j, d, "story.1", {5, 0}, "Harbor", "smells");
        D::insert_index_mark(j, d, "story.1", {4, 0}, "Maps");
        D::insert_index_mark(j, d, "story.1", {3, 0}, "Guild");
        D::insert_index_mark(j, d, "story.1", {2, 0}, "harbor");
        auto ix = L::index_entries(d, flows);
        const auto folio = [&](int p) {
            const auto c = L::caret_at(d, "story.1", flows.get(d, "story.1"), {p, 0});
            return c.ok ? d.folio(d.as<core::Frame>(c.frame)->owner) : std::string{};
        };
        check("entries sorted, case folded: guild, harbor, harbor/smells, maps",
              ix.size() == 4 && ix[0].term == "Guild" && ix[1].term == "harbor" && ix[1].sub.empty() && ix[2].sub == "smells" &&
                  ix[3].term == "Maps", std::to_string(ix.size()));
        if (ix.size() == 4) {
            check("one entry for both cases; its pages in order, each once",
                  ix[2].marks == 2 && ix[2].folios == std::vector<std::string>{folio(1), folio(5)} && folio(1) != folio(5));
            check("the term's own line: its own marks' pages", ix[1].marks == 1 && ix[1].folios == std::vector<std::string>{folio(2)});
        }
        {   // A sub-entry alone still heads under its term.
            core::Doc e;
            core::DocJournal je{e};
            e.set_door([&je] { return je.open(); });
            core::load_sample(e);
            D::insert_index_mark(je, e, "story.1", {1, 0}, "Ships", "cargo");
            L::FlowCache fe;
            const auto ex = L::index_entries(e, fe);
            check("a sub-entry alone: its term heads it, with no pages", ex.size() == 2 && ex[0].term == "Ships" && ex[0].sub.empty() &&
                                                                          ex[0].pages.empty() && ex[0].marks == 0 && ex[1].sub == "cargo");
        }
        // Changed: one step; the same again nothing.
        check("changed: one step", D::set_index_mark(j, d, "story.1", 4, 0, "Charts", "folding"));
        check("the same again: nothing", !D::set_index_mark(j, d, "story.1", 4, 0, "Charts", "folding"));
        check("an empty term: nothing", !D::set_index_mark(j, d, "story.1", 4, 0, " "));
        // Kept through a character style and through copy / paste; saved.
        D::set_char_style(j, d, "story.1", {1, 0}, {1, 20}, "style.5");
        check("a character style keeps it", P(1).index.size() == 1 && P(1).index[0].term == "harbor");
        const auto c = D::carried_between(S(), {1, 0}, {1, 10});
        D::edit_text(j, d, "story.1", {3, 0}, {3, 0}, core::text_between(S(), {1, 0}, {1, 10}), "Paste", "", &c);
        check("pasted: the mark keeps its term", P(3).index.size() == 2 && P(3).index[0] == core::IndexMark{"harbor", "smells"});
        {
            core::Doc e;
            std::string err;
            check("json: kept", core::from_json(e, core::to_json(d), &err) && e.records() == d.records(), err);
        }
        while (j.can_undo()) j.undo();
        check("undo all: no marks", P(1).index.empty() && d.strays() == 0);
    }

    {
        std::cout << "\n-- the index, built (s135) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto last = d.pages().back();
        check("a list on no page that isn't one: nothing", D::add_list(j, d, core::ListKind::Index, "Index", "frame.1").empty());
        const auto n0 = j.size();
        const auto ix = D::add_list(j, d, core::ListKind::Index, "Index", last);
        check("a new index: one step, never built", !ix.empty() && j.size() == n0 + 1 && d.as<core::GenList>(ix)->built_entries < 0 &&
                                                    d.as<core::GenList>(ix)->kind == core::ListKind::Index);
        L::FlowCache fl;
        const auto stale_of = [&](const std::string& id) {
            L::FlowCache f;
            for (const auto& x : L::stale_lists(d, f)) if (x.list == id) return x.why;
            return std::string{};
        };
        check("not built yet: stale", stale_of(ix) == "Not built yet");
        D::insert_index_mark(j, d, "story.1", {1, 4}, "harbor", "smells");
        D::insert_index_mark(j, d, "story.1", {5, 0}, "Harbor", "smells");
        D::insert_index_mark(j, d, "story.1", {2, 0}, "harbor");
        D::insert_index_mark(j, d, "story.1", {3, 0}, "guild");
        D::insert_index_mark(j, d, "story.1", {4, 0}, "Maps");
        const auto en = L::list_entries(d, fl, ix);
        const auto f = [&](int p) {
            const auto c = L::caret_at(d, "story.1", fl.get(d, "story.1"), {p, 0});
            return c.ok ? d.folio(d.as<core::Frame>(c.frame)->owner) : std::string{};
        };
        check("entries: a letter heading each initial; term, its sub-entries",
              en.size() == 7 && en[0].text == "G" && en[0].level == 0 && en[1].text == "guild" && en[2].text == "H" &&
                  en[3].text == "harbor" && en[3].level == 1 && en[4].text == "smells" && en[4].level == 2 &&
                  en[4].folio == f(1) + ", " + f(5) && en[5].text == "M" && en[6].text == "Maps",
              std::to_string(en.size()));
        check("built: one step", L::build_list(j, d, ix));
        const auto* l = d.as<core::GenList>(ix);
        const auto* st = d.as<core::Story>(l->story);
        check("written into a new story on its page", st && st->paras.size() == 7 && l->built_entries == 7);
        if (st && st->paras.size() == 7) {
            check("  \"harbor, n\" -- a comma, not a tab", st->paras[3].text == "harbor, " + f(2) && st->paras[4].text == "smells, " + f(1) + ", " + f(5));
            const auto* hs = d.as<core::Style>(st->paras[0].style);
            const auto* s1 = d.as<core::Style>(st->paras[1].style);
            const auto* s2 = d.as<core::Style>(st->paras[4].style);
            check("  letters in a \"Letter\" style, bold; sub-entries indented",
                  hs && s1 && s2 && hs->name == s1->name + " Letter" && hs->bold && s2->left_indent > s1->left_indent);
        }
        check("built: current", stale_of(ix).empty(), stale_of(ix));
        check("again: nothing", !L::build_list(j, d, ix));
        D::insert_index_mark(j, d, "story.1", {0, 0}, "anchors");
        check("a new mark: stale, saying which", stale_of(ix) == "\u201cA\u201d not listed yet", stale_of(ix));
        L::build_list(j, d, ix);
        D::edit_text(j, d, "story.1", {2, 0}, {2, 3}, "", "Delete", "");   // the harbor mark at {2,0} gone
        check("a mark gone: stale", !stale_of(ix).empty(), stale_of(ix));
        {   // A title written above the entries: kept by a rebuild.
            core::Story t = *d.as<core::Story>(d.as<core::GenList>(ix)->story);
            t.paras.insert(t.paras.begin(), core::Para{t.paras[1].style, "Index"});
            j.run("title", {d.as<core::GenList>(ix)->story}, [&] { d.put(d.as<core::GenList>(ix)->story, t); });
            L::build_list(j, d, ix);
            const auto& w = d.as<core::Story>(d.as<core::GenList>(ix)->story)->paras;
            check("a title above: kept; the letters after it", w.size() > 2 && w[0].text == "Index" && w[1].text == "A");
        }
        {   // The inspector: Build offered; marks counted.
            const auto o = core::project(d, "", nullptr, nullptr, nullptr);
            bool build = false, marks = false;
            for (const auto& it : o.items())
                if (it.id == ix)
                    for (const auto& fd : it.fields) {
                        build = build || (fd.edit == "genlist" && !fd.choices.empty());
                        marks = marks || (fd.label == "Marks" && fd.value == "5");
                    }
            check("the inspector: Build offered, the marks counted", build && marks);
        }
        while (j.can_undo()) j.undo();
        check("undo all: no index", !d.as<core::GenList>(ix) && d.strays() == 0);
    }

    {
        std::cout << "\n-- the index: ranges, see, sorting (s136) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        // A frame with a story on pages 4, 5, 6 and 8 (numbered so): "x" with marks.
        const auto page_numbered = [&](const std::string& f) {
            for (const auto& pg : d.pages()) if (d.folio(pg) == f) return pg;
            return std::string{};
        };
        std::map<std::string, std::string> story_on;
        for (const char* f : {"4", "5", "6", "8"}) {
            const auto pg = page_numbered(f);
            const auto fr = D::add_frame(j, d, pg, core::FrameKind::Text, {72, 72, 200, 100});
            story_on[f] = d.as<core::Frame>(fr)->story;
            D::edit_text(j, d, story_on[f], {0, 0}, {0, 0}, "x");
        }
        for (const char* f : {"4", "5", "6", "8"}) D::insert_index_mark(j, d, story_on[f], {0, 1}, "Ships");
        for (const char* f : {"4", "5"}) D::insert_index_mark(j, d, story_on[f], {0, 1}, "Sails");
        L::FlowCache fl;
        const auto find = [&](const std::string& term, const std::string& sub = {}) {
            for (const auto& e : L::index_entries(d, fl)) if (e.term == term && e.sub == sub) return e;
            return L::IndexEntry{};
        };
        check("three pages in a row and more: \"4\u20136, 8\"", find("Ships").pages_text == "4\u20136, 8", find("Ships").pages_text);
        check("two in a row stay apart: \"4, 5\"", find("Sails").pages_text == "4, 5", find("Sails").pages_text);
        // See and see also.
        D::insert_index_mark(j, d, "story.1", {1, 0}, core::IndexMark{"Charts", "", "Maps"});
        D::insert_index_mark(j, d, "story.1", {1, 0}, core::IndexMark{"Ships", "", "Boats", true});
        D::insert_index_mark(j, d, "story.1", {1, 0}, core::IndexMark{"Ships", "", "Anchors", true});
        check("a see: no page of its own", find("Charts").pages.empty() && find("Charts").see == std::vector<std::string>{"Maps"});
        check("see also: sorted, the pages kept", find("Ships").also == (std::vector<std::string>{"Anchors", "Boats"}) &&
                                                  find("Ships").pages.size() == 4);
        const auto ix = D::add_list(j, d, core::ListKind::Index, "Index", d.pages().back());
        const auto en = L::list_entries(d, fl, ix);
        std::map<std::string, std::string> line;
        for (const auto& e : en) line[e.text] = e.folio;
        check("written: \"Charts. See Maps\"", line.count("Charts. See Maps") && line["Charts. See Maps"].empty());
        check("written: \"Ships, 4\u20136, 8. See also Anchors; Boats\"", line["Ships"] == "4\u20136, 8. See also Anchors; Boats", line["Ships"]);
        L::build_list(j, d, ix);
        {
            const auto& w = d.as<core::Story>(d.as<core::GenList>(ix)->story)->paras;
            bool ok = false;
            for (const auto& p : w) ok = ok || p.text == "Ships, 4\u20136, 8. See also Anchors; Boats";
            check("built: the line as written", ok);
            L::FlowCache f2;
            bool stale = false;
            for (const auto& x : L::stale_lists(d, f2)) stale = stale || x.list == ix;
            check("built: current", !stale);
        }
        // Sorting: an accented initial with its letter, after the plain word.
        D::insert_index_mark(j, d, "story.1", {2, 0}, "\u00c9clair");
        D::insert_index_mark(j, d, "story.1", {2, 0}, "eagle");
        D::insert_index_mark(j, d, "story.1", {2, 0}, "echo");
        D::insert_index_mark(j, d, "story.1", {2, 0}, "eclair");
        std::vector<std::string> order;
        for (const auto& e : L::index_entries(d, fl)) if (e.sub.empty()) order.push_back(e.term);
        const auto at = [&](const std::string& t) { return std::find(order.begin(), order.end(), t) - order.begin(); };
        std::string ord;
        for (const auto& o : order) ord += o + " ";
        check("\u00c9clair with the e's: just after eclair, before Sails (not after z)",
              at("eagle") < at("echo") && at("echo") < at("eclair") && at("eclair") + 1 == at("\u00c9clair") && at("\u00c9clair") < at("Sails"), ord);
        bool e_head = false, accented_head = false;
        for (const auto& e : L::list_entries(d, fl, ix)) {
            e_head = e_head || (e.level == 0 && e.text == "E");
            accented_head = accented_head || (e.level == 0 && e.text == "\u00c9");
        }
        check("one letter heading E for both", e_head && !accented_head);
        // Kept: the see in the file; changed keeps it unless changed.
        {
            core::Doc e;
            std::string err;
            check("json: see and also kept", core::from_json(e, core::to_json(d), &err) && e.records() == d.records(), err);
        }
        int k0 = 0;
        while (d.as<core::Story>("story.1")->paras[1].index[static_cast<std::size_t>(k0)].term != "Charts") ++k0;
        check("renamed by term: its see kept", D::set_index_mark(j, d, "story.1", 1, k0, "Charts and plans") &&
                                               d.as<core::Story>("story.1")->paras[1].index[static_cast<std::size_t>(k0)].see == "Maps");
        check("a see emptied: also off", D::set_index_mark(j, d, "story.1", 1, k0, core::IndexMark{"Charts", "", " ", true}) &&
                                         !d.as<core::Story>("story.1")->paras[1].index[static_cast<std::size_t>(k0)].also);
        while (j.can_undo()) j.undo();
        check("undo all", d.strays() == 0 && d.as<core::Story>("story.1")->paras[1].index.empty());
    }

    {
        std::cout << "\n-- index links in the PDF (s137) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        namespace fs = std::filesystem;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto page_numbered = [&](const std::string& f) {
            for (const auto& pg : d.pages()) if (d.folio(pg) == f) return pg;
            return std::string{};
        };
        std::map<std::string, std::string> story_on;
        for (const char* f : {"4", "5", "6", "8"}) {
            const auto fr = D::add_frame(j, d, page_numbered(f), core::FrameKind::Text, {72, 144, 200, 100});
            story_on[f] = d.as<core::Frame>(fr)->story;
            D::edit_text(j, d, story_on[f], {0, 0}, {0, 0}, "x");
            D::insert_index_mark(j, d, story_on[f], {0, 1}, "Ships");
        }
        D::insert_index_mark(j, d, "story.1", {1, 4}, "harbor");
        D::insert_index_mark(j, d, "story.1", {1, 0}, core::IndexMark{"Charts", "", "Maps"});
        L::FlowCache fl;
        check("no index built: no links", L::index_links(d, fl).empty());
        const auto ix = D::add_list(j, d, core::ListKind::Index, "Index", d.pages().back());
        L::build_list(j, d, ix);
        auto links = L::index_links(d, fl);
        // Ships: "4\u20136" and "8" -- two links; harbor: one; Charts (a see): none.
        int ships = 0, to4 = 0, to8 = 0, harbor = 0;
        for (const auto& l : links) {
            if (l.to == page_numbered("4")) { ++to4; ++ships; }
            if (l.to == page_numbered("8")) { ++to8; ++ships; }
            if (l.to == d.as<core::Frame>(L::flow_story(d, "story.1").frames.front().frame)->owner) ++harbor;
        }
        check("three links: a range to its first page, a number, harbor's", links.size() == 3 && to4 == 1 && to8 == 1 && harbor == 1,
              std::to_string(links.size()));
        bool ok = true;
        for (const auto& l : links) {
            const auto* pg = d.as<core::Page>(l.page);
            ok = ok && l.page == d.pages().back() && pg && l.box.w > 0 && l.box.h > 0 && l.box.r() <= pg->w + 0.5 && l.box.b() <= pg->h;
        }
        check("each box on the index's page", ok);
        {   // The range's box is wider than the single number's.
            double wr = 0, w8 = 0, y4 = -1;
            for (const auto& l : links) {
                if (l.to == page_numbered("4")) { wr = l.box.w; y4 = l.to_y; }
                if (l.to == page_numbered("8")) w8 = l.box.w;
            }
            check("the range's box: the whole \"4\u20136\"", wr > w8 * 1.5, std::to_string(wr) + " / " + std::to_string(w8));
            check("to the mark: down its page where the frame is", y4 > 100 && y4 < 200, std::to_string(y4));
        }
        // Stale: a mark gone -- that line no longer reads right, its link goes; the rest stay.
        D::insert_index_mark(j, d, "story.1", {2, 0}, "harbor");      // the same page: the line still reads "harbor, 1"
        D::insert_index_mark(j, d, story_on["8"], {0, 1}, "harbor");  // page 8: now "harbor, 1, 8" -- stale
        links = L::index_links(d, fl);
        check("stale: the line that changed has no link; the others keep theirs", links.size() == 2, std::to_string(links.size()));
        L::build_list(j, d, ix);
        check("rebuilt: harbor's two numbers linked", L::index_links(d, fl).size() == 4);
        // Written: both PDFs; the Export sheet counts them.
        const auto base = fs::temp_directory_path() / "composix_s137";
        fs::create_directories(base);
        const L::ImageSource src = [](const core::Asset&, double, double) -> cairo_surface_t* { return nullptr; };
        L::PdfOptions o;
        o.compress = false;
        L::PdfReport rep;
        std::string err;
        const auto count = [](const std::string& f) {
            std::ifstream in(f, std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            const auto hay = ss.str();
            int k = 0;
            for (auto at = hay.find("/Subtype /Link"); at != std::string::npos; at = hay.find("/Subtype /Link", at + 1)) ++k;
            return k;
        };
        const int want = static_cast<int>(L::contents_links(d, fl).size() + L::note_links(d, fl).size() + L::xref_links(d, fl).size() +
                                          L::index_links(d, fl).size());
        const auto pout = (base / "print.pdf").string(), sout = (base / "screen.pdf").string();
        check("print PDF: the index links written", L::export_print_pdf(d, pout, o, src, &rep, &err) && count(pout) == want, err);
        check("screen PDF: the index links written", L::export_pdf(d, sout, o, src, &rep, &err) && count(sout) == want, err);
        const auto sum = L::export_summary(d, fl, o, false, "");
        check("the Export sheet counts them", sum.index_links == 4 && sum.nav.find("4 index links") != std::string::npos, sum.nav);
        std::error_code ec;
        if (!std::getenv("KEEP_PDF")) fs::remove_all(base, ec);
    }

    {
        std::cout << "\n-- the index's styles (s138) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        D::insert_index_mark(j, d, "story.1", {1, 4}, "harbor", "smells");
        D::insert_index_mark(j, d, "story.1", {2, 0}, "guild");
        const auto ix = D::add_list(j, d, core::ListKind::Index, "Index", d.pages().back());
        L::build_list(j, d, ix);
        const auto rows = [&] {
            std::map<std::string, std::string> m;
            const auto o = core::project(d, "", nullptr, nullptr, nullptr);   // (kept: items() is a reference into it)
            for (const auto& it : o.items())
                if (it.id == ix) for (const auto& f : it.fields) m[f.label] = f.value;
            return m;
        };
        auto r = rows();
        std::string seen;
        for (const auto& [k, v] : r) seen += k + "=" + v + "; ";
        check("the inspector: entries, sub-entries (the one Build made), page numbers; built current", r["Entries in"] == "Automatic" && r["Sub-entries in"] == "Body 2" &&
                                                                  r["Page numbers"] == "Yes" && r["Built"] == "Current", seen);
        const auto stale_of = [&] {
            L::FlowCache f;
            for (const auto& x : L::stale_lists(d, f)) if (x.list == ix) return x.why;
            return std::string{};
        };
        check("built: current", stale_of().empty(), stale_of());
        D::set_list_entry_style(j, d, ix, 1, "style.4");   // the Sources' style for entries
        check("a style picked: out of date, saying which", stale_of() == "Entries not in " + d.as<core::Style>("style.4")->name, stale_of());
        L::build_list(j, d, ix);
        const auto& w = d.as<core::Story>(d.as<core::GenList>(ix)->story)->paras;
        bool in4 = false, letter_from4 = false;
        for (const auto& p : w) {
            in4 = in4 || (p.text.rfind("guild", 0) == 0 && p.style == "style.4");
            const auto* st = d.as<core::Style>(p.style);
            letter_from4 = letter_from4 || (p.text == "G" && st && st->based_on == "style.4");
        }
        check("rebuilt: the entries in it, the letters made from it", in4 && letter_from4 && stale_of().empty(), stale_of());
        D::set_list_numbers(j, d, ix, false);
        L::build_list(j, d, ix);
        bool bare = false;
        for (const auto& p : d.as<core::Story>(d.as<core::GenList>(ix)->story)->paras) bare = bare || p.text == "guild";
        check("page numbers off: the words alone", bare && stale_of().empty());
        L::FlowCache f;
        check("page numbers off: no index links", L::index_links(d, f).empty());
    }

    {
        std::cout << "\n-- index marks that cover a stretch (s139) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto& st = *d.as<core::Story>("story.1");   // 0 Chapter One, 1-3 body, 4 Chapter Two, 5 map
        check("a heading: a Contents list's style", D::heading_para(d, st.paras[4]) && !D::heading_para(d, st.paras[2]));
        check("span ends: point, paragraph, to heading, 2 paragraphs, story end",
              D::index_span_end(d, st, 1, {"x"}) == 1 && D::index_span_end(d, st, 1, {"x", "", "", false, core::IndexSpan::Paragraph}) == 1 &&
                  D::index_span_end(d, st, 1, {"x", "", "", false, core::IndexSpan::NextHeading}) == 3 &&
                  D::index_span_end(d, st, 1, {"x", "", "", false, core::IndexSpan::Paragraphs, 2}) == 2 &&
                  D::index_span_end(d, st, 1, {"x", "", "", false, core::IndexSpan::Paragraphs, 99}) == 5 &&
                  D::index_span_end(d, st, 5, {"x", "", "", false, core::IndexSpan::NextHeading}) == 5 &&
                  D::index_span_end(d, st, 1, {"x", "", "", false, core::IndexSpan::StoryEnd}) == 5);
        // Two frames threaded on pages 4 and 5, the text long enough to run over both.
        const auto page_numbered = [&](const std::string& f) {
            for (const auto& pg : d.pages()) if (d.folio(pg) == f) return pg;
            return std::string{};
        };
        const auto f4 = D::add_frame(j, d, page_numbered("4"), core::FrameKind::Text, {72, 72, 200, 80});
        const auto f5 = D::add_frame(j, d, page_numbered("5"), core::FrameKind::Text, {72, 72, 200, 300});
        const auto f6 = D::add_frame(j, d, page_numbered("6"), core::FrameKind::Text, {72, 72, 200, 300});
        D::thread_frames(j, d, f4, f5);
        const auto run = d.as<core::Frame>(f4)->story;
        std::string body;
        for (int k = 0; k < 40; ++k) body += "Words enough to run on past the first frame and into the next one. ";
        D::edit_text(j, d, run, {0, 0}, {0, 0}, "Harbors\n" + body);
        D::edit_text(j, d, d.as<core::Frame>(f6)->story, {0, 0}, {0, 0}, "Six");
        L::FlowCache fl;
        const auto pages_of = [&](const std::string& term) {
            for (const auto& e : L::index_entries(d, fl)) if (e.term == term) return e.pages_text;
            return std::string{"(none)"};
        };
        D::insert_index_mark(j, d, run, {1, 0}, core::IndexMark{"Ports"});
        check("a point: its page only", pages_of("Ports") == "4", pages_of("Ports"));
        D::insert_index_mark(j, d, run, {1, 0}, core::IndexMark{"Harbors", "", "", false, core::IndexSpan::Paragraph});
        check("its paragraph over two pages in a row: \"4\u20135\"", pages_of("Harbors") == "4\u20135", pages_of("Harbors"));
        D::insert_index_mark(j, d, d.as<core::Frame>(f6)->story, {0, 0}, "Ports");
        D::insert_index_mark(j, d, run, {0, 0}, core::IndexMark{"Ports", "", "", false, core::IndexSpan::Point});
        check("points on 4 and 6: \"4, 6\"", pages_of("Ports") == "4, 6", pages_of("Ports"));
        {   // In the sample's own story: to the story end -- pages 1 and 3, not in a row.
            D::insert_index_mark(j, d, "story.1", {1, 0}, core::IndexMark{"Voyage", "", "", false, core::IndexSpan::StoryEnd});
            std::set<std::string> want;
            for (const auto& ft : L::flow_story(d, "story.1").frames)
                if (!ft.lines.empty()) want.insert(d.folio(d.as<core::Frame>(ft.frame)->owner));
            check("to the story's end: every page its text is on", pages_of("Voyage").find(*want.begin()) != std::string::npos &&
                                                                   pages_of("Voyage").find(", ") != std::string::npos, pages_of("Voyage"));
        }
        check("a See covers nothing (its span dropped)",
              D::insert_index_mark(j, d, "story.1", {2, 0}, core::IndexMark{"Charts", "", "Maps", false, core::IndexSpan::StoryEnd}) == core::TextPos{2, 3} &&
                  d.as<core::Story>("story.1")->paras[2].index[0].span == core::IndexSpan::Point);
        D::insert_index_mark(j, d, "story.1", {3, 0}, core::IndexMark{"Guild", "", "", false, core::IndexSpan::Paragraphs, 2});
        {
            core::Doc e;
            std::string err;
            check("json: span and count kept", core::from_json(e, core::to_json(d), &err) && e.records() == d.records(), err);
        }
        while (j.can_undo()) j.undo();
        check("undo all", d.strays() == 0 && d.as<core::Story>("story.1")->paras[1].index.empty());
    }

    {
        std::cout << "\n-- the bibliography, built (s140) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);   // list.2: Bibliography from style.4 (the Sources story), on the last page
        L::FlowCache fl;
        const auto stale_of = [&] {
            L::FlowCache f;
            for (const auto& x : L::stale_lists(d, f)) if (x.list == "list.2") return x.why;
            return std::string{};
        };
        check("never written: not out of date", stale_of().empty());
        // A source elsewhere too, one repeated, and one with "The".
        D::edit_text(j, d, "story.4", {2, static_cast<int>(d.as<core::Story>("story.4")->paras[2].text.size())},
                     {2, static_cast<int>(d.as<core::Story>("story.4")->paras[2].text.size())},
                     "\nThe Book of Tides.\nAlmanac of the Outer Ports, 3rd ed.\nBaltic Pilot.");
        auto en = L::list_entries(d, fl, "list.2");
        std::vector<std::string> w;
        for (const auto& e : en) w.push_back(e.text);
        check("the sources: sorted (\"The\" set aside), each once, no page numbers",
              w == (std::vector<std::string>{"Almanac of the Outer Ports, 3rd ed.", "Baltic Pilot.", "The Book of Tides.", "Harbor Registers, vols. II\u2013IV.",
                                             "Letters of the Surveyor General."}) &&
                  std::all_of(en.begin(), en.end(), [](const auto& e) { return e.folio.empty(); }),
              [&] { std::string x; for (const auto& t : w) x += t + " | "; return x; }());
        const auto frames_before = d.frames_on(d.as<core::GenList>("list.2")->placed_on).size();
        check("built: one step", L::build_list(j, d, "list.2"));
        const auto* l = d.as<core::GenList>("list.2");
        const auto* st = d.as<core::Story>(l->story);
        check("the Sources typed on its page: that story is it -- sorted in place, no frame on top",
              l->story == "story.4" && d.frames_on(l->placed_on).size() == frames_before);
        check("  sorted, each once, the words alone", st && st->paras.size() == 5 && st->paras[0].text == "Almanac of the Outer Ports, 3rd ed." &&
                                                     st->paras[2].text == "The Book of Tides.");
        check("  its italics kept (the Almanac's title in Emphasis)", st && !st->paras.empty() && !st->paras[0].runs.empty() &&
                                                                       st->paras[0].runs[0].style == "style.5");
        check("  in the sources' own style", st && std::all_of(st->paras.begin(), st->paras.end(), [](const core::Para& p) { return p.style == "style.4"; }));
        check("built: current", stale_of().empty(), stale_of());
        check("again: nothing", !L::build_list(j, d, "list.2"));
        D::edit_text(j, d, "story.4", {0, 0}, {0, 0}, "Zed's Charts.\n");
        check("one typed at the top: out of date", !stale_of().empty(), stale_of());
        L::build_list(j, d, "list.2");
        check("rebuilt: sorted to its place", d.as<core::Story>("story.4")->paras.back().text == "Zed's Charts." && stale_of().empty());
        {   // The inspector offers Build for it.
            const auto o = core::project(d, "", nullptr, nullptr, nullptr);
            bool build = false;
            for (const auto& it : o.items())
                if (it.id == "list.2") for (const auto& f : it.fields) build = build || (f.edit == "genlist" && !f.choices.empty());
            check("the inspector: Rebuild offered", build);
        }
        {   // One on a page without its sources: a frame of its own, gathering them from where they are.
            const auto other = d.pages()[d.pages().size() - 2];
            const auto bl = D::add_list(j, d, core::ListKind::Bibliography, "Works Cited", other);
            {
                core::GenList g = *d.as<core::GenList>(bl);
                g.from_styles = {"style.4"};
                j.run("styles", {bl}, [&] { d.put(bl, g); });
            }
            check("elsewhere: built into a frame of its own", L::build_list(j, d, bl) && !d.as<core::GenList>(bl)->story.empty() &&
                                                            d.as<core::GenList>(bl)->story != "story.4");
            const auto& w2 = d.as<core::Story>(d.as<core::GenList>(bl)->story)->paras;
            check("  the same sources, each once (its own copies not doubled)", w2.size() == 6 && w2[0].text == "Almanac of the Outer Ports, 3rd ed.");
            check("  built again: nothing", !L::build_list(j, d, bl));
        }
        while (j.can_undo()) j.undo();
        check("undo all", d.strays() == 0 && d.as<core::GenList>("list.2")->story.empty());
    }

    {
        std::cout << "\n-- citations (s141) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);   // story.4: Almanac, Harbor Registers, Letters -- Sources (list.2 gathers style.4)
        const auto& src = *d.as<core::Story>("story.4");
        check("sources numbered in their story", core::source_number(d, src, 0) == 1 && core::source_number(d, src, 2) == 3 &&
                                                 core::source_number(d, *d.as<core::Story>("story.1"), 1) == 0);
        check("a citation of what isn't a source: nothing", !D::insert_citation(j, d, "story.1", {1, 0}, "story.1", 2) && j.size() == 0);
        check("a citation: one step", D::insert_citation(j, d, "story.1", {1, 3}, "story.4", 2) && j.size() == 1);
        const auto shown = [&](int p) {
            const auto r = L::resolve_story(d, "story.1", "");
            return r.any ? r.text.paras[static_cast<std::size_t>(p)].text : std::string{};
        };
        check("  shows [3]", shown(1).rfind("The[3]", 0) == 0, shown(1).substr(0, 12));
        // A source typed at the top, the bibliography rebuilt: sorted, the cited one is 4th now.
        D::edit_text(j, d, "story.4", {0, 0}, {0, 0}, "Baltic Pilot.\n");
        L::build_list(j, d, "list.2");
        check("sorted again: the citation follows its source -- [4]", shown(1).rfind("The[4]", 0) == 0 &&
                                                                      d.as<core::Story>("story.4")->paras[3].text.find("Letters of the Surveyor General.") != std::string::npos,
              shown(1).substr(0, 12));
        check("  its anchor kept by the rebuild", !d.as<core::Story>("story.4")->paras[3].anchors.empty());
        {
            L::FlowCache f;
            bool stale = false;
            for (const auto& x : L::stale_lists(d, f)) stale = stale || x.list == "list.2";
            check("  a cited source (its anchor) doesn't make it out of date", !stale);
        }
        {   // A bibliography elsewhere: copies come without the anchor (one place, one id).
            const auto bl = D::add_list(j, d, core::ListKind::Bibliography, "Works Cited", d.pages()[d.pages().size() - 2]);
            core::GenList g = *d.as<core::GenList>(bl);
            g.from_styles = {"style.4"};
            j.run("styles", {bl}, [&] { d.put(bl, g); });
            L::build_list(j, d, bl);
            int with = 0;
            for (const auto& p : d.as<core::Story>(d.as<core::GenList>(bl)->story)->paras)
                for (const auto& a : p.anchors) with += !a.empty();
            check("a copy elsewhere: no anchor ids copied", with == 0);
        }
        // The source deleted: the citation broken, Preflight says so.
        D::edit_text(j, d, "story.4", {3, 0}, {3, static_cast<int>(d.as<core::Story>("story.4")->paras[3].text.size())}, "", "Delete", "");
        D::edit_text(j, d, "story.4", {2, static_cast<int>(d.as<core::Story>("story.4")->paras[2].text.size())}, {3, 0}, "", "Delete", "");
        L::FlowCache fl;
        int xr = 0;
        for (const auto& i : L::preflight_issues(d, fl, {})) xr += i.kind == "xref";
        check("its source gone: ??, and a Preflight warning", shown(1).rfind("The??", 0) == 0 && xr == 1, shown(1).substr(0, 12));
        {
            core::Doc e;
            std::string err;
            check("json: a citation kept", core::from_json(e, core::to_json(d), &err) && e.records() == d.records(), err);
        }
        while (j.can_undo()) j.undo();
        check("undo all", d.strays() == 0 && d.as<core::Story>("story.1")->paras[1].refs.empty());
    }

    {
        std::cout << "\n-- the bibliography in cited order (s142) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);   // Sources: Almanac (1), Harbor Registers (2), Letters (3)
        const auto shown = [&](int p) {
            const auto r = L::resolve_story(d, "story.1", "");
            return r.any ? r.text.paras[static_cast<std::size_t>(p)].text : std::string{};
        };
        // Letters cited first (paragraph 1), then Almanac (paragraph 2); Harbor never.
        D::insert_citation(j, d, "story.1", {1, 0}, "story.4", 2);
        D::insert_citation(j, d, "story.1", {2, 0}, "story.4", 0);
        check("alphabetical: [3] then [1]", shown(1).rfind("[3]", 0) == 0 && shown(2).rfind("[1]", 0) == 0, shown(1).substr(0, 5) + shown(2).substr(0, 5));
        const auto rows = [&] {
            std::map<std::string, std::string> m;
            const auto o = core::project(d, "", nullptr, nullptr, nullptr);
            for (const auto& it : o.items()) if (it.id == "list.2") for (const auto& f : it.fields) m[f.label] = f.value;
            return m;
        };
        check("the inspector: Order Alphabetical", rows()["Order"] == "Alphabetical");
        L::build_list(j, d, "list.2");   // written alphabetical first
        check("first cited: one step", D::set_list_cite_order(j, d, "list.2", true) && rows()["Order"] == "First cited");
        {
            L::FlowCache f;
            std::string why;
            for (const auto& x : L::stale_lists(d, f)) if (x.list == "list.2") why = x.why;
            check("  out of date, saying why: the order", why == "Not in the order first cited", why);
        }
        check("  the same again: nothing", !D::set_list_cite_order(j, d, "list.2", true));
        L::build_list(j, d, "list.2");
        const auto& w = d.as<core::Story>("story.4")->paras;
        const auto has = [](const core::Para& p, const char* t) { return p.text.find(t) != std::string::npos; };
        check("rebuilt: Letters, Almanac, then the uncited Harbor Registers",
              w.size() == 3 && has(w[0], "Letters") && has(w[1], "Almanac") && has(w[2], "Harbor"));
        check("  the citations renumbered: [1] then [2]", shown(1).rfind("[1]", 0) == 0 && shown(2).rfind("[2]", 0) == 0,
              shown(1).substr(0, 5) + shown(2).substr(0, 5));
        {
            L::FlowCache f;
            bool stale = false;
            for (const auto& x : L::stale_lists(d, f)) stale = stale || x.list == "list.2";
            check("  current", !stale);
        }
        // A citation of Harbor added before the others: it moves to the front on a rebuild.
        D::insert_citation(j, d, "story.1", {0, 0}, "story.4", 2);
        L::build_list(j, d, "list.2");
        check("a new first citation: that source first", has(d.as<core::Story>("story.4")->paras[0], "Harbor") && shown(0).rfind("[1]", 0) == 0,
              shown(0).substr(0, 5));
        {
            core::Doc e;
            std::string err;
            check("json: the order kept", core::from_json(e, core::to_json(d), &err) && e.records() == d.records(), err);
        }
        while (j.can_undo()) j.undo();
        check("undo all", d.strays() == 0 && !d.as<core::GenList>("list.2")->cite_order);
    }

    {
        std::cout << "\n-- citations: locators, several sources (s143) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);   // Sources 1-3 in story.4
        const auto shown = [&](int p) {
            const auto r = L::resolve_story(d, "story.1", "");
            return r.any ? r.text.paras[static_cast<std::size_t>(p)].text : std::string{};
        };
        check("a locator: \"[2, p. 14]\" (no-break spaces)", D::insert_citation(j, d, "story.1", {1, 0}, {{"story.4", 1}}, " p. 14 ") &&
                                                         shown(1).rfind("[2,\u00a0p.\u00a014]", 0) == 0,
              shown(1).substr(0, 12));
        const auto n0 = j.size();
        check("several, out of order: \"[1, 3]\" -- one step", D::insert_citation(j, d, "story.1", {2, 0}, {{"story.4", 2}, {"story.4", 0}}) &&
                                                                   j.size() == n0 + 1 && shown(2).rfind("[1, 3]", 0) == 0, shown(2).substr(0, 10));
        check("none a source: nothing", !D::insert_citation(j, d, "story.1", {3, 0}, {{"story.1", 2}}) && j.size() == n0 + 1);
        check("one added to it: \"[1\u20133]\"", D::add_cite_source(j, d, "story.1", 2, 0, "story.4", 1) && shown(2).rfind("[1\u20133]", 0) == 0,
              shown(2).substr(0, 10));
        check("  the same again: nothing", !D::add_cite_source(j, d, "story.1", 2, 0, "story.4", 1));
        check("  to what isn't a citation: nothing", [&] {
            D::insert_ref_to(j, d, "story.1", {3, 0}, "story.1", 4, core::RefShow::Page);
            return !D::add_cite_source(j, d, "story.1", 3, 0, "story.4", 1);
        }());
        check("the locator edited (Apply), its sources kept", D::set_ref(j, d, "story.1", 2, 0, core::RefShow::Cite, "ch. 3") &&
                                                              shown(2).rfind("[1\u20133,\u00a0ch.\u00a03]", 0) == 0, shown(2).substr(0, 16));
        {
            core::Doc e;
            std::string err;
            check("json: locator and sources kept", core::from_json(e, core::to_json(d), &err) && e.records() == d.records(), err);
        }
        // One of its sources gone: "??" with the rest, and a Preflight warning.
        D::edit_text(j, d, "story.4", {1, 0}, {2, 0}, "", "Delete", "");   // Harbor Registers (2) gone; Letters is 2 now
        L::FlowCache fl;
        int xr = 0;
        for (const auto& i : L::preflight_issues(d, fl, {})) xr += i.kind == "xref";
        check("a source gone: the rest renumbered, ?? for it, a warning", shown(2).rfind("[1, 2, ??,\u00a0ch.\u00a03]", 0) == 0 && xr >= 2,
              shown(2).substr(0, 20) + " / " + std::to_string(xr));
        while (j.can_undo()) j.undo();
        check("undo all", d.strays() == 0 && d.as<core::Story>("story.1")->paras[2].refs.empty());
    }

    {
        std::cout << "\n-- the sources, listed (s144) --\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        L::FlowCache fl;
        auto rows = L::source_list(d, fl);
        check("the sample's three sources, numbered, none cited", rows.size() == 3 && rows[0].number == 1 && rows[2].number == 3 &&
                                                                   rows[0].words.rfind("Almanac", 0) == 0 &&
                                                                   std::all_of(rows.begin(), rows.end(), [](const auto& r) { return r.cites == 0; }));
        D::insert_citation(j, d, "story.1", {1, 0}, "story.4", 2);   // page 1
        D::insert_citation(j, d, "story.1", {5, 0}, {{"story.4", 2}, {"story.4", 0}});   // Chapter Two's page: both
        const auto folio = [&](int p) {
            const auto c = L::caret_at(d, "story.1", fl.get(d, "story.1"), {p, 0});
            return c.ok ? d.folio(d.as<core::Frame>(c.frame)->owner) : std::string{};
        };
        rows = L::source_list(d, fl);
        check("Letters: cited twice, its pages in order", rows.size() == 3 && rows[2].cites == 2 &&
                                                          rows[2].pages == (std::vector<std::string>{folio(1), folio(5)}));
        check("Almanac: once (in a citation of two)", rows[0].cites == 1 && rows[0].pages == std::vector<std::string>{folio(5)});
        check("Harbor: not cited", rows[1].cites == 0 && rows[1].pages.empty());
        check("the words without the anchor's mark", rows[2].words == "Letters of the Surveyor General.");
    }

    {
        std::cout << "\n-- the bare document Close Project leaves (s146) --\n";
        core::Doc d;
        core::NewDoc n;
        n.title = "Untitled";
        n.pages = 1;
        n.text_frames = false;
        n.folios = false;
        check("made", core::new_document(d, n));
        check("one page, one master, one section", d.pages().size() == 1 && d.ids_of<core::Master>().size() == 1 &&
                                                   d.ids_of<core::Section>().size() == 1);
        check("no frames, no stories", d.ids_of<core::Frame>().empty() && d.ids_of<core::Story>().empty());
        check("the colours and the base styles", d.ids_of<core::Swatch>().size() >= 2 && !d.ids_of<core::Style>().empty());
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        check("a page can be added after it", !core::doc::add_pages(j, d, d.pages().front(), 1, d.ids_of<core::Master>().front()).empty() &&
                                              d.pages().size() == 2);
    }

    {
        std::cout << "\n-- masters own their page settings (s147) --\n";
        using core::kPt;
        namespace D = core::doc;
        namespace L = composix::layout;
        namespace fs = std::filesystem;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::NewDoc n;
        n.pages = 6;
        n.text_frames = false;
        core::new_document(d, n);
        const auto body = d.ids_of<core::Master>().front();
        // A cover master: single, its own size and bleed, on page 1.
        core::Master cover = *d.as<core::Master>(body);
        cover.prefix = "C";
        cover.name = "Cover";
        cover.pos = 0.5;
        j.run("cover", {}, [&]() -> std::vector<core::Doc::Id> { d.put("master.cover", cover); return {"master.cover"}; });
        {
            auto pg = *d.as<core::Page>(d.pages().front());
            pg.master = "master.cover";
            j.run("p1", {d.pages().front()}, [&] { d.put(d.pages().front(), pg); });
        }
        check("its size: one step, its pages follow", D::set_master_size(j, d, "master.cover", 6.25 * kPt, 9.25 * kPt) &&
                                                       d.as<core::Page>(d.pages().front())->w == 6.25 * kPt &&
                                                       d.as<core::Page>(d.pages()[1])->w == 6 * kPt);
        check("  the same again: nothing", !D::set_master_size(j, d, "master.cover", 6.25 * kPt, 9.25 * kPt));
        {   // A page of its own size (a foldout) keeps it.
            auto pg = *d.as<core::Page>(d.pages()[3]);
            pg.w = 11.75 * kPt;
            j.run("fold", {d.pages()[3]}, [&] { d.put(d.pages()[3], pg); });
            D::set_master_size(j, d, body, 5.5 * kPt, 8.5 * kPt);
            check("  a page of its own size keeps it", d.as<core::Page>(d.pages()[3])->w == 11.75 * kPt &&
                                                       d.as<core::Page>(d.pages()[2])->w == 5.5 * kPt);
        }
        check("single cover, facing body: page 1 alone, then pairs", D::set_master_facing(j, d, "master.cover", false) &&
                                                                    d.side_of(d.pages().front()) == core::Side::Any &&
                                                                    core::spreads(d).front().size() == 1 && core::spreads(d)[1].size() == 2);
        check("bleed: each master's", D::set_master_bleed(j, d, "master.cover", 18) && core::bleed_of(d, d.pages().front()) == 18 &&
                                      core::bleed_of(d, d.pages()[1]) == n.bleed);
        {   // The PDF: the cover's sheet the larger bleed.
            const auto base = fs::temp_directory_path() / "composix_s147";
            fs::create_directories(base);
            const L::ImageSource src = [](const core::Asset&, double, double) -> cairo_surface_t* { return nullptr; };
            L::PdfOptions o;
            o.compress = false;
            o.bleed = true;
            L::PdfReport rep;
            std::string err;
            const auto out = (base / "p.pdf").string();
            const bool ok = L::export_print_pdf(d, out, o, src, &rep, &err);
            std::ifstream in(out, std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            char want[64];
            std::snprintf(want, sizeof want, "/BleedBox [0 0 %g %g]", 6.25 * kPt + 36, 9.25 * kPt + 36);
            check("  the print PDF: the cover sheet bled 1/4 in", ok && ss.str().find(want) != std::string::npos, err);
            std::error_code ec;
            if (!std::getenv("KEEP_PDF")) fs::remove_all(base, ec);
        }
        {   // A file from before: masters without bleed / facing take the document's.
            // (every "bleed" / "facing" key out -- never an object's first -- the document's too, so defaults then)
            const std::string js = std::regex_replace(core::to_json(d), std::regex(R"re(,\s*"(bleed|facing)"\s*:\s*[^,}\]]+)re"), "");
            core::Doc e;
            std::string err;
            check("old file: loads", core::from_json(e, js, &err), err);
            const auto* m = e.as<core::Master>("master.cover");
            check("  its masters: the document's bleed and facing", m && m->bleed == e.settings().bleed && m->facing == e.settings().facing);
        }
        while (j.can_undo()) j.undo();
        check("undo all", d.strays() == 0 && !d.as<core::Master>("master.cover"));
    }

    {
        std::cout << "\n-- masters before pages; a grid of any measure (s150) --\n";
        namespace D = core::doc;
        int im = -1, ip = -1, i = 0;
        for (const auto& s : core::segments()) {
            if (s.id == core::Segment::Masters) im = i;
            if (s.id == core::Segment::Pages) ip = i;
            ++i;
        }
        check("the navigator: Masters before Pages", im >= 0 && ip >= 0 && im < ip);
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto mid = d.ids_of<core::Master>().front();
        auto grid_field = [&]() -> std::optional<core::Field> {
            const auto o = core::project(d, "");
            for (const auto& f : o.find(mid)->fields)
                if (f.edit == "mgrid") return f;
            return std::nullopt;
        };
        const auto g0 = grid_field();
        check("the grid: typed, its presets beside", g0 && g0->unit > 0 && g0->hi >= 72 && !g0->choices.empty());
        D::edit_masters(j, d, {mid}, "Baseline grid", [](core::Master& m) { m.grid = 13.5; });
        const auto g1 = grid_field();
        check("a measure not in the presets: kept and shown", d.as<core::Master>(mid)->grid == 13.5 && g1 && g1->value == "13.5 pt");
        j.undo();
        check("undo: the grid back", d.as<core::Master>(mid)->grid != 13.5);
    }

    {
        std::cout << "\n-- a facing master opens as its left / right spread (s150) --\n";
        namespace D = core::doc;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        core::Doc::Id fm, sm;
        for (const auto& m : d.ids_of<core::Master>()) {
            const auto* M = d.as<core::Master>(m);
            if (M->facing && M->folds.empty() && fm.empty()) fm = m;
        }
        check("the sample has a facing master", !fm.empty());
        const auto pl = core::layout_pages(d, {fm});
        check("two panels: Left then Right, meeting at the spine",
              pl.size() == 2 && pl[0].id == fm && pl[1].id == fm && pl[0].side == core::Side::Left &&
                  pl[1].side == core::Side::Right && std::abs(pl[0].r.r() - pl[1].r.x) < 1e-9 && pl[0].master && pl[1].master);
        D::set_master_facing(j, d, fm, false);
        check("single: one panel", core::layout_pages(d, {fm}).size() == 1 && core::layout_pages(d, {fm})[0].side == core::Side::Any);
        j.undo();
        // Its items: one for each side, one for both.
        const auto& M = *d.as<core::Master>(fm);
        const auto mk = [&](core::Side side, double x) {
            core::Frame f;
            f.owner = fm;
            f.kind = core::FrameKind::Shape;
            f.x = x; f.y = 20; f.w = 30; f.h = 30;
            f.side = side;
            const auto id = "frame.s150." + std::to_string(static_cast<int>(side));
            j.run("item", {}, [&]() -> std::vector<core::Doc::Id> { d.put(id, f); return {id}; });
            return id;
        };
        const auto fl = mk(core::Side::Left, 40), fr = mk(core::Side::Right, M.w - 70), fa = mk(core::Side::Any, M.w / 2 - 15);
        const auto pl2 = core::layout_pages(d, {fm});
        check("on the left panel: left and both, not right",
              core::on_panel(d, pl2[0], fl) && core::on_panel(d, pl2[0], fa) && !core::on_panel(d, pl2[0], fr));
        check("on the right panel: right and both, not left",
              core::on_panel(d, pl2[1], fr) && core::on_panel(d, pl2[1], fa) && !core::on_panel(d, pl2[1], fl));
        const double cy = 35;
        check("a both-sides item picked on either panel",
              core::hit_frame(d, pl2, pl2[0].r.x + M.w / 2, cy, 0) == fa && core::hit_frame(d, pl2, pl2[1].r.x + M.w / 2, cy, 0) == fa);
        check("a right item not picked where it would be on the left",
              core::hit_frame(d, pl2, pl2[0].r.x + M.w - 55, cy, 0).empty() && core::hit_frame(d, pl2, pl2[1].r.x + M.w - 55, cy, 0) == fr);
        const auto all = core::frames_in(d, pl2, pl2[0].r.unite(pl2[1].r));
        check("a marquee over the spread: each item once",
              std::count(all.begin(), all.end(), fa) == 1 && std::count(all.begin(), all.end(), fl) == 1 &&
                  std::count(all.begin(), all.end(), fr) == 1);
        check("the right item laid out on the right panel", core::frame_in_layout(d, pl2, fr).x == pl2[1].r.x + M.w - 70);
        const auto ml = core::margin_rect(d, fm, core::Side::Left), mr = core::margin_rect(d, fm, core::Side::Right);
        check("margins mirrored: the inside by the spine on both",
              std::abs(ml.x - M.margins.outside) < 1e-9 && std::abs(mr.x - M.margins.inside) < 1e-9 &&
                  std::abs((M.w - ml.r()) - M.margins.inside) < 1e-9);
        check("the master's own margins, no side: as before (a right page)",
              std::abs(core::margin_rect(d, fm).x - M.margins.inside) < 1e-9);
        while (j.can_undo()) j.undo();
        check("undo all", d.strays() == 0);
    }

    {
        std::cout << "\n-- a master's inspector holds its page settings (s148) --\n";
        namespace D = core::doc;
        using core::kPt;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto mid = d.ids_of<core::Master>().front();
        const auto o = core::project(d, "");
        std::map<std::string, std::string> mf, df;
        for (const auto& f : o.find(mid)->fields) mf[f.edit.empty() ? f.label : f.edit] = f.value;
        for (const auto& f : o.find("doc")->fields) df[f.edit.empty() ? f.label : f.edit] = f.value;
        check("the master: name, size, bleed, pages, columns, margins", mf.count("mname") && mf.count("msize") && mf.count("mbleed") &&
                                                                         mf.count("mfacing") && mf.count("mcolumns") && mf.count("mtop"));
        check("the document: no trim, bleed or pages; title, units, footnotes stay",
              !df.count("dtrim") && !df.count("dbleed") && !df.count("dfacing") && df.count("dtitle") && df.count("dunits") &&
                  df.count("dnote_restart"));
        check("the document item: \"Document\"", o.find("doc")->title == "Document");
        // A cover master 8 x 10: its page's size presets start from it; a page added with it takes its size.
        core::Master cover = *d.as<core::Master>(mid);
        cover.w = 8 * kPt;
        cover.h = 10 * kPt;
        j.run("cover", {}, [&]() -> std::vector<core::Doc::Id> { d.put("master.cover", cover); return {"master.cover"}; });
        const auto made = D::add_pages(j, d, d.pages().front(), 1, "master.cover");
        check("a page added with it: its size", made.size() == 1 && d.as<core::Page>(made[0])->w == 8 * kPt && d.as<core::Page>(made[0])->h == 10 * kPt);
        const auto pr = D::size_presets(d, made[0]);
        check("its size presets: from its master (8 x 10, foldouts of it)", pr.size() == 3 && pr[0].w == 8 * kPt && pr[1].folds.front() == 8 * kPt);
        check("rename: one step", D::edit_masters(j, d, {"master.cover"}, "Master name", [](core::Master& m) { m.name = "Cover"; }) &&
                                  d.as<core::Master>("master.cover")->name == "Cover");
    }

    {
        std::cout << "\n-- a master's mock-up (s149) --\n";
        namespace L = composix::layout;
        using core::kPt;
        core::Doc d;
        core::load_sample(d);
        L::FlowCache fl;
        const L::ImageSource src = [](const core::Asset&, double, double) -> cairo_surface_t* { return nullptr; };
        const auto body = std::string("master.2");
        check("its size: a facing master is a spread", std::abs(L::mockup_size(d, body).w - 2 * d.as<core::Master>(body)->w) < 1e-9);
        check("a foldout master: one sheet", [&] {
            for (const auto& mid : d.ids_of<core::Master>())
                if (!d.as<core::Master>(mid)->folds.empty()) return std::abs(L::mockup_size(d, mid).w - d.as<core::Master>(mid)->w) < 1e-9;
            return true;
        }());
        // Drawn: the margin's magenta where the right page's top margin is (A-Front: no guides over it).
        const int W = 432, H = 324;   // 2 x 6 in by 9 in at 36 px an inch: exact fit
        cairo_surface_t* img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
        cairo_t* cr = cairo_create(img);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_paint(cr);
        const std::string front = "master.1";
        L::draw_master_mockup(cr, d, front, fl, src, 0, 0, W, H);
        cairo_surface_flush(img);
        const auto* m = d.as<core::Master>(front);
        const double sc = W / L::mockup_size(d, front).w;
        const int mx = static_cast<int>(W / 2 + (m->margins.inside + 20) * sc), my = static_cast<int>(std::round(m->margins.top * sc));
        const auto px = [&](int x, int y) {
            const auto* row = cairo_image_surface_get_data(img) + y * cairo_image_surface_get_stride(img);
            const std::uint32_t v = reinterpret_cast<const std::uint32_t*>(row)[x];
            return v;
        };
        bool magenta = false;
        for (int dy = -1; dy <= 1; ++dy) {
            const auto v = px(mx, my + dy);
            const int r = (v >> 16) & 0xff, g = (v >> 8) & 0xff, b = v & 0xff;
            magenta = magenta || (r > 180 && g < 150 && b > 150);
        }
        check("drawn: the right page's top margin in magenta", magenta);
        cairo_destroy(cr);
        cairo_surface_destroy(img);
        // A master no page uses on one side (the bare document's left): drawn all the same.
        core::Doc bare;
        core::NewDoc n;
        n.pages = 1;
        n.text_frames = false;
        core::new_document(bare, n);
        cairo_surface_t* img2 = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 100, 80);
        cairo_t* cr2 = cairo_create(img2);
        L::FlowCache f2;
        L::draw_master_mockup(cr2, bare, bare.ids_of<core::Master>().front(), f2, src, 0, 0, 100, 80);
        check("a side no page uses: drawn from a page made for it, nothing kept", bare.pages().size() == 1 && cairo_status(cr2) == CAIRO_STATUS_SUCCESS);
        cairo_destroy(cr2);
        cairo_surface_destroy(img2);
        {   // The tiles: masters, sections and the document show the mock-up.
            const auto o = core::project(d, "");
            check("tiles: a master's, a section's, the document's are its master's mock-up",
                  o.find(body)->thumb.page == "mockup:" + body && o.find("doc")->thumb.page.rfind("mockup:", 0) == 0 &&
                      o.find(d.ids_of<core::Section>().front())->thumb.page.rfind("mockup:", 0) == 0);
        }
    }


    // ── s171: colour libraries -- the formats, My Library, the system's palettes ──
    {
        std::cout << "--- s171 colour libraries ---\n";
        namespace SW = core::swatches;
        namespace D = core::doc;
        using CM = core::ColorModel;
        const auto near = [](std::uint32_t a, std::uint32_t b, int tol) {
            for (int sh : {16, 8, 0})
                if (std::abs(static_cast<int>((a >> sh) & 0xFF) - static_cast<int>((b >> sh) & 0xFF)) > tol) return false;
            return true;
        };
        check("Lab: white, black, mid grey", SW::lab_to_rgb(100, 0, 0) == 0xFFFFFF && SW::lab_to_rgb(0, 0, 0) == 0 &&
                                               near(SW::lab_to_rgb(50, 0, 0), 0x777777, 2));
        // ASE: written, read back -- CMYK, RGB, Gray, a spot, a name past ASCII.
        std::vector<core::Swatch> four{
            {"Teal", 0, CM::CMYK, {80, 0, 30, 10}},
            {"Brick", 0, CM::RGB, {170, 60, 40, 0}},
            {"Half", 0, CM::Gray, {50, 0, 0, 0}},
            {"Café Ü", 0, CM::Spot, {0, 60, 100, 0}, "Café Ü"}};
        for (auto& c : four) c.preview = D::swatch_preview(c);
        const std::string ase = SW::to_ase(four);
        check("ASE: sniffed", SW::format_of("x.bin", ase) == SW::Format::ASE);
        const auto a = SW::parse(ase, SW::Format::ASE);
        bool ase_ok = a && a->colors.size() == 4;
        for (std::size_t i = 0; ase_ok && i < 4; ++i) {
            const auto& g = a->colors[i];
            ase_ok = g.name == four[i].name && g.model == four[i].model;
            for (int k = 0; k < 4; ++k) ase_ok = ase_ok && std::abs(g.v[static_cast<std::size_t>(k)] - four[i].v[static_cast<std::size_t>(k)]) < 0.01;
        }
        check("ASE: CMYK, RGB, Gray and a spot back as they went", ase_ok);
        check("ASE: the spot keeps its ink", ase_ok && a->colors[3].spot == "Café Ü");
        check("ASE: cut short -- refused or what was whole", !SW::parse("ASEF", SW::Format::ASE) || SW::parse("ASEF", SW::Format::ASE)->colors.empty());
        {   // An ASE colour in Lab, by hand: L 0.5 (50), a 0, b 0.
            std::string b = "ASEF";
            const auto put = [&](std::uint32_t v, int n) { for (int i = n - 1; i >= 0; --i) b += static_cast<char>((v >> (8 * i)) & 0xFF); };
            const auto f32 = [&](float f) { std::uint32_t v; std::memcpy(&v, &f, 4); put(v, 4); };
            put(1, 2); put(0, 2); put(3, 4);
            put(0xC001, 2); put(8, 4); put(3, 2); put('G', 2); put('1', 2); put(0, 2);   // a group
            put(1, 2); put(2 + 4 + 4 + 12 + 2, 4); put(2, 2); put('L', 2); put(0, 2);
            b += "LAB "; f32(0.5f); f32(0.f); f32(0.f); put(2, 2);
            put(0xC002, 2); put(0, 4);
            const auto lab = SW::parse(b, SW::Format::ASE);
            check("ASE: a Lab colour inside a group -- RGB, its look", lab && lab->colors.size() == 1 && lab->colors[0].model == CM::RGB &&
                                                                    lab->colors[0].name == "L" && near(lab->colors[0].preview, 0x777777, 2));
        }
        // GPL.
        const std::string gpl = "GIMP Palette\nName: Earth\nColumns: 4\n# a comment\n255   0   0\tRed\n 0 128 0 Leaf Green\n10 20 30\n";
        const auto g = SW::parse(gpl, SW::Format::GPL);
        check("GPL: sniffed, named, three colours", SW::format_of("p.txt", gpl) == SW::Format::GPL && g && g->name == "Earth" &&
                                                   g->colors.size() == 3 && g->colors[1].name == "Leaf Green" && g->colors[1].v[1] == 128 &&
                                                   g->colors[2].name == "#0A141E");
        {
            const auto back = SW::parse(SW::to_gpl("Mine", four), SW::Format::GPL);
            check("GPL: written -- the screen colours, named", back && back->name == "Mine" && back->colors.size() == 4 &&
                                                               back->colors[0].preview == four[0].preview && back->colors[3].name == four[3].name);
        }
        // LibreOffice .soc.
        const std::string soc = R"(<?xml version="1.0" encoding="UTF-8"?>
<ooo:color-table xmlns:office="urn:oasis:names:tc:opendocument:xmlns:office:1.0" xmlns:draw="urn:oasis:names:tc:opendocument:xmlns:drawing:1.0">
<draw:color draw:name="Dark Red 2" draw:color="#a51d2d"/>
<draw:color draw:name="Salt &amp; Pepper" draw:color="#808080"/>
</ooo:color-table>)";
        const auto so = SW::parse(soc, SW::format_of("standard.soc", soc));
        check("SOC: two colours, entities decoded", so && so->colors.size() == 2 && so->colors[0].preview == 0xA51D2D &&
                                                     so->colors[1].name == "Salt & Pepper");
        // Scribus: old (CMYK="#..") and new (Space=) attributes, a spot.
        const std::string scx = R"(<?xml version="1.0"?><SCRIBUSCOLORS Name="Press">
<COLOR NAME="Cyan" CMYK="#ff000000" Spot="0"/>
<COLOR NAME="Web" RGB="#336699"/>
<COLOR Space="CMYK" NAME="Gold Ink" C="0" M="20" Y="80" K="10" Spot="1" Register="0"/>
</SCRIBUSCOLORS>)";
        const auto sc = SW::parse(scx, SW::format_of("press.xml", scx));
        check("Scribus: named, CMYK by hex and by parts, RGB, a spot", sc && sc->name == "Press" && sc->colors.size() == 3 &&
                                                                        sc->colors[0].model == CM::CMYK && std::abs(sc->colors[0].v[0] - 100) < 0.01 &&
                                                                        sc->colors[1].preview == 0x336699 && sc->colors[2].model == CM::Spot &&
                                                                        sc->colors[2].spot == "Gold Ink" && sc->colors[2].v[2] == 80);
        // Photoshop .aco: version 1 then version 2 (named).
        {
            std::string b;
            const auto put = [&](std::uint32_t v, int n) { for (int i = n - 1; i >= 0; --i) b += static_cast<char>((v >> (8 * i)) & 0xFF); };
            const auto colours = [&] {
                put(0, 2); put(65535, 2); put(0, 2); put(0, 2); put(0, 2);           // RGB red
                put(2, 2); put(0, 2); put(65535, 2); put(65535, 2); put(65535, 2);   // CMYK: 100 C (inverted)
            };
            put(1, 2); put(2, 2); colours();
            put(2, 2); put(2, 2);
            put(0, 2); put(65535, 2); put(0, 2); put(0, 2); put(0, 2); put(4, 4); for (char c : std::string("Red")) put(static_cast<unsigned char>(c), 2); put(0, 2);
            put(2, 2); put(0, 2); put(65535, 2); put(65535, 2); put(65535, 2); put(5, 4); for (char c : std::string("Cyan")) put(static_cast<unsigned char>(c), 2); put(0, 2);
            const auto ac = SW::parse(b, SW::format_of("old.aco", b));
            check("ACO: version 2's names, RGB and inverted CMYK", ac && ac->colors.size() == 2 && ac->colors[0].name == "Red" &&
                                                                   ac->colors[0].preview == 0xFF0000 && ac->colors[1].name == "Cyan" &&
                                                                   std::abs(ac->colors[1].v[0] - 100) < 0.01 && ac->colors[1].v[1] < 0.01);
        }
        check("unknown bytes: refused", !SW::parse("hello", SW::format_of("x.txt", "hello")));
        // Files, and My Library (lossless).
        const auto tmp = std::filesystem::temp_directory_path() / ("cx_swatch_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::remove_all(tmp);
        {
            const std::string f = (tmp / "out" / "set.ase").string();
            check("write ASE, read it: named by the file", SW::write_file(f, "Set", four, SW::Format::ASE) &&
                                                          SW::read_file(f) && SW::read_file(f)->name == "set" && SW::read_file(f)->colors.size() == 4);
            check("SOC isn't written", !SW::write_file((tmp / "x.soc").string(), "x", four, SW::Format::SOC) && !SW::writable(SW::Format::SOC));
            auto mine = SW::load_mine((tmp / "lib" / "my-library.json").string());
            check("My Library: missing is empty, its kind", mine.colors.empty() && mine.kind == SW::LibKind::Mine && mine.name == "My Library");
            auto tagged = four;
            tagged[0].from = "Earth";
            check("merge: four in", SW::merge_into(mine, tagged) == 4);
            auto other = four[1];
            other.v = {10, 10, 10, 0};
            check("merge: the same skipped, a taken name numbered", SW::merge_into(mine, {four[0], other}) == 1 &&
                                                                     mine.colors.back().name == "Brick 2");
            check("My Library: saved", SW::save_mine(mine));
            const auto again = SW::load_mine(mine.path);
            check("My Library: back as it was, where it came from too", again.colors == mine.colors && again.colors[0].from == "Earth");
        }
        {   // The system's palettes: GIMP 2 and 3 shipping one file once, Inkscape's, Scribus's (a folder down), a stray XML left out.
            const auto data = tmp / "share", home = tmp / "home";
            const auto mk = [](const std::filesystem::path& p, const std::string& text) {
                std::filesystem::create_directories(p.parent_path());
                std::ofstream(p) << text;
            };
            mk(data / "gimp" / "2.0" / "palettes" / "Web.gpl", gpl);
            mk(data / "gimp" / "3.0" / "palettes" / "Web.gpl", gpl);
            mk(data / "inkscape" / "palettes" / "Tango.gpl", gpl);
            mk(data / "scribus" / "swatches" / "Press" / "press.xml", scx);
            mk(data / "scribus" / "swatches" / "readme.xml", "<html/>");
            mk(home / ".config" / "GIMP" / "3.0" / "palettes" / "Mine.gpl", gpl);
            const auto found = SW::system_palettes(home.string(), {data.string()});
            std::vector<std::string> got;
            for (const auto& f : found) got.push_back(f.source + ":" + std::filesystem::path(f.path).filename().string());
            check("system palettes: each once, by source, palettes only",
                  got == std::vector<std::string>{"GIMP:Mine.gpl", "GIMP:Web.gpl", "Inkscape:Tango.gpl", "Scribus:press.xml"});
        }
        std::filesystem::remove_all(tmp);
        // Into the document: one step, from noted; again is nothing; a taken name numbered; undo.
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::NewDoc n;
        n.pages = 1;
        core::new_document(d, n);
        const auto before = d.ids_of<core::Swatch>().size();
        const auto steps = j.size();
        const auto ids = D::add_library_swatches(j, d, {four[0], four[3], four[0]}, "Earth");
        check("library into the document: one step, two new, the repeat is the first",
              j.size() == steps + 1 && d.ids_of<core::Swatch>().size() == before + 2 && ids.size() == 3 && ids[0] == ids[2] &&
                  d.as<core::Swatch>(ids[0]) && d.as<core::Swatch>(ids[0])->from == "Earth" && d.as<core::Swatch>(ids[1])->model == CM::Spot);
        const auto again = D::add_library_swatches(j, d, {four[0]}, "Earth");
        check("library into the document: there already -- no step, its id", j.size() == steps + 1 && again[0] == ids[0]);
        auto teal2 = four[0];
        teal2.v = {0, 0, 0, 100};
        const auto renamed = D::add_library_swatches(j, d, {teal2}, "Other");
        check("library into the document: a taken name numbered", d.as<core::Swatch>(renamed[0])->name == "Teal 2" &&
                                                                  d.as<core::Swatch>(renamed[0])->from == "Other");
        {
            core::Doc e;
            check("from: saved with the document", core::from_json(e, core::to_json(d)) && e.as<core::Swatch>(ids[0]) &&
                                                   e.as<core::Swatch>(ids[0])->from == "Earth");
        }
        j.undo();
        j.undo();
        check("library into the document: undone", d.ids_of<core::Swatch>().size() == before);
        // s179: unused swatches, all at once (Black and Paper stay).
        {
            core::Doc e;
            core::DocJournal je{e};
            e.set_door([&je] { return je.open(); });
            core::load_sample(e);
            const auto before = e.ids_of<core::Swatch>().size();
            const std::size_t steps = je.size();
            D::add_library_swatches(je, e, {four[0], four[1]}, "Earth");   // two new, unused
            const auto un = D::unused_swatches(e);
            bool fine = un.size() >= 2;
            for (const auto& id : un) {
                const auto* s = e.as<core::Swatch>(id);
                fine = fine && s->name != "Black" && s->name != "Paper" && D::swatch_uses(e, id) == 0;
            }
            check("unused swatches: found, never Black or Paper", fine);
            const int n = D::delete_unused_swatches(je, e);
            check("unused swatches: gone in one step", n == static_cast<int>(un.size()) && je.size() == steps + 2 &&
                  D::unused_swatches(e).empty() && e.ids_of<core::Swatch>().size() == before + 2 - un.size());
            check("unused swatches: none -- no step", D::delete_unused_swatches(je, e) == 0 && je.size() == steps + 2);
            je.undo();
            check("unused swatches: undone", D::unused_swatches(e).size() == un.size());
        }
        // s177: a picture's colours.
        {
            const int W = 100, H = 100;
            std::vector<std::uint32_t> px(W * H);
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x)   // 60% sky (two near blues), 30% sand, 10% see-through
                    px[static_cast<std::size_t>(y * W + x)] = y < 60 ? (x % 2 ? 0xff3366ccu : 0xff3a6ad0u)
                                                             : y < 90 ? 0xffe0c080u : 0x00000000u;
            const auto pc = SW::picture_colors(px.data(), W, H, W);
            check("picture colours: the main ones, most first; near ones pooled; see-through not counted",
                  pc.size() == 2 && std::abs(pc[0].share - 60.0 / 90) < 0.01 && std::abs(pc[1].share - 30.0 / 90) < 0.01 &&
                  pc[1].rgb == 0xe0c080u && ((pc[0].rgb >> 16) & 0xff) >= 0x33 && ((pc[0].rgb >> 16) & 0xff) <= 0x3a);
            px[5] = 0xff00ff00u;   // one green pixel: under 1%
            check("picture colours: a speck left out", SW::picture_colors(px.data(), W, H, W).size() == 2);
            check("picture colours: at most asked", SW::picture_colors(px.data(), W, H, W, 1).size() == 1);
            std::vector<std::uint32_t> clear(16, 0u);
            check("picture colours: see-through all over -- none", SW::picture_colors(clear.data(), 4, 4, 4).empty());
            const auto pl = SW::picture_library("cove.png", pc);
            check("picture colours: a library of RGB swatches named by hex", pl.name == "cove.png" && pl.colors.size() == 2 &&
                  pl.colors[1].name == "#E0C080" && pl.colors[1].model == CM::RGB && pl.colors[1].preview == 0xe0c080u);
        }
        // s172: the Libraries window's shelf.
        {
            check("shelf: My Library's file", SW::mine_file("/d") == "/d/composix/swatches/my-library.json");
            core::AppSettings st;
            const bool a = SW::remember_imported(st, "/x/a.ase"), b = SW::remember_imported(st, "/x/b.gpl"),
                       again = SW::remember_imported(st, "/x/a.ase");
            check("shelf: imported remembered in order, once", a && b && !again &&
                  SW::imported(st) == std::vector<std::string>{"/x/a.ase", "/x/b.gpl"});
            check("shelf: forgotten", SW::forget_imported(st, "/x/a.ase") && !SW::forget_imported(st, "/x/a.ase") &&
                  SW::imported(st) == std::vector<std::string>{"/x/b.gpl"});
            SW::forget_imported(st, "/x/b.gpl");
            check("shelf: none left -- the setting goes", !st.count("colors.imported") && SW::imported(st).empty());
            check("shelf: save format by extension", SW::save_format("p.GPL") == SW::Format::GPL &&
                  SW::save_format("p.ase") == SW::Format::ASE && SW::save_format("p") == SW::Format::ASE &&
                  SW::save_format("p.json") == SW::Format::Composix);
            check("shelf: extension added when missing", SW::with_extension("/t/Earth", SW::Format::GPL) == "/t/Earth.gpl" &&
                  SW::with_extension("/t/Earth.ase", SW::Format::GPL) == "/t/Earth.ase" &&
                  SW::with_extension("/t/E.v2", SW::Format::ASE) == "/t/E.v2.ase");
            SW::Library lib;
            lib.colors = {four[0], four[1], four[2], four[3]};
            const int gone = SW::remove_at(lib, {3, 0, 3, 9});
            check("shelf: removed by place (repeats and strays ignored)", gone == 2 && lib.colors.size() == 2 &&
                  lib.colors[0].name == four[1].name && lib.colors[1].name == four[2].name);
            const auto dc = SW::document_colors(d);
            bool ordered = dc.size() == d.ids_of<core::Swatch>().size();
            for (std::size_t i = 1; i < dc.size(); ++i) ordered = ordered && dc[i - 1].pos <= dc[i].pos;
            check("shelf: the document's colours, in order", ordered && !dc.empty());
            // Out and back: the document's colours to .ase, then read as a library.
            const auto tmp2 = std::filesystem::temp_directory_path() / "cx_s172_shelf";
            std::filesystem::create_directories(tmp2);
            const auto out = SW::with_extension((tmp2 / "Doc").string(), SW::Format::ASE);
            const bool wrote = SW::write_file(out, "Doc", dc, SW::save_format(out));
            const auto back = SW::read_file(out);
            check("shelf: the document exported and read back", wrote && back && back->colors.size() == dc.size());
            std::filesystem::remove_all(tmp2);
        }
    }
    // ── s180: the eyedropper -- a colour taken from the page as it prints ──
    {
        std::cout << "--- s180 eyedropper ---\n";
        namespace D = core::doc;
        namespace L = composix::layout;
        core::Doc d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::load_sample(d);
        const auto page = d.as<core::Frame>("frame.1")->owner;
        const auto sw = D::add_swatch(j, d, core::Swatch{"Brick", 0, core::ColorModel::RGB, {180, 60, 40, 0}});
        const auto box = D::add_frame(j, d, page, core::FrameKind::Shape, {300, 20, 40, 30});
        D::edit_frames(j, d, {box}, "Fill", [&](core::Frame& f) { f.fill = sw; f.stroke.clear(); f.z = 1e6; });
        L::FlowCache flows;
        const auto want = d.as<core::Swatch>(sw)->preview;
        const auto near = [](std::uint32_t a, std::uint32_t b, int tol) {
            for (int sh : {16, 8, 0})
                if (std::abs(static_cast<int>((a >> sh) & 0xFF) - static_cast<int>((b >> sh) & 0xFF)) > tol) return false;
            return true;
        };
        const auto in = L::sample_page(d, page, flows, {}, 320, 35, 1);
        char hx[16];
        std::snprintf(hx, sizeof hx, "#%06X", in);
        check("eyedropper: inside a filled frame -- its colour", near(in, want, 2), hx);
        const auto tinted = [&] {
            D::edit_frames(j, d, {box}, "Fill tint", [](core::Frame& f) { f.fill_tint = 0.5; });
            const auto c = L::sample_page(d, page, flows, {}, 320, 35, 1);
            j.undo();
            return c;
        }();
        const auto half = [](std::uint32_t c, int sh) { return (static_cast<int>((c >> sh) & 0xFF) + 255) / 2; };
        check("eyedropper: a tint -- half way to paper",
              std::abs(static_cast<int>((tinted >> 16) & 0xFF) - half(want, 16)) <= 3 &&
              std::abs(static_cast<int>(tinted & 0xFF) - half(want, 0)) <= 3);
        const auto edge = L::sample_page(d, page, flows, {}, 300, 35, 2);   // half on the frame, half off
        check("eyedropper: across an edge -- the two averaged",
              ((edge >> 16) & 0xFF) > ((want >> 16) & 0xFF) && (edge & 0xFF) > (want & 0xFF) && (edge & 0xFF) < 0xF0);
        {   // a spot clear of every item on the page and its masters (either side of a spread)
            const auto* pg = d.as<core::Page>(page);
            auto owners = core::master_chain(d, page);
            owners.push_back(page);
            const auto clear = [&](double x, double y) {
                for (const auto& o : owners)
                    for (const auto& fid : d.frames_on(o)) {
                        const auto r = core::frame_rect(*d.as<core::Frame>(fid));
                        for (const double dx : {0.0, -pg->w, pg->w})
                            if (x > r.x + dx - 4 && x < r.r() + dx + 4 && y > r.y - 4 && y < r.b() + 4) return false;
                    }
                return true;
            };
            double cx = -1, cy = -1;
            for (double y = 2; pg && y < pg->h - 2 && cx < 0; y += 7)
                for (double x = 2; x < pg->w - 2; x += 7)
                    if (clear(x, y)) { cx = x; cy = y; break; }
            check("eyedropper: where nothing is -- paper", cx >= 0 && L::sample_page(d, page, flows, {}, cx, cy, 0.5) == 0xFFFFFFu);
        }
        check("eyedropper: nothing changed, no step", d.as<core::Frame>(box)->fill == sw);
    }
    // ── s181 (C1b): a vector picture's colours, as written ──
    {
        std::cout << "--- s181 SVG colours ---\n";
        namespace SW = core::swatches;
        check("css: #rgb, #rrggbb, #rrggbbaa", SW::css_color("#f80") == 0xFF8800u && SW::css_color(" #1A2b3C ") == 0x1A2B3Cu &&
                                                SW::css_color("#11223380") == 0x112233u && SW::css_color("#f808") == 0xFF8800u);
        check("css: rgb() numbers and percentages, rgba()", SW::css_color("rgb(255, 0, 10)") == 0xFF000Au &&
              SW::css_color("rgb(100%,50%,0%)") == 0xFF8000u && SW::css_color("rgba(0 128 255 / 0.5)") == 0x0080FFu);
        check("css: hsl()", SW::css_color("hsl(120, 100%, 25%)") == 0x008000u && SW::css_color("hsl(0,0%,100%)") == 0xFFFFFFu);
        check("css: names, any case", SW::css_color("Teal") == 0x008080u && SW::css_color("rebeccapurple") == 0x663399u &&
                                      SW::css_color("aliceblue") == 0xF0F8FFu && SW::css_color("yellowgreen") == 0x9ACD32u);
        check("css: not colours", !SW::css_color("none") && !SW::css_color("currentColor") && !SW::css_color("url(#g1)") &&
                                  !SW::css_color("transparent") && !SW::css_color("inherit") && !SW::css_color("#12") &&
                                  !SW::css_color("#ggg") && !SW::css_color(""));
        const std::string svg = R"svg(<?xml version="1.0"?>
<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
  <style>.sea { fill: #1E6091; stroke:none } .sand{fill:rgb(233,196,106)!important}</style>
  <defs><linearGradient id="g1"><stop offset="0" stop-color="#1E6091"/><stop offset="1" style="stop-color:white;stop-opacity:1"/></linearGradient></defs>
  <rect class="sea" width="100" height="60"/>
  <rect fill="url(#g1)" fill-opacity="0.5" data-fill="#ABCDEF" width="10" height="10"/>
  <path fill="#1e6091" d="M0 0"/>
  <circle style="fill:#e63946 icc-color(FOGRA39, 0, 0.85, 0.7, 0.05);stroke:#000" r="5"/>
  <text fill="none" stroke="currentColor" color="crimson">fill: lots</text>
</svg>)svg";
        const auto vc = SW::svg_colors(svg);
        const auto find = [&](std::uint32_t rgb, bool cmyk) -> const SW::VectorColor* {
            for (const auto& c : vc) if (c.rgb == rgb && c.cmyk == cmyk) return &c;
            return nullptr;
        };
        const auto* sea = find(0x1E6091u, false);
        check("svg: attributes, style=, <style> and stops all read; the most used first",
              sea && sea->uses == 3 && &vc.front() == sea && find(0xE9C46Au, false) && find(0xFFFFFFu, false) && find(0x000000u, false) &&
              find(0xDC143Cu, false));
        const auto* red = find(0xE63946u, true);
        check("svg: an icc-color gives its CMYK (the fallback for the screen)",
              red && red->v[0] == 0 && red->v[1] == 85 && red->v[2] == 70 && red->v[3] == 5 && !find(0xE63946u, false));
        check("svg: none / url() / currentColor / data-fill / fill-opacity / text not taken",
              !find(0xABCDEFu, false) && vc.size() == 6);
        check("svg: at most asked", SW::svg_colors(svg, 2).size() == 2);
        check("svg: nothing drawn in colour -- none", SW::svg_colors("<svg><rect fill=\"none\"/></svg>").empty());
        const auto lib = SW::vector_library("map.svg", vc);
        bool named = lib.name == "map.svg" && lib.colors.size() == vc.size();
        for (const auto& s : lib.colors)
            named = named && (s.model == core::ColorModel::CMYK ? s.name == "C0 M85 Y70 K5" : s.name.size() == 7 && s.name[0] == '#');
        check("svg: a library -- RGB by hex, CMYK by its values", named && lib.colors.front().name == "#1E6091");
    }
    std::cout << "-----------------------------------------------\n";
    std::cout << g_pass << " pass / " << g_fail << " fail\n";
    return g_fail == 0 ? 0 : 1;
}
