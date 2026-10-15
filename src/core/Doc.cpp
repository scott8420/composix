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
    };
    return table;
}

}  // namespace doc
}  // namespace composix::core
