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
#include "core/Outline.hpp"
#include "core/Recents.hpp"
#include "core/Shortcuts.hpp"
#include "core/Stones.hpp"
#include "core/Undo.hpp"
#include "core/TextMap.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace core = composix::core;

namespace {
int g_pass = 0, g_fail = 0;
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



    std::cout << "-----------------------------------------------\n";
    std::cout << g_pass << " pass / " << g_fail << " fail\n";
    return g_fail == 0 ? 0 : 1;
}
