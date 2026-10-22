// s342 (TB2, as InDesign): table styles and cell styles -- made, edited
// (written through to what uses them), applied, deleted. See Doc.hpp.
#include "core/Doc.hpp"

#include <algorithm>
#include <set>

namespace composix::core::doc {

namespace {

// Every table in every story, changed by `f` (true when it changed one) --
// one step over the stories it touched, plus `extra` records.
template <class F>
bool each_table(DocJournal& j, Doc& d, const std::string& label, const std::vector<Doc::Id>& extra, const std::function<void()>& put_extra, F&& f) {
    std::vector<std::pair<Doc::Id, Story>> changed;
    for (const auto& sid : d.ids_of<Story>()) {
        Story s = *d.as<Story>(sid);
        bool any = false;
        for (auto& p : s.paras)
            for (auto& t : p.tables) any = f(t) || any;
        if (any) changed.emplace_back(sid, std::move(s));
    }
    std::vector<Doc::Id> ids = extra;
    for (const auto& c : changed) ids.push_back(c.first);
    return j.run(label, ids, [&] {
        if (put_extra) put_extra();
        for (auto& c : changed) d.put(c.first, std::move(c.second));
    });
}

void cell_look(const CellStyle& cs, TableCell& c) {
    if (cs.fill) { c.fill = *cs.fill; c.fill_tint = cs.fill_tint; }
    if (cs.valign) c.valign = *cs.valign;
    for (std::size_t k = 0; k < 4; ++k)
        if (cs.borders[k].w >= 0) c.borders[k] = cs.borders[k];
    if (!cs.para_style.empty())
        for (auto& p : c.paras) p.style = cs.para_style;
}

// A cell following its style from `o` to `n`: each setting the old style
// set that the cell still has (or the old didn't set) takes the new one's.
void cell_through(const CellStyle& o, const CellStyle& n, TableCell& c) {
    if (n.fill && (!o.fill || (c.fill == *o.fill && c.fill_tint == o.fill_tint))) { c.fill = *n.fill; c.fill_tint = n.fill_tint; }
    if (!n.fill && o.fill && c.fill == *o.fill) c.fill.clear();
    if (n.valign && (!o.valign || c.valign == *o.valign)) c.valign = *n.valign;
    if (!n.valign && o.valign && c.valign == *o.valign) c.valign = 0;
    for (std::size_t k = 0; k < 4; ++k) {
        const auto& ob = o.borders[k];
        const auto& nb = n.borders[k];
        auto& cb = c.borders[k];
        if (nb.w >= 0 && (ob.w < 0 || cb == ob)) cb = nb;
        else if (nb.w < 0 && ob.w >= 0 && cb == ob) cb = CellBorder{};
    }
    if (n.para_style != o.para_style && !n.para_style.empty())
        for (auto& p : c.paras)
            if (o.para_style.empty() || p.style == o.para_style) p.style = n.para_style;
}

template <class T>
void through(T& v, const T& was, const T& now) {
    if (v == was) v = now;
}

std::string fresh_name(const std::vector<std::string>& taken, const std::string& base) {
    if (std::find(taken.begin(), taken.end(), base) == taken.end()) return base;
    for (int n = 2;; ++n) {
        const auto x = base + " " + std::to_string(n);
        if (std::find(taken.begin(), taken.end(), x) == taken.end()) return x;
    }
}

}  // namespace

std::string region_cell_style(const TableStyle& ts, const Table& t, int r) {
    const int R = static_cast<int>(t.rows.size());
    if (r < t.header_rows) return ts.header_cell;
    if (t.footer_rows > 0 && r >= R - t.footer_rows) return ts.footer_cell;
    return ts.body_cell;
}

void apply_cell_style(const Doc& d, TableCell& c, const Doc::Id& id) {
    c.style = id;
    if (const auto* cs = d.as<CellStyle>(id)) cell_look(*cs, c);
}

void apply_table_style(const Doc& d, Table& t, const Doc::Id& id) {
    const auto* was = d.as<TableStyle>(t.style);
    const auto* ts = d.as<TableStyle>(id);
    t.style = ts ? id : std::string{};
    if (!ts) return;
    t.rule_w = ts->rule_w;
    t.rule_color = ts->rule_color;
    t.inset = ts->inset;
    t.header_fill = ts->header_fill;
    t.header_tint = ts->header_tint;
    t.band_fill = ts->band_fill;
    t.band_tint = ts->band_tint;
    t.band_every = ts->band_every;
    for (int r = 0; r < static_cast<int>(t.rows.size()); ++r) {
        const std::string region = region_cell_style(*ts, t, r);
        const std::string old_region = was ? region_cell_style(*was, t, r) : std::string{};
        for (auto& c : t.rows[static_cast<std::size_t>(r)].cells) {
            if (!c.style.empty() && c.style != old_region) continue;   // a style of its own
            if (region.empty()) { if (!c.style.empty()) c.style.clear(); continue; }
            apply_cell_style(d, c, region);
        }
    }
}

Doc::Id add_table_style(DocJournal& j, Doc& d, const std::string& name, const Table* from) {
    TableStyle ts;
    std::vector<std::string> taken;
    for (const auto& id : d.ids_of<TableStyle>()) taken.push_back(d.as<TableStyle>(id)->name);
    ts.name = fresh_name(taken, name.empty() ? std::string("Table Style") : name);
    ts.pos = d.pos_after<TableStyle>({});
    if (from) {
        ts.rule_w = from->rule_w;
        ts.rule_color = from->rule_color;
        ts.inset = from->inset;
        ts.header_fill = from->header_fill;
        ts.header_tint = from->header_tint;
        ts.band_fill = from->band_fill;
        ts.band_tint = from->band_tint;
        ts.band_every = from->band_every;
    }
    Doc::Id made;
    j.run("New Table Style", {}, [&] {
        made = d.fresh_id("tablestyle");
        d.put(made, ts);
        return std::vector<Doc::Id>{made};
    });
    return made;
}

Doc::Id add_cell_style(DocJournal& j, Doc& d, const std::string& name, const TableCell* from) {
    CellStyle cs;
    std::vector<std::string> taken;
    for (const auto& id : d.ids_of<CellStyle>()) taken.push_back(d.as<CellStyle>(id)->name);
    cs.name = fresh_name(taken, name.empty() ? std::string("Cell Style") : name);
    cs.pos = d.pos_after<CellStyle>({});
    if (from) {
        if (!from->fill.empty()) { cs.fill = from->fill; cs.fill_tint = from->fill_tint; }
        if (from->valign) cs.valign = from->valign;
        cs.borders = from->borders;
    }
    Doc::Id made;
    j.run("New Cell Style", {}, [&] {
        made = d.fresh_id("cellstyle");
        d.put(made, cs);
        return std::vector<Doc::Id>{made};
    });
    return made;
}

bool edit_cell_style(DocJournal& j, Doc& d, const Doc::Id& id, const std::string& label, const std::function<void(CellStyle&)>& f) {
    const auto* cur = d.as<CellStyle>(id);
    if (!cur) return false;
    const CellStyle o = *cur;
    CellStyle n = o;
    f(n);
    n.pos = o.pos;
    if (n == o) return false;
    return each_table(j, d, label, {id}, [&] { d.put(id, n); }, [&](Table& t) {
        bool any = false;
        for (auto& row : t.rows)
            for (auto& c : row.cells)
                if (c.style == id) {
                    const TableCell was = c;
                    cell_through(o, n, c);
                    any = any || !(c == was);
                }
        return any;
    });
}

bool edit_table_style(DocJournal& j, Doc& d, const Doc::Id& id, const std::string& label, const std::function<void(TableStyle&)>& f) {
    const auto* cur = d.as<TableStyle>(id);
    if (!cur) return false;
    const TableStyle o = *cur;
    TableStyle n = o;
    f(n);
    n.pos = o.pos;
    if (n == o) return false;
    return each_table(j, d, label, {id}, [&] { d.put(id, n); }, [&](Table& t) {
        if (t.style != id) return false;
        const Table was = t;
        through(t.rule_w, o.rule_w, n.rule_w);
        through(t.rule_color, o.rule_color, n.rule_color);
        through(t.inset, o.inset, n.inset);
        through(t.header_fill, o.header_fill, n.header_fill);
        through(t.header_tint, o.header_tint, n.header_tint);
        through(t.band_fill, o.band_fill, n.band_fill);
        through(t.band_tint, o.band_tint, n.band_tint);
        through(t.band_every, o.band_every, n.band_every);
        for (int r = 0; r < static_cast<int>(t.rows.size()); ++r) {
            const auto ro = region_cell_style(o, t, r), rn = region_cell_style(n, t, r);
            if (ro == rn) continue;
            for (auto& c : t.rows[static_cast<std::size_t>(r)].cells) {
                if (c.style != ro) continue;   // a style of its own
                if (rn.empty()) c.style.clear();
                else apply_cell_style(d, c, rn);
            }
        }
        return !(t == was);
    });
}

bool delete_table_style(DocJournal& j, Doc& d, const Doc::Id& id) {
    if (!d.as<TableStyle>(id)) return false;
    return each_table(j, d, "Delete Table Style", {id}, [&] { d.put(id, std::nullopt); }, [&](Table& t) {
        if (t.style != id) return false;
        t.style.clear();
        return true;
    });
}

bool delete_cell_style(DocJournal& j, Doc& d, const Doc::Id& id) {
    if (!d.as<CellStyle>(id)) return false;
    std::vector<Doc::Id> tss;
    std::vector<std::pair<Doc::Id, TableStyle>> fixed;   // table styles naming it lose it
    for (const auto& ts : d.ids_of<TableStyle>()) {
        TableStyle v = *d.as<TableStyle>(ts);
        bool any = false;
        for (auto* s : {&v.header_cell, &v.body_cell, &v.footer_cell})
            if (*s == id) { s->clear(); any = true; }
        if (any) { tss.push_back(ts); fixed.emplace_back(ts, v); }
    }
    tss.push_back(id);
    return each_table(j, d, "Delete Cell Style", tss, [&] {
        for (auto& [k, v] : fixed) d.put(k, v);
        d.put(id, std::nullopt);
    }, [&](Table& t) {
        bool any = false;
        for (auto& row : t.rows)
            for (auto& c : row.cells)
                if (c.style == id) { c.style.clear(); any = true; }
        return any;
    });
}

int table_style_uses(const Doc& d, const Doc::Id& id) {
    int n = 0;
    for (const auto& sid : d.ids_of<Story>())
        for (const auto& p : d.as<Story>(sid)->paras)
            for (const auto& t : p.tables) n += t.style == id;
    return n;
}

int cell_style_uses(const Doc& d, const Doc::Id& id) {
    int n = 0;
    for (const auto& sid : d.ids_of<Story>())
        for (const auto& p : d.as<Story>(sid)->paras)
            for (const auto& t : p.tables)
                for (const auto& row : t.rows)
                    for (const auto& c : row.cells) n += c.style == id;
    return n;
}

}  // namespace composix::core::doc
