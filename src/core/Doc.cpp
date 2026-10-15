#include "core/Doc.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <set>

namespace composix::core {

// ─────────────────────────────────────────────────────────────────────────────
// The store
// ─────────────────────────────────────────────────────────────────────────────
std::optional<Record> Doc::get(const Id& id) const {
    const auto it = m_recs.find(id);
    if (it == m_recs.end()) return std::nullopt;
    return it->second;
}

void Doc::put(const Id& id, const std::optional<Record>& v) {
    // The gate: a write with the door shut, outside a load, is a stray.
    if (m_loading == 0 && m_door && !m_door()) ++m_strays;
    if (v) m_recs[id] = *v;
    else   m_recs.erase(id);
    ++m_rev;
    // Keep fresh ids ahead of anything that comes back (undo of a delete).
    const auto dot = id.rfind('.');
    if (dot != std::string::npos) {
        try {
            const int n = std::stoi(id.substr(dot + 1));
            int& next = m_next[id.substr(0, dot)];
            next = std::max(next, n + 1);
        } catch (...) {}
    }
    if (m_changed) m_changed();
}

void Doc::reset() {
    m_recs.clear();
    m_next.clear();
    ++m_rev;
    if (m_changed) m_changed();
}

const Settings& Doc::settings() const {
    static const Settings kDefault{};
    const auto* s = as<Settings>("doc");
    return s ? *s : kDefault;
}

Doc::Id Doc::fresh_id(const std::string& prefix) {
    int& n = m_next[prefix];
    if (n == 0) n = 1;
    Id id;
    do { id = prefix + "." + std::to_string(n++); } while (m_recs.count(id));
    return id;
}

namespace {
template <class T> double pos_of(const T& r) {
    if constexpr (requires { r.pos; }) return r.pos;
    else return 0;
}
}  // namespace

template <class T> std::vector<Doc::Id> Doc::ids_of() const {
    std::vector<Id> out;
    for (const auto& [id, r] : m_recs)
        if (std::holds_alternative<T>(r)) out.push_back(id);
    if constexpr (std::is_same_v<T, Section>) {
        std::stable_sort(out.begin(), out.end(), [this](const Id& a, const Id& b) {
            return page_index(as<Section>(a)->start_page) < page_index(as<Section>(b)->start_page);
        });
    } else {
        std::stable_sort(out.begin(), out.end(), [this](const Id& a, const Id& b) {
            return pos_of(*as<T>(a)) < pos_of(*as<T>(b));
        });
    }
    return out;
}

template <class T> double Doc::pos_after(const Id& after) const {
    const auto order = ids_of<T>();
    if (order.empty()) return 1.0;
    const auto it = std::find(order.begin(), order.end(), after);
    if (after.empty() || it == order.end()) return pos_of(*as<T>(order.back())) + 1.0;
    const double here = pos_of(*as<T>(*it));
    if (it + 1 == order.end()) return here + 1.0;
    return (here + pos_of(*as<T>(*(it + 1)))) / 2.0;
}

// Explicit instantiations for every kind (the header declares the templates).
template std::vector<Doc::Id> Doc::ids_of<Settings>() const;
template std::vector<Doc::Id> Doc::ids_of<Section>() const;
template std::vector<Doc::Id> Doc::ids_of<Page>() const;
template std::vector<Doc::Id> Doc::ids_of<Master>() const;
template std::vector<Doc::Id> Doc::ids_of<Frame>() const;
template std::vector<Doc::Id> Doc::ids_of<Story>() const;
template std::vector<Doc::Id> Doc::ids_of<Style>() const;
template std::vector<Doc::Id> Doc::ids_of<Swatch>() const;
template std::vector<Doc::Id> Doc::ids_of<Asset>() const;
template std::vector<Doc::Id> Doc::ids_of<GenList>() const;
template std::vector<Doc::Id> Doc::ids_of<Guide>() const;
template double Doc::pos_after<Page>(const Id&) const;
template double Doc::pos_after<Master>(const Id&) const;
template double Doc::pos_after<Style>(const Id&) const;
template double Doc::pos_after<Swatch>(const Id&) const;
template double Doc::pos_after<Asset>(const Id&) const;
template double Doc::pos_after<GenList>(const Id&) const;

int Doc::page_index(const Id& page) const {
    if (!as<Page>(page)) return -1;
    const double p = as<Page>(page)->pos;
    int n = 0;
    for (const auto& [id, r] : m_recs)
        if (const auto* pg = std::get_if<Page>(&r))
            if (pg->pos < p || (pg->pos == p && id < page)) ++n;
    return n;
}

Doc::Id Doc::section_of(const Id& page) const {
    const int at = page_index(page);
    if (at < 0) return {};
    Id best;
    int best_at = -1;
    for (const auto& sid : ids_of<Section>()) {
        const int s = page_index(as<Section>(sid)->start_page);
        if (s >= 0 && s <= at && s > best_at) { best = sid; best_at = s; }
    }
    return best;
}

std::string Doc::folio(const Id& page) const {
    const int at = page_index(page);
    if (at < 0) return {};
    const Id sec = section_of(page);
    if (sec.empty()) return format_folio(at + 1, Numbering::Arabic);
    const auto* s = as<Section>(sec);
    return format_folio(s->start_at + (at - page_index(s->start_page)), s->numbering);
}

Side Doc::side_of(const Id& page) const {
    if (!settings().facing) return Side::Any;
    const int at = page_index(page);
    return at % 2 == 0 ? Side::Right : Side::Left;   // the first page is a right-hand page
}

std::vector<Doc::Id> Doc::pages_of(const Id& section) const {
    std::vector<Id> out;
    for (const auto& p : pages())
        if (section_of(p) == section) out.push_back(p);
    return out;
}

std::vector<Doc::Id> Doc::frames_on(const Id& owner) const {
    std::vector<Id> out;
    for (const auto& id : ids_of<Frame>())
        if (as<Frame>(id)->owner == owner) out.push_back(id);
    std::stable_sort(out.begin(), out.end(),
                     [this](const Id& a, const Id& b) { return as<Frame>(a)->z < as<Frame>(b)->z; });
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// Display helpers
// ─────────────────────────────────────────────────────────────────────────────
std::string format_folio(int n, Numbering how) {
    if (n <= 0) return std::to_string(n);
    switch (how) {
    case Numbering::Arabic: return std::to_string(n);
    case Numbering::RomanLower:
    case Numbering::RomanUpper: {
        static const std::pair<int, const char*> k[] = {
            {1000, "m"}, {900, "cm"}, {500, "d"}, {400, "cd"}, {100, "c"}, {90, "xc"},
            {50, "l"},   {40, "xl"},  {10, "x"},  {9, "ix"},   {5, "v"},   {4, "iv"}, {1, "i"}};
        std::string out;
        for (const auto& [v, s] : k)
            while (n >= v) { out += s; n -= v; }
        if (how == Numbering::RomanUpper)
            for (auto& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return out;
    }
    case Numbering::AlphaLower:
    case Numbering::AlphaUpper: {   // a..z, aa..zz (as InDesign)
        const char base = how == Numbering::AlphaLower ? 'a' : 'A';
        const int reps = (n - 1) / 26 + 1;
        return std::string(static_cast<std::size_t>(reps), static_cast<char>(base + (n - 1) % 26));
    }
    }
    return std::to_string(n);
}

std::string inches(double pt) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.3f", pt / kPt);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

std::string size_text(double w, double h, std::size_t folds) {
    std::string s = inches(w) + " × " + inches(h) + " in";
    if (folds > 0) s += " (" + std::to_string(folds + 1) + "-panel)";
    return s;
}

std::string numbering_text(Numbering n) {
    switch (n) {
    case Numbering::Arabic:     return "Arabic (1, 2, 3)";
    case Numbering::RomanLower: return "Roman (i, ii, iii)";
    case Numbering::RomanUpper: return "Roman (I, II, III)";
    case Numbering::AlphaLower: return "Letters (a, b, c)";
    case Numbering::AlphaUpper: return "Letters (A, B, C)";
    }
    return "?";
}

std::string side_text(Side s) {
    switch (s) {
    case Side::Any:   return "Any";
    case Side::Right: return "Right";
    case Side::Left:  return "Left";
    }
    return "?";
}

std::string master_label(const Doc& d, const Doc::Id& master) {
    const auto* m = d.as<Master>(master);
    if (!m) return "None";
    return m->prefix + "-" + m->name;
}

// ─────────────────────────────────────────────────────────────────────────────
// The verbs
// ─────────────────────────────────────────────────────────────────────────────
namespace doc {

namespace {
std::string plural(std::size_t n, const char* one, const char* many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

// Only the ids that are pages, in page order, without repeats.
std::vector<Doc::Id> only_pages(const Doc& d, const std::vector<Doc::Id>& ids) {
    std::vector<Doc::Id> out;
    for (const auto& p : d.pages())
        if (std::find(ids.begin(), ids.end(), p) != ids.end()) out.push_back(p);
    return out;
}

std::vector<Doc::Id> only_sections(const Doc& d, const std::vector<Doc::Id>& ids) {
    std::vector<Doc::Id> out;
    for (const auto& id : ids)
        if (d.as<Section>(id) && std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
    return out;
}

// Apply `f` to each page record; a step only if something changes.
template <class F>
bool edit_pages(DocJournal& j, Doc& d, const std::vector<Doc::Id>& ids, const std::string& label, F f) {
    const auto pages = only_pages(d, ids);
    if (pages.empty()) return false;
    return j.run(label, pages, [&] {
        for (const auto& p : pages) {
            Page pg = *d.as<Page>(p);
            f(pg);
            d.put(p, pg);
        }
    });
}

template <class F>
bool edit_sections(DocJournal& j, Doc& d, const std::vector<Doc::Id>& ids, const std::string& label, F f) {
    const auto secs = only_sections(d, ids);
    if (secs.empty()) return false;
    return j.run(label, secs, [&] {
        for (const auto& s : secs) {
            Section sec = *d.as<Section>(s);
            f(sec);
            d.put(s, sec);
        }
    });
}
}  // namespace

std::vector<SizePreset> size_presets(const Settings& s) {
    const double tuck = 0.125 * kPt;   // a fold-in panel is narrower so it clears the spine
    return {
        {"Trim (" + size_text(s.trim_w, s.trim_h, 0) + ")", s.trim_w, s.trim_h, {}},
        {"2-panel foldout (" + size_text(2 * s.trim_w - tuck, s.trim_h, 1) + ")",
         2 * s.trim_w - tuck, s.trim_h, {s.trim_w}},
        {"3-panel foldout (" + size_text(3 * s.trim_w - 2 * tuck, s.trim_h, 2) + ")",
         3 * s.trim_w - 2 * tuck, s.trim_h, {s.trim_w, 2 * s.trim_w}},
    };
}

std::vector<Doc::Id> add_pages(DocJournal& j, Doc& d, const Doc::Id& after, int count,
                               const Doc::Id& master) {
    std::vector<Doc::Id> made;
    if (count <= 0) return made;
    j.run("Add " + plural(static_cast<std::size_t>(count), "page", "pages"), {}, [&] {
        const auto& s = d.settings();
        Doc::Id prev = after.empty() && !d.pages().empty() ? d.pages().back() : after;
        for (int i = 0; i < count; ++i) {
            const Doc::Id id = d.fresh_id("page");
            Page pg;
            pg.pos = d.pos_after<Page>(prev);
            pg.w = s.trim_w;
            pg.h = s.trim_h;
            pg.master = master;
            d.put(id, pg);
            made.push_back(id);
            prev = id;
        }
        return made;
    });
    return made;
}

bool remove_pages(DocJournal& j, Doc& d, const std::vector<Doc::Id>& ids) {
    const auto pages = only_pages(d, ids);
    if (pages.empty() || pages.size() >= d.pages().size()) return false;   // a document keeps one page
    // Everything the removal touches is in the step: the pages, their frames,
    // and any section starting on one of them (it moves to the next survivor,
    // or goes if its whole run goes).
    std::vector<Doc::Id> touched = pages;
    for (const auto& p : pages)
        for (const auto& f : d.frames_on(p)) touched.push_back(f);
    const auto secs = d.ids_of<Section>();
    for (const auto& s : secs) touched.push_back(s);
    return j.run("Delete " + plural(pages.size(), "page", "pages"), touched, [&] {
        const auto order = d.pages();
        const auto gone = [&](const Doc::Id& p) {
            return std::find(pages.begin(), pages.end(), p) != pages.end();
        };
        for (const auto& sid : secs) {
            Section s = *d.as<Section>(sid);
            if (!gone(s.start_page)) continue;
            auto it = std::find(order.begin(), order.end(), s.start_page);
            Doc::Id next;
            for (++it; it != order.end(); ++it) {
                if (gone(*it)) continue;
                // A survivor still in this section's run (before the next section starts)?
                bool other = false;
                for (const auto& o : secs)
                    if (o != sid && d.as<Section>(o)->start_page == *it) other = true;
                if (!other) next = *it;
                break;
            }
            if (next.empty()) d.put(sid, std::nullopt);
            else { s.start_page = next; d.put(sid, s); }
        }
        for (const auto& p : pages) {
            for (const auto& f : d.frames_on(p)) d.put(f, std::nullopt);
            d.put(p, std::nullopt);
        }
    });
}

bool move_pages(DocJournal& j, Doc& d, const std::vector<Doc::Id>& ids, const Doc::Id& after) {
    const auto pages = only_pages(d, ids);
    if (pages.empty() || std::find(pages.begin(), pages.end(), after) != pages.end()) return false;
    return j.run("Move " + plural(pages.size(), "page", "pages"), pages, [&] {
        // Lay them out, in order, into the gap after `after` (or at the front).
        const auto rest = [&] {
            std::vector<Doc::Id> r;
            for (const auto& p : d.pages())
                if (std::find(pages.begin(), pages.end(), p) == pages.end()) r.push_back(p);
            return r;
        }();
        double lo = 0, hi = 0;
        if (after.empty()) {
            hi = rest.empty() ? 1.0 : d.as<Page>(rest.front())->pos;
            lo = hi - 1.0;
        } else {
            const auto it = std::find(rest.begin(), rest.end(), after);
            lo = d.as<Page>(after)->pos;
            hi = (it + 1 == rest.end()) ? lo + 1.0 : d.as<Page>(*(it + 1))->pos;
        }
        const double step = (hi - lo) / static_cast<double>(pages.size() + 1);
        double at = lo;
        for (const auto& p : pages) {
            Page pg = *d.as<Page>(p);
            at += step;
            pg.pos = at;
            d.put(p, pg);
        }
    });
}

bool set_master(DocJournal& j, Doc& d, const std::vector<Doc::Id>& ids, const Doc::Id& master) {
    if (!master.empty() && !d.as<Master>(master)) return false;
    return edit_pages(j, d, ids, "Apply master " + master_label(d, master),
                      [&](Page& p) { p.master = master; });
}

bool set_page_size(DocJournal& j, Doc& d, const std::vector<Doc::Id>& ids, const SizePreset& sz) {
    return edit_pages(j, d, ids, "Page size " + size_text(sz.w, sz.h, sz.folds.size()), [&](Page& p) {
        p.w = sz.w;
        p.h = sz.h;
        p.folds = sz.folds;
    });
}

Doc::Id add_section(DocJournal& j, Doc& d, const Doc::Id& start_page, const std::string& name,
                    Numbering n, int start_at) {
    if (!d.as<Page>(start_page)) return {};
    for (const auto& s : d.ids_of<Section>())
        if (d.as<Section>(s)->start_page == start_page) return {};   // one section per start
    Doc::Id made;
    j.run("New section “" + name + "”", {}, [&] {
        made = d.fresh_id("section");
        Section s;
        s.name = name;
        s.start_page = start_page;
        s.numbering = n;
        s.start_at = start_at;
        s.master = d.as<Page>(start_page)->master;
        d.put(made, s);
        return std::vector<Doc::Id>{made};
    });
    return made;
}

bool set_numbering(DocJournal& j, Doc& d, const std::vector<Doc::Id>& ids, Numbering n) {
    return edit_sections(j, d, ids, "Numbering " + numbering_text(n), [&](Section& s) { s.numbering = n; });
}

bool set_starts(DocJournal& j, Doc& d, const std::vector<Doc::Id>& ids, Side side) {
    return edit_sections(j, d, ids, "Section starts " + side_text(side), [&](Section& s) { s.starts = side; });
}

bool remove_section(DocJournal& j, Doc& d, const Doc::Id& section) {
    const auto* s = d.as<Section>(section);
    if (!s) return false;
    return j.run("Delete section “" + s->name + "”", {section},
                 [&] { d.put(section, std::nullopt); });
}

Doc::Id neighbour_page(const Doc& d, const std::vector<Doc::Id>& gone) {
    const auto order = d.pages();
    const auto is_gone = [&](const Doc::Id& p) { return std::find(gone.begin(), gone.end(), p) != gone.end(); };
    int last = -1;
    for (int i = 0; i < static_cast<int>(order.size()); ++i)
        if (is_gone(order[static_cast<std::size_t>(i)])) last = i;
    if (last < 0) return {};
    for (int i = last + 1; i < static_cast<int>(order.size()); ++i)
        if (!is_gone(order[static_cast<std::size_t>(i)])) return order[static_cast<std::size_t>(i)];
    for (int i = last - 1; i >= 0; --i)
        if (!is_gone(order[static_cast<std::size_t>(i)])) return order[static_cast<std::size_t>(i)];
    return {};
}

bool move_frames(DocJournal& j, Doc& d, const std::vector<Doc::Id>& ids, double dx, double dy,
                 const std::string& merge_key) {
    std::vector<Doc::Id> frames;
    for (const auto& id : ids) if (d.as<Frame>(id)) frames.push_back(id);
    if (frames.empty() || (dx == 0 && dy == 0)) return false;
    return j.run("Move " + plural(frames.size(), "frame", "frames"), frames, [&] {
        for (const auto& id : frames) {
            Frame f = *d.as<Frame>(id);
            f.x += dx;
            f.y += dy;
            d.put(id, f);
        }
    }, merge_key);
}

void frames_begin(DocJournal& j, Doc& d, const std::vector<Doc::Id>& ids, const std::string& label) {
    std::vector<Doc::Id> frames;
    for (const auto& id : ids) if (d.as<Frame>(id)) frames.push_back(id);
    if (!frames.empty()) j.begin(label, frames);
}

void frames_set(DocJournal& j, Doc& d, const Doc::Id& id, const Rect& r) {
    const auto* f0 = d.as<Frame>(id);
    if (!f0) return;
    Frame f = *f0;
    if (f.x == r.x && f.y == r.y && f.w == r.w && f.h == r.h) return;
    // Through the door even mid-drag: inside the open gesture this just writes.
    j.run("Frame bounds", {id}, [&] {
        f.x = r.x; f.y = r.y; f.w = r.w; f.h = r.h;
        d.put(id, f);
    });
}

bool frames_end(DocJournal& j) { return j.commit(); }

Doc::Id add_guide(DocJournal& j, Doc& d, const Doc::Id& owner, bool vertical, double pos) {
    if (!d.as<Page>(owner) && !d.as<Master>(owner)) return {};
    Doc::Id made;
    j.run(vertical ? "Add vertical guide" : "Add horizontal guide", {}, [&] {
        made = d.fresh_id("guide");
        d.put(made, Guide{owner, vertical, pos});
        return std::vector<Doc::Id>{made};
    });
    return made;
}

void guide_begin(DocJournal& j, Doc& d, const Doc::Id& id) {
    if (d.as<Guide>(id)) j.begin("Move guide", {id});
}

void guide_set(DocJournal& j, Doc& d, const Doc::Id& id, double pos) {
    const auto* g0 = d.as<Guide>(id);
    if (!g0 || g0->pos == pos) return;
    Guide g = *g0;
    j.run("Move guide", {id}, [&] { g.pos = pos; d.put(id, g); });
}

// Dragged off its page: the guide goes, inside the open gesture -- the
// gesture's one step records it as a removal ("Move guide" becomes one undo
// that brings it back where it started).
void guide_remove_in_gesture(DocJournal& j, Doc& d, const Doc::Id& id) {
    if (!d.as<Guide>(id)) return;
    j.relabel("Remove guide");
    j.run("Remove guide", {id}, [&] { d.put(id, std::nullopt); });
}

bool remove_guide(DocJournal& j, Doc& d, const Doc::Id& id) {
    if (!d.as<Guide>(id)) return false;
    return j.run("Remove guide", {id}, [&] { d.put(id, std::nullopt); });
}
void frames_cancel(DocJournal& j) { j.cancel(); }

const std::vector<Verb>& verbs() {
    // Each runs on the loaded sample. A verb the UI offers and this table
    // lacks is the gap the gate cannot see -- add it in the same change.
    static const std::vector<Verb> table = {
        {"add_pages", [](DocJournal& j, Doc& d) { add_pages(j, d, d.pages()[2], 2, "master.2"); }},
        {"remove_pages", [](DocJournal& j, Doc& d) {
             const auto p = d.pages();
             remove_pages(j, d, {p[0], p[4]});   // p[0] starts a section: it moves
         }},
        {"move_pages", [](DocJournal& j, Doc& d) {
             const auto p = d.pages();
             move_pages(j, d, {p[1], p[2]}, p[6]);
         }},
        {"set_master", [](DocJournal& j, Doc& d) {
             const auto p = d.pages();
             set_master(j, d, {p[1], p[3], p[5]}, "master.3");
         }},
        {"set_page_size", [](DocJournal& j, Doc& d) {
             const auto p = d.pages();
             set_page_size(j, d, {p[3]}, size_presets(d.settings())[1]);
         }},
        {"add_section", [](DocJournal& j, Doc& d) {
             add_section(j, d, d.pages()[9], "Appendix", Numbering::AlphaUpper, 1);
         }},
        {"set_numbering", [](DocJournal& j, Doc& d) {
             set_numbering(j, d, d.ids_of<Section>(), Numbering::RomanUpper);
         }},
        {"set_starts", [](DocJournal& j, Doc& d) {
             set_starts(j, d, d.ids_of<Section>(), Side::Left);
         }},
        {"remove_section", [](DocJournal& j, Doc& d) { remove_section(j, d, d.ids_of<Section>().back()); }},
        {"move_frames", [](DocJournal& j, Doc& d) { move_frames(j, d, {"frame.1", "frame.4"}, 9, -3); }},
        {"add_guide", [](DocJournal& j, Doc& d) { add_guide(j, d, "page.6", true, 100); }},
        {"drag_guide", [](DocJournal& j, Doc& d) {
             guide_begin(j, d, "guide.1");
             guide_set(j, d, "guide.1", 60);
             guide_set(j, d, "guide.1", 72);
             frames_end(j);
         }},
        {"drag_guide_off", [](DocJournal& j, Doc& d) {   // dragged off its page: removed, one step
             guide_begin(j, d, "guide.1");
             guide_set(j, d, "guide.1", -40);
             guide_remove_in_gesture(j, d, "guide.1");
             frames_end(j);
         }},
        {"remove_guide", [](DocJournal& j, Doc& d) { remove_guide(j, d, "guide.2"); }},
        {"drag_frames", [](DocJournal& j, Doc& d) {   // press .. drag .. release
             frames_begin(j, d, {"frame.5", "frame.6"}, "Resize 2 frames");
             frames_set(j, d, "frame.5", {20, 40, 900, 400});
             frames_set(j, d, "frame.6", {20, 450, 400, 50});
             frames_set(j, d, "frame.5", {24, 44, 880, 390});
             frames_end(j);
         }},
    };
    return table;
}

}  // namespace doc
}  // namespace composix::core

namespace composix::core {

std::vector<Placed> layout_pages(const Doc& d, const std::vector<Doc::Id>& ids, double gap) {
    std::vector<Placed> out;
    double x = 0;
    for (const auto& id : ids) {
        double w = 0, h = 0;
        bool master = false;
        if (const auto* p = d.as<Page>(id))        { w = p->w; h = p->h; }
        else if (const auto* m = d.as<Master>(id)) { w = m->w; h = m->h; master = true; }
        else continue;
        out.push_back({id, {x, 0, w, h}, master});
        x += w + gap;
    }
    return out;
}

std::vector<Doc::Id> master_chain(const Doc& d, const Doc::Id& id) {
    std::vector<Doc::Id> chain;
    Doc::Id m;
    if (const auto* p = d.as<Page>(id)) m = p->master;
    else if (const auto* ms = d.as<Master>(id)) m = ms->based_on;
    while (!m.empty() && d.as<Master>(m) &&
           std::find(chain.begin(), chain.end(), m) == chain.end()) {   // a cycle stops here
        chain.insert(chain.begin(), m);
        m = d.as<Master>(m)->based_on;
    }
    return chain;
}

Rect frame_in_layout(const Doc& d, const std::vector<Placed>& pl, const Doc::Id& id) {
    const auto* f = d.as<Frame>(id);
    if (!f) return {};
    for (const auto& p : pl)
        if (p.id == f->owner) return {p.r.x + f->x, p.r.y + f->y, f->w, f->h};
    return {};
}

Doc::Id hit_frame(const Doc& d, const std::vector<Placed>& pl, double x, double y, double tol) {
    // Every placed page's frames, even off the page: a frame may hang onto
    // the pasteboard.
    for (const auto& p : pl) {
        const auto own = d.frames_on(p.id);
        for (auto it = own.rbegin(); it != own.rend(); ++it) {   // topmost first
            const Rect r = frame_in_layout(d, pl, *it);
            if (Rect{r.x - tol, r.y - tol, r.w + 2 * tol, r.h + 2 * tol}.contains(x, y)) return *it;
        }
    }
    return {};
}

std::vector<Doc::Id> frames_in(const Doc& d, const std::vector<Placed>& pl, const Rect& r) {
    std::vector<Doc::Id> out;
    for (const auto& p : pl)
        for (const auto& f : d.frames_on(p.id))
            if (frame_in_layout(d, pl, f).intersects(r)) out.push_back(f);
    return out;
}

std::pair<int, int> thread_place(const Doc& d, const Doc::Id& id) {
    const auto* f = d.as<Frame>(id);
    if (!f || f->kind != FrameKind::Text || f->story.empty()) return {0, 0};
    // The head: the frame of this story nobody points to.
    std::vector<Doc::Id> mine;
    for (const auto& o : d.ids_of<Frame>())
        if (d.as<Frame>(o)->story == f->story) mine.push_back(o);
    Doc::Id head;
    for (const auto& o : mine) {
        bool pointed = false;
        for (const auto& q : mine) pointed = pointed || d.as<Frame>(q)->next == o;
        if (!pointed) { head = o; break; }
    }
    int at = 0, n = 0;
    for (Doc::Id k = head; !k.empty() && n <= static_cast<int>(mine.size()); k = d.as<Frame>(k)->next) {
        ++n;
        if (k == id) at = n;
        if (!d.as<Frame>(k)) break;
    }
    return {at, n};
}

}  // namespace composix::core

namespace composix::core {

std::vector<Doc::Id> guides_on(const Doc& d, const Doc::Id& owner) {
    std::vector<Doc::Id> out;
    for (const auto& id : d.ids_of<Guide>())
        if (d.as<Guide>(id)->owner == owner) out.push_back(id);
    return out;
}

Doc::Id hit_guide(const Doc& d, const std::vector<Placed>& pl, double x, double y, double tol) {
    for (const auto& p : pl) {
        if (!p.r.contains(x, y)) continue;
        for (const auto& g : guides_on(d, p.id)) {
            const auto& gd = *d.as<Guide>(g);
            const double at = gd.vertical ? p.r.x + gd.pos : p.r.y + gd.pos;
            if (std::abs((gd.vertical ? x : y) - at) <= tol) return g;
        }
    }
    return {};
}

Doc::Id page_at(const std::vector<Placed>& pl, double x, double y) {
    for (const auto& p : pl)
        if (p.r.contains(x, y)) return p.id;
    return {};
}

SnapTargets snap_targets(const Doc& d, const std::vector<Placed>& pl, const std::vector<Doc::Id>& exclude) {
    SnapTargets t;
    const auto skip = [&](const Doc::Id& id) { return std::find(exclude.begin(), exclude.end(), id) != exclude.end(); };
    for (const auto& p : pl) {
        t.xs.insert(t.xs.end(), {p.r.x, p.r.cx(), p.r.r()});
        t.ys.insert(t.ys.end(), {p.r.y, p.r.cy(), p.r.b()});
        const std::vector<double>* folds = nullptr;
        if (const auto* pg = d.as<Page>(p.id)) folds = &pg->folds;
        else if (const auto* m = d.as<Master>(p.id)) folds = &m->folds;
        if (folds) for (double f : *folds) t.xs.push_back(p.r.x + f);
        auto owners = p.master ? std::vector<Doc::Id>{} : master_chain(d, p.id);
        owners.push_back(p.id);
        for (const auto& o : owners) {
            for (const auto& g : guides_on(d, o)) {
                const auto& gd = *d.as<Guide>(g);
                (gd.vertical ? t.xs : t.ys).push_back((gd.vertical ? p.r.x : p.r.y) + gd.pos);
            }
            for (const auto& f : d.frames_on(o)) {
                if (skip(f)) continue;
                const auto& fr = *d.as<Frame>(f);
                const Rect r{p.r.x + fr.x, p.r.y + fr.y, fr.w, fr.h};
                t.xs.insert(t.xs.end(), {r.x, r.cx(), r.r()});
                t.ys.insert(t.ys.end(), {r.y, r.cy(), r.b()});
            }
        }
    }
    return t;
}

SnapHit snap_any(const std::vector<double>& vs, const std::vector<double>& targets, double tol) {
    SnapHit best;
    double gap = tol;
    for (double v : vs)
        for (double t : targets) {
            const double dd = t - v;
            if (std::abs(dd) <= gap) {
                gap = std::abs(dd);
                best.delta = dd;
                best.line = t;
            }
        }
    return best;
}

}  // namespace composix::core
