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
#include "layout/Export.hpp"
#include "core/Outline.hpp"
#include "core/Recents.hpp"
#include "core/Shortcuts.hpp"
#include "core/Stones.hpp"
#include "core/Undo.hpp"
#include "core/TextMap.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
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
              core::page_at(pl, 10, 10) == "page.6" && core::page_at(pl, 440, 10).empty());
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
        j.undo();
        check("undo: frame and story both gone", !d.as<core::Frame>(t) && !d.as<core::Story>(tf ? "story.5" : "x"));
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
              (has("/FontFile2") || has("/FontFile3")) && has("/ToUnicode") && has("+DejaVu"));
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
        // Balance: where the story ends, the columns come out even.
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
                if (ft->lines[k + 1].y == ft->lines[k].y && ft->lines[k + 1].x > ft->lines[k].x) {
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
                for (const auto& l : f.lines) none = none && !(l.y + l.h > jr[0].y0 && l.y < jr[0].y1);
        check("wrap: Jump leaves its band empty", none && !jr.empty() && jr[0].jump);
        j.undo();
        check("wrap: undo", d.as<core::Frame>("frame.11")->wrap == core::Wrap::Around);
        // None: the opening text as before s018 (no line set on its own).
        check("wrap: off", core::doc::edit_frames(j, d, {"frame.11"}, "Text wrap",
                                                  [](core::Frame& f) { f.wrap = core::Wrap::None; }));
        bool plain = true;
        for (const auto& f : L::flow_story(d, "story.1").frames)
            for (const auto& l : f.lines) plain = plain && l.w <= 0;
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
        check("styles + swatches to start with", d.ids_of<core::Style>().size() == 4 && d.ids_of<core::Swatch>().size() == 2);
        check("not a step: nothing to undo", !j.can_undo());
        check("the next frame gets a fresh id", d.fresh_id("frame") == "frame.9");
        std::string err;
        core::Doc e;
        check("round-trips through JSON", core::from_json(e, core::to_json(d), &err) && e.records() == d.records(), err);
        n.facing = false;
        n.text_frames = false;
        core::Doc u;
        core::new_document(u, n);
        check("unfaced: no sides, no frames", u.side_of("page.2") != core::Side::Left && u.ids_of<core::Frame>().empty() &&
                                              !u.as<core::Story>("story.1"));
        const auto o = core::project(d, "");
        check("the projection shows it", o.size() > 0);
    }

    std::cout << "-----------------------------------------------\n";
    std::cout << g_pass << " pass / " << g_fail << " fail\n";
    return g_fail == 0 ? 0 : 1;
}
