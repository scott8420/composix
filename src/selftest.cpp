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
#include "layout/Flow.hpp"
#include "layout/Preflight.hpp"
#include "layout/ColorReport.hpp"
#include <plate/plate.hpp>
#include "layout/Export.hpp"
#include "core/Outline.hpp"
#include "core/Recents.hpp"
#include "core/Settings.hpp"
#include "core/Shortcuts.hpp"
#include "core/Stones.hpp"
#include "core/Undo.hpp"
#include "core/TextMap.hpp"
#include "core/Hyphen.hpp"

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
                                                     seen.size() == 8 && keys.size() == 8);
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
              cross.size() == 1 && cross.front().label == "Size" && cross.front().edit.empty());
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
            of = of || (f.edit == "wrap_off" && f.value == "9 pt");
        }
        check("wrap: inspector shows Wrap + offset", wf && of);
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
        check("styles + swatches to start with", d.ids_of<core::Style>().size() == 5 && d.ids_of<core::Swatch>().size() == 2);
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
                                                                   count(pdf, "/XYZ 0 ") == static_cast<int>(bm.size()));
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
              core::doc::style_descendants(d, "style.1") == std::vector<core::Doc::Id>{"style.10", "style.6"});
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
            check("next style: a paste keeps the style", d.as<core::Story>("story.1")->paras[1].style == "style.2");
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
        for (const auto& f : items1.at(0).fields) row = row || (f.edit == "inset" && f.value == "6 pt");
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

    std::cout << "-----------------------------------------------\n";
    std::cout << g_pass << " pass / " << g_fail << " fail\n";
    return g_fail == 0 ? 0 : 1;
}
