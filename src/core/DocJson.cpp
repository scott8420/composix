// core::Doc <-> document.json, and the project folder (s004).
//
// The project is a FOLDER (REQUIREMENTS §8, as Folio): <name>/document.json
// plus assets/ for embedded files. The JSON is a list of records, each
// {"id", "kind", ...fields}, so a hand-read diff shows what changed and an
// unknown kind from a newer Composix is skipped, not fatal.
#include "core/Doc.hpp"

#include "json.hpp"

#include <algorithm>
#include <filesystem>
#include <cstdio>
#include <set>
#include <fstream>
#include <ctime>
#include <chrono>
#include <sstream>

namespace composix::core {

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr int kFormat = 1;

template <class E> int en(E e) { return static_cast<int>(e); }
template <class E> E de(const json& j, const char* k, E dflt) {
    return j.contains(k) ? static_cast<E>(j.at(k).get<int>()) : dflt;
}
template <class T> T val(const json& j, const char* k, T dflt) {
    return j.contains(k) ? j.at(k).get<T>() : dflt;
}

json to(const Record& r) {
    return std::visit([](const auto& v) -> json {
        using T = std::decay_t<decltype(v)>;
        json j;
        if constexpr (std::is_same_v<T, Settings>) {
            j = {{"kind", "settings"}, {"title", v.title}, {"trim_w", v.trim_w}, {"trim_h", v.trim_h},
                 {"bleed", v.bleed}, {"facing", v.facing}, {"wrap_beneath", v.wrap_beneath}, {"units", static_cast<int>(v.units)},
                 {"export", {{"print", v.exp.print}, {"bleed", v.exp.bleed}, {"marks", v.exp.marks},
                             {"profile", v.exp.profile}, {"convert", v.exp.convert}, {"standard", v.exp.standard},
                             {"range", v.exp.range}, {"bookmarks", v.exp.bookmarks}}}};
            if (v.note_restart != NoteRestart::Story) j["note_restart"] = static_cast<int>(v.note_restart);   // s111
            if (v.note_style != NoteStyle::Arabic) j["note_style"] = static_cast<int>(v.note_style);
            if (v.note_area != 100) j["note_area"] = v.note_area;   // s117
            if (v.note_rule != 33) j["note_rule"] = v.note_rule;   // s118
            if (v.note_rule_w != 0.5) j["note_rule_w"] = v.note_rule_w;
            if (v.note_space != 0) j["note_space"] = v.note_space;
            if (!v.note_cont.empty()) j["note_cont"] = v.note_cont;
            if (v.note_at != NoteAt::Foot) j["note_at"] = static_cast<int>(v.note_at);   // s120
            if (v.note_title != "Notes") j["note_title"] = v.note_title;
            if (v.note_num != Settings::NoteNum::Plain) j["note_num"] = static_cast<int>(v.note_num);   // s126
            if (!v.exp.mark_lists.empty()) j["export"]["mark_lists"] = v.exp.mark_lists;   // s082
            if (v.exp.spreads) j["export"]["spreads"] = true;                              // s084
            if (v.exp.perceptual) j["export"]["perceptual"] = true;                        // s085
            if (!v.exp.rebuild) j["export"]["rebuild"] = false;                            // s089
            if (!v.queries.empty()) {   // s078
                json qs = json::array();
                for (const auto& q : v.queries)
                    qs.push_back({{"name", q.name}, {"what", q.what}, {"with", q.with}, {"case", q.match_case}, {"word", q.whole_word},
                                  {"mode", q.mode}, {"format", q.format}, {"in", q.in_style}, {"apply", q.apply_style}});
                j["queries"] = qs;
            }
        } else if constexpr (std::is_same_v<T, Section>) {
            j = {{"kind", "section"}, {"name", v.name}, {"start_page", v.start_page},
                 {"numbering", en(v.numbering)}, {"start_at", v.start_at}, {"starts", en(v.starts)},
                 {"master", v.master}};
        } else if constexpr (std::is_same_v<T, Page>) {
            j = {{"kind", "page"}, {"pos", v.pos}, {"w", v.w}, {"h", v.h}, {"folds", v.folds},
                 {"master", v.master}};
            if (v.margins)   // s062
                j["margins"] = {v.margins->top, v.margins->bottom, v.margins->inside, v.margins->outside};
        } else if constexpr (std::is_same_v<T, Master>) {
            j = {{"kind", "master"}, {"prefix", v.prefix}, {"name", v.name}, {"pos", v.pos}, {"w", v.w},
                 {"h", v.h}, {"folds", v.folds}, {"columns", v.columns}, {"based_on", v.based_on},
                 {"margins", {v.margins.top, v.margins.bottom, v.margins.inside, v.margins.outside}},
                 {"gutter", v.gutter}, {"grid", v.grid}};
        } else if constexpr (std::is_same_v<T, Frame>) {
            j = {{"kind", "frame"}, {"owner", v.owner}, {"type", en(v.kind)}, {"x", v.x}, {"y", v.y},
                 {"w", v.w}, {"h", v.h}, {"rot", v.rot}, {"z", v.z}, {"story", v.story},
                 {"next", v.next}, {"asset", v.asset}, {"fit", en(v.fit)},
                 {"pic_x", v.pic_x}, {"pic_y", v.pic_y}, {"pic_w", v.pic_w},
                 {"columns", v.columns}, {"gutter", v.gutter}, {"balance", v.balance}, {"widths", v.widths},
                 {"fill", v.fill}, {"stroke", v.stroke}, {"stroke_w", v.stroke_w},
                 {"fill_tint", v.fill_tint}, {"stroke_tint", v.stroke_tint}, {"opacity", v.opacity},
                 {"side", en(v.side)}, {"overrides", v.overrides},
                 {"wrap", en(v.wrap)},
                 {"wrap_off", {v.wrap_off.top, v.wrap_off.right, v.wrap_off.bottom, v.wrap_off.left}}};
            if (v.valign != VAlign::Top) j["valign"] = en(v.valign);   // s042
            if (v.autosize) j["autosize"] = true;                      // s048
            if (v.ignore_wrap) j["ignore_wrap"] = true;                // s069
            if (!(v.inset == Sides{})) j["inset"] = {v.inset.top, v.inset.right, v.inset.bottom, v.inset.left};   // s040
            if (!(v.col_rule == ColumnRule{}))   // s055
                j["col_rule"] = {{"weight", v.col_rule.weight}, {"color", v.col_rule.color},
                                 {"tint", v.col_rule.tint}, {"inset", v.col_rule.inset}};
        } else if constexpr (std::is_same_v<T, Story>) {
            json paras = json::array();
            for (const auto& p : v.paras) {
                json jp = {{"style", p.style}, {"text", p.text}};
                if (!p.runs.empty()) {
                    json runs = json::array();
                    for (const auto& r : p.runs) {
                        json jr = {{"start", r.start}, {"end", r.end}, {"style", r.style}};
                        if (!r.local.empty())   // s030
                            jr["local"] = {{"bold", r.local.bold}, {"italic", r.local.italic},
                                           {"size", r.local.size}, {"color", r.local.color}};
                        runs.push_back(jr);
                    }
                    jp["runs"] = runs;
                }
                if (p.restart > 0) jp["restart"] = p.restart;   // s047
                if (p.brk != ParaStart::Anywhere) jp["brk"] = en(p.brk);   // s055
                if (!p.over.empty()) {   // s093
                    json o = json::object();
                    if (p.over.align) o["align"] = en(*p.over.align);
                    if (p.over.left_indent) o["left_indent"] = *p.over.left_indent;
                    if (p.over.right_indent) o["right_indent"] = *p.over.right_indent;
                    if (p.over.first_indent) o["first_indent"] = *p.over.first_indent;
                    if (p.over.space_before) o["space_before"] = *p.over.space_before;
                    if (p.over.space_after) o["space_after"] = *p.over.space_after;
                    if (p.over.keep_next) o["keep_next"] = *p.over.keep_next;     // s094
                    if (p.over.keep_lines) o["keep_lines"] = *p.over.keep_lines;
                    if (p.over.hyphenate) o["hyphenate"] = *p.over.hyphenate;
                    jp["own"] = o;
                }
                if (!p.notes.empty()) {   // s109
                    json ns = json::array();
                    for (const auto& n : p.notes) {
                        json jn = {{"text", n.text}};
                        if (!n.runs.empty()) {
                            json runs = json::array();
                            for (const auto& r : n.runs) {
                                json jr = {{"start", r.start}, {"end", r.end}, {"style", r.style}};
                                if (!r.local.empty())
                                    jr["local"] = {{"bold", r.local.bold}, {"italic", r.local.italic},
                                                   {"size", r.local.size}, {"color", r.local.color}};
                                runs.push_back(jr);
                            }
                            jn["runs"] = runs;
                        }
                        ns.push_back(jn);
                    }
                    jp["notes"] = ns;
                }
                if (!p.refs.empty()) {   // s128
                    json rs = json::array();
                    for (const auto& r : p.refs) {
                        json jr = {{"to", r.target}, {"show", static_cast<int>(r.show)}};
                        if (!r.pattern.empty()) jr["pattern"] = r.pattern;   // s129
                        rs.push_back(jr);
                    }
                    jp["refs"] = rs;
                }
                if (!p.anchors.empty()) jp["anchors"] = p.anchors;   // s128
                if (!p.index.empty()) {   // s134
                    json xs = json::array();
                    for (const auto& m : p.index) {
                        json jm = {{"term", m.term}};
                        if (!m.sub.empty()) jm["sub"] = m.sub;
                        if (!m.see.empty()) jm["see"] = m.see;   // s136
                        if (m.also) jm["also"] = true;
                        if (m.span != IndexSpan::Point) jm["span"] = static_cast<int>(m.span);   // s139
                        if (m.span == IndexSpan::Paragraphs) jm["count"] = m.count;
                        xs.push_back(jm);
                    }
                    jp["index"] = xs;
                }
                paras.push_back(jp);
            }
            j = {{"kind", "story"}, {"name", v.name}, {"paras", paras}};
        } else if constexpr (std::is_same_v<T, Style>) {
            j = {{"kind", "style"}, {"name", v.name}, {"type", en(v.kind)}, {"pos", v.pos},
                 {"family", v.family}, {"size", v.size}, {"leading", v.leading}, {"bold", v.bold},
                 {"italic", v.italic}, {"align", en(v.align)}, {"space_before", v.space_before},
                 {"space_after", v.space_after}, {"color", v.color},
                 {"color_tint", v.color_tint},
                 {"left_indent", v.left_indent}, {"right_indent", v.right_indent}, {"first_indent", v.first_indent},
                 {"skip_after_heading", v.skip_after_heading},
                 {"drop_lines", v.drop_lines}, {"drop_chars", v.drop_chars}, {"align_grid", v.align_grid}, {"hyphenate", v.hyphenate},
                 {"hyph_limit", v.hyph_limit}, {"hyph_shortest", v.hyph_shortest}, {"hyph_last_word", v.hyph_last_word},
                 {"keep_next", v.keep_next}, {"keep_lines", v.keep_lines}, {"start", en(v.start)}};
            if (v.no_break) j["no_break"] = true;   // s075
            if (!v.tabs.empty()) {
                json tabs = json::array();
                for (const auto& t : v.tabs) tabs.push_back({{"pos", t.pos}, {"align", en(t.align)}, {"leader", t.leader}});
                j["tabs"] = tabs;
            }
            if (!v.based_on.empty()) { j["based_on"] = v.based_on; j["own"] = v.own; }   // s037
            if (v.list.type != ListType::None)   // s045
                j["list"] = {{"type", en(v.list.type)}, {"bullet", v.list.bullet}, {"numbering", en(v.list.numbering)},
                             {"after", v.list.after}, {"hang", v.list.hang}, {"continues", v.list.continues}, {"level", v.list.level}};
            for (const auto& [key, r] : {std::pair{"rule_above", &v.rule_above}, std::pair{"rule_below", &v.rule_below}})   // s044
                if (r->weight > 0)
                    j[key] = {{"weight", r->weight}, {"color", r->color}, {"tint", r->tint}, {"offset", r->offset}, {"text", r->text_width}};
            if (!v.next.empty()) j["next"] = v.next;
        } else if constexpr (std::is_same_v<T, Swatch>) {
            j = {{"kind", "swatch"}, {"name", v.name}, {"pos", v.pos}, {"model", en(v.model)},
                 {"v", v.v}, {"spot", v.spot}, {"preview", v.preview}};
        } else if constexpr (std::is_same_v<T, Asset>) {
            j = {{"kind", "asset"}, {"name", v.name}, {"pos", v.pos}, {"path", v.path},
                 {"embedded", v.embedded}, {"color", v.color}, {"px_w", v.px_w}, {"px_h", v.px_h},
                 {"source", v.source}, {"stamp", v.stamp}};
        } else if constexpr (std::is_same_v<T, Guide>) {
            j = {{"kind", "guide"}, {"owner", v.owner}, {"vertical", v.vertical}, {"pos", v.pos}};
        } else if constexpr (std::is_same_v<T, GenList>) {
            j = {{"kind", "list"}, {"name", v.name}, {"type", en(v.kind)}, {"pos", v.pos},
                 {"from_styles", v.from_styles}, {"placed_on", v.placed_on},
                 {"built_entries", v.built_entries}};
            if (!v.story.empty()) j["story"] = v.story;   // s087
            if (!v.entry_styles.empty()) j["entry_styles"] = v.entry_styles;   // s088
            if (!v.numbers) j["numbers"] = false;                              // s090
        }
        return j;
    }, r);
}

std::optional<Record> from(const json& j) {
    const std::string k = val<std::string>(j, "kind", "");
    if (k == "settings") {
        Settings v;
        v.title = val(j, "title", v.title);
        v.trim_w = val(j, "trim_w", v.trim_w);
        v.trim_h = val(j, "trim_h", v.trim_h);
        v.bleed = val(j, "bleed", v.bleed);
        v.facing = val(j, "facing", v.facing);
        v.wrap_beneath = val(j, "wrap_beneath", false);   // s069
        v.units = static_cast<Units>(std::clamp(val(j, "units", 0), 0, 3));   // s073
        v.note_restart = static_cast<NoteRestart>(std::clamp(val(j, "note_restart", 0), 0, 3));   // s124: chapter   // s111
        v.note_style = static_cast<NoteStyle>(std::clamp(val(j, "note_style", 0), 0, 3));
        v.note_area = std::clamp(val(j, "note_area", 100), 10, 100);   // s117
        v.note_rule = std::clamp(val(j, "note_rule", 33), 0, 100);   // s118
        v.note_rule_w = std::clamp(val(j, "note_rule_w", 0.5), 0.1, 6.0);
        v.note_space = std::clamp(val(j, "note_space", 0.0), 0.0, 36.0);
        v.note_cont = val(j, "note_cont", std::string{});
        v.note_at = static_cast<NoteAt>(std::clamp(val(j, "note_at", 0), 0, 2));   // s120
        v.note_title = val(j, "note_title", std::string("Notes"));
        v.note_num = static_cast<Settings::NoteNum>(std::clamp(val(j, "note_num", 0), 0, 3));   // s126
        if (j.contains("export")) {   // s032
            const auto& e = j.at("export");
            v.exp = {val(e, "print", true), val(e, "bleed", true), val(e, "marks", false),
                     val<std::string>(e, "profile", ""), val(e, "convert", false), val(e, "standard", 0),
                     val<std::string>(e, "range", ""), val(e, "bookmarks", true)};   // s077
            if (e.contains("mark_lists") && e.at("mark_lists").is_array())   // s082
                for (const auto& m : e.at("mark_lists"))
                    if (m.is_string()) v.exp.mark_lists.push_back(m.get<std::string>());
            v.exp.spreads = val(e, "spreads", false);   // s084
            v.exp.perceptual = val(e, "perceptual", false);   // s085
            v.exp.rebuild = val(e, "rebuild", true);          // s089
        }
        if (j.contains("queries") && j.at("queries").is_array())   // s078
            for (const auto& q : j.at("queries"))
                v.queries.push_back({val<std::string>(q, "name", ""), val<std::string>(q, "what", ""), val<std::string>(q, "with", ""),
                                     val(q, "case", false), val(q, "word", false), val(q, "mode", 0), val(q, "format", 0),
                                     val<std::string>(q, "in", ""), val<std::string>(q, "apply", "")});
        return v;
    }
    if (k == "section") {
        Section v;
        v.name = val<std::string>(j, "name", "");
        v.start_page = val<std::string>(j, "start_page", "");
        v.numbering = de(j, "numbering", Numbering::Arabic);
        v.start_at = val(j, "start_at", 1);
        v.starts = de(j, "starts", Side::Any);
        v.master = val<std::string>(j, "master", "");
        return v;
    }
    if (k == "page") {
        Page v;
        v.pos = val(j, "pos", 0.0);
        v.w = val(j, "w", v.w);
        v.h = val(j, "h", v.h);
        v.folds = val(j, "folds", std::vector<double>{});
        v.master = val<std::string>(j, "master", "");
        if (j.contains("margins") && j["margins"].is_array() && j["margins"].size() == 4) {   // s062
            const auto& a = j["margins"];
            v.margins = Margins{a[0].get<double>(), a[1].get<double>(), a[2].get<double>(), a[3].get<double>()};
        }
        return v;
    }
    if (k == "master") {
        Master v;
        v.prefix = val<std::string>(j, "prefix", "");
        v.name = val<std::string>(j, "name", "");
        v.pos = val(j, "pos", 0.0);
        v.w = val(j, "w", v.w);
        v.h = val(j, "h", v.h);
        v.folds = val(j, "folds", std::vector<double>{});
        v.columns = val(j, "columns", 1);
        v.based_on = val<std::string>(j, "based_on", "");
        if (j.contains("margins") && j.at("margins").is_array() && j.at("margins").size() == 4) {
            const auto& m = j.at("margins");
            v.margins = {m[0].get<double>(), m[1].get<double>(), m[2].get<double>(), m[3].get<double>()};
        }
        v.gutter = val(j, "gutter", 12.0);
        v.grid = val(j, "grid", 0.0);   // s036
        return v;
    }
    if (k == "frame") {
        Frame v;
        v.owner = val<std::string>(j, "owner", "");
        v.kind = de(j, "type", FrameKind::Text);
        v.x = val(j, "x", 0.0); v.y = val(j, "y", 0.0);
        v.w = val(j, "w", 0.0); v.h = val(j, "h", 0.0);
        v.rot = val(j, "rot", 0.0); v.z = val(j, "z", 0.0);
        v.story = val<std::string>(j, "story", "");
        v.next = val<std::string>(j, "next", "");
        v.asset = val<std::string>(j, "asset", "");
        v.fit = de(j, "fit", ImageFit::Fill);
        v.pic_x = val(j, "pic_x", 0.0);
        v.pic_y = val(j, "pic_y", 0.0);
        v.pic_w = val(j, "pic_w", 0.0);
        v.columns = val(j, "columns", 1);
        v.gutter = val(j, "gutter", 12.0);
        v.balance = val(j, "balance", false);
        v.widths = val(j, "widths", std::vector<double>{});
        v.fill = val<std::string>(j, "fill", "");
        v.stroke = val<std::string>(j, "stroke", "");
        v.stroke_w = val(j, "stroke_w", 1.0);
        v.fill_tint = val(j, "fill_tint", 1.0);
        v.stroke_tint = val(j, "stroke_tint", 1.0);
        v.opacity = val(j, "opacity", 1.0);
        v.side = de(j, "side", Side::Any);
        v.overrides = val<std::string>(j, "overrides", "");
        v.wrap = de(j, "wrap", Wrap::None);
        v.valign = de(j, "valign", VAlign::Top);   // s042
        v.autosize = val(j, "autosize", false);    // s048
        v.ignore_wrap = val(j, "ignore_wrap", false);   // s069
        if (j.contains("wrap_off") && j["wrap_off"].is_array() && j["wrap_off"].size() == 4) {
            const auto& a = j["wrap_off"];
            v.wrap_off = {a[0].get<double>(), a[1].get<double>(), a[2].get<double>(), a[3].get<double>()};
        }
        if (j.contains("inset") && j["inset"].is_array() && j["inset"].size() == 4) {   // s040
            const auto& a = j["inset"];
            v.inset = {a[0].get<double>(), a[1].get<double>(), a[2].get<double>(), a[3].get<double>()};
        }
        if (j.contains("col_rule") && j["col_rule"].is_object()) {   // s055
            const auto& r = j["col_rule"];
            v.col_rule = {val(r, "weight", 0.0), val<std::string>(r, "color", ""), val(r, "tint", 1.0), val(r, "inset", 0.0)};
        }
        return v;
    }
    if (k == "story") {
        Story v;
        v.name = val<std::string>(j, "name", "");
        if (j.contains("paras"))
            for (const auto& p : j.at("paras"))
            {
                Para para{val<std::string>(p, "style", ""), val<std::string>(p, "text", ""), {}};
                if (p.contains("runs"))
                    for (const auto& r : p.at("runs")) {
                        Run run{val(r, "start", 0), val(r, "end", 0), val<std::string>(r, "style", "")};
                        if (r.contains("local")) {
                            const auto& l = r.at("local");
                            run.local = {val(l, "bold", -1), val(l, "italic", -1), val(l, "size", 0.0), val<std::string>(l, "color", "")};
                        }
                        para.runs.push_back(run);
                    }
                para.restart = val(p, "restart", 0);   // s047
                para.brk = de(p, "brk", ParaStart::Anywhere);   // s055
                if (p.contains("own") && p.at("own").is_object()) {   // s093
                    const auto& o = p.at("own");
                    if (o.contains("align")) para.over.align = de(o, "align", Align::Left);
                    for (const auto& [k, slot] : {std::pair<const char*, std::optional<double>*>{"left_indent", &para.over.left_indent},
                                                  {"right_indent", &para.over.right_indent}, {"first_indent", &para.over.first_indent},
                                                  {"space_before", &para.over.space_before}, {"space_after", &para.over.space_after}})
                        if (o.contains(k) && o.at(k).is_number()) *slot = o.at(k).get<double>();
                    if (o.contains("keep_next") && o.at("keep_next").is_boolean()) para.over.keep_next = o.at("keep_next").get<bool>();   // s094
                    if (o.contains("hyphenate") && o.at("hyphenate").is_boolean()) para.over.hyphenate = o.at("hyphenate").get<bool>();
                    if (o.contains("keep_lines") && o.at("keep_lines").is_number_integer()) para.over.keep_lines = o.at("keep_lines").get<int>();
                }
                if (p.contains("notes") && p.at("notes").is_array())   // s109
                    for (const auto& n : p.at("notes")) {
                        Note note{val<std::string>(n, "text", ""), {}};
                        if (n.contains("runs"))
                            for (const auto& r : n.at("runs")) {
                                Run run{val(r, "start", 0), val(r, "end", 0), val<std::string>(r, "style", "")};
                                if (r.contains("local")) {
                                    const auto& l = r.at("local");
                                    run.local = {val(l, "bold", -1), val(l, "italic", -1), val(l, "size", 0.0), val<std::string>(l, "color", "")};
                                }
                                note.runs.push_back(run);
                            }
                        para.notes.push_back(std::move(note));
                    }
                fit_notes(para);   // a mark without its note (or the other way): made to match
                if (p.contains("refs") && p.at("refs").is_array())   // s128
                    for (const auto& r : p.at("refs"))
                        para.refs.push_back({val<std::string>(r, "to", ""), static_cast<RefShow>(std::clamp(val(r, "show", 0), 0, 4)),
                                             val<std::string>(r, "pattern", "")});   // s129
                if (p.contains("anchors") && p.at("anchors").is_array())
                    for (const auto& a : p.at("anchors")) para.anchors.push_back(a.is_string() ? a.get<std::string>() : std::string{});
                if (p.contains("index") && p.at("index").is_array())   // s134
                    for (const auto& m : p.at("index")) para.index.push_back({val<std::string>(m, "term", ""), val<std::string>(m, "sub", ""),
                                             val<std::string>(m, "see", ""), val(m, "also", false),
                                             static_cast<IndexSpan>(std::clamp(val(m, "span", 0), 0, 4)), val(m, "count", 1)});   // s136, s139
                fit_marks(para);
                v.paras.push_back(std::move(para));
            }
        return v;
    }
    if (k == "style") {
        Style v;
        v.name = val<std::string>(j, "name", "");
        v.kind = de(j, "type", StyleKind::Paragraph);
        v.pos = val(j, "pos", 0.0);
        v.family = val(j, "family", v.family);
        v.size = val(j, "size", v.size);
        v.leading = val(j, "leading", v.leading);
        v.bold = val(j, "bold", false);
        v.italic = val(j, "italic", false);
        v.no_break = val(j, "no_break", false);   // s075
        v.align = de(j, "align", Align::Left);
        v.space_before = val(j, "space_before", 0.0);
        v.space_after = val(j, "space_after", 0.0);
        v.color = val<std::string>(j, "color", "");
        v.color_tint = val(j, "color_tint", 1.0);
        v.left_indent = val(j, "left_indent", 0.0);
        v.right_indent = val(j, "right_indent", 0.0);
        v.first_indent = val(j, "first_indent", 0.0);
        v.skip_after_heading = val(j, "skip_after_heading", val(j, "indent_after_same", false));   // s026 files said indent_after_same
        v.drop_lines = val(j, "drop_lines", 0);
        v.drop_chars = val(j, "drop_chars", 1);
        v.align_grid = val(j, "align_grid", false);   // s036
        v.hyphenate = val(j, "hyphenate", false);     // s038
        v.hyph_limit = val(j, "hyph_limit", 2);        // s065
        v.hyph_shortest = val(j, "hyph_shortest", 5);
        v.hyph_last_word = val(j, "hyph_last_word", false);
        v.keep_next = val(j, "keep_next", false);     // s039
        v.keep_lines = val(j, "keep_lines", 0);
        v.start = de(j, "start", ParaStart::Anywhere);   // s043
        if (j.contains("tabs") && j["tabs"].is_array())
            for (const auto& t : j["tabs"])
                v.tabs.push_back({val(t, "pos", 0.0), de(t, "align", TabAlign::Left), val<std::string>(t, "leader", "")});
        for (const auto& [key, r] : {std::pair{"rule_above", &v.rule_above}, std::pair{"rule_below", &v.rule_below}})   // s044
            if (j.contains(key) && j[key].is_object()) {
                const auto& o = j[key];
                *r = {val(o, "weight", 0.0), val<std::string>(o, "color", ""), val(o, "tint", 1.0), val(o, "offset", 0.0), val(o, "text", false)};
            }
        if (j.contains("list") && j["list"].is_object()) {   // s045
            const auto& o = j["list"];
            v.list = {de(o, "type", ListType::None), val<std::string>(o, "bullet", "\u2022"), de(o, "numbering", Numbering::Arabic),
                      val<std::string>(o, "after", "."), val(o, "hang", 18.0), val(o, "continues", false), val(o, "level", 1)};
        }
        v.based_on = val<std::string>(j, "based_on", "");   // s037
        v.next = val<std::string>(j, "next", "");
        if (j.contains("own") && j["own"].is_array())
            for (const auto& k : j["own"]) if (k.is_string()) v.own.push_back(k.get<std::string>());
        std::sort(v.own.begin(), v.own.end());
        return v;
    }
    if (k == "swatch") {
        Swatch v;
        v.name = val<std::string>(j, "name", "");
        v.pos = val(j, "pos", 0.0);
        v.model = de(j, "model", ColorModel::CMYK);
        v.v = val(j, "v", std::array<double, 4>{});
        v.spot = val<std::string>(j, "spot", "");
        v.preview = val(j, "preview", v.preview);
        return v;
    }
    if (k == "asset") {
        Asset v;
        v.name = val<std::string>(j, "name", "");
        v.pos = val(j, "pos", 0.0);
        v.path = val<std::string>(j, "path", "");
        v.embedded = val(j, "embedded", false);
        v.color = val(j, "color", v.color);
        v.px_w = val(j, "px_w", 0);
        v.px_h = val(j, "px_h", 0);
        v.source = val<std::string>(j, "source", "");
        v.stamp = val(j, "stamp", std::int64_t{0});
        return v;
    }
    if (k == "guide") {
        Guide v;
        v.owner = val<std::string>(j, "owner", "");
        v.vertical = val(j, "vertical", true);
        v.pos = val(j, "pos", 0.0);
        return v;
    }
    if (k == "list") {
        GenList v;
        v.name = val<std::string>(j, "name", "");
        v.kind = de(j, "type", ListKind::Contents);
        v.pos = val(j, "pos", 0.0);
        v.from_styles = val(j, "from_styles", std::vector<std::string>{});
        v.placed_on = val<std::string>(j, "placed_on", "");
        v.built_entries = val(j, "built_entries", -1);
        v.story = val<std::string>(j, "story", "");   // s087
        v.numbers = val(j, "numbers", true);   // s090
        if (j.contains("entry_styles") && j.at("entry_styles").is_array())   // s088
            for (const auto& e : j.at("entry_styles")) v.entry_styles.push_back(e.is_string() ? e.get<std::string>() : std::string{});
        return v;
    }
    return std::nullopt;   // a kind this Composix doesn't know: skipped
}

}  // namespace

std::string to_json(const Doc& d) {
    json recs = json::array();
    for (const auto& [id, r] : d.records()) {   // std::map: stable id order, clean diffs
        json j = to(r);
        j["id"] = id;
        recs.push_back(std::move(j));
    }
    json root = {{"composix", kFormat}, {"records", recs}};
    return root.dump(2);
}

bool from_json(Doc& d, const std::string& text, std::string* err) {
    json root;
    try {
        root = json::parse(text);
    } catch (const std::exception& e) {
        if (err) *err = std::string("not JSON: ") + e.what();
        return false;
    }
    if (!root.is_object() || !root.contains("records") || !root.at("records").is_array()) {
        if (err) *err = "not a Composix document (no records)";
        return false;
    }
    if (val(root, "composix", 0) > kFormat) {
        if (err) *err = "written by a newer Composix (format " + std::to_string(val(root, "composix", 0)) + ")";
        return false;
    }
    try {
        d.reset();
        Doc::Loading load(d);
        for (const auto& j : root.at("records")) {
            const std::string id = val<std::string>(j, "id", "");
            if (id.empty()) continue;
            if (auto r = from(j)) d.put(id, *r);
        }
    } catch (const std::exception& e) {
        if (err) *err = std::string("bad record: ") + e.what();
        return false;
    }
    return true;
}

bool save_project(const Doc& d, const std::string& dir, std::string* err, const std::string& from_dir) {
    std::error_code ec;
    fs::create_directories(fs::path(dir) / "assets", ec);
    if (ec) { if (err) *err = ec.message(); return false; }
    // Save As: the embedded files come along (s009).
    if (!from_dir.empty() && fs::path(from_dir) != fs::path(dir)) {
        for (const auto& aid : d.ids_of<Asset>()) {
            const auto* a = d.as<Asset>(aid);
            if (!a->embedded || fs::path(a->path).is_absolute()) continue;
            const fs::path src = fs::path(from_dir) / a->path, dst = fs::path(dir) / a->path;
            if (!fs::exists(src, ec) || fs::exists(dst, ec)) continue;
            fs::create_directories(dst.parent_path(), ec);
            fs::copy_file(src, dst, ec);
            if (ec) { if (err) *err = "copying " + a->name + ": " + ec.message(); return false; }
        }
    }
    // Write beside, then rename: a crash mid-save never leaves half a document.
    const fs::path out = fs::path(dir) / "document.json";
    const fs::path tmp = fs::path(dir) / "document.json.tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { if (err) *err = "cannot write " + tmp.string(); return false; }
        f << to_json(d) << '\n';
        if (!f) { if (err) *err = "write failed: " + tmp.string(); return false; }
    }
    fs::rename(tmp, out, ec);
    if (ec) { if (err) *err = ec.message(); return false; }
    return true;
}

bool load_project(Doc& d, const std::string& dir, std::string* err) {
    const fs::path in = fs::path(dir) / "document.json";
    std::ifstream f(in, std::ios::binary);
    if (!f) { if (err) *err = "no document.json in " + dir; return false; }
    std::stringstream ss;
    ss << f.rdbuf();
    return from_json(d, ss.str(), err);
}

std::vector<UnusedFile> unused_asset_files(const Doc& d, const std::string& dir, const std::vector<std::string>& also_used) {   // s097
    std::vector<UnusedFile> out;
    if (dir.empty()) return out;
    std::error_code ec;
    const fs::path folder = fs::path(dir) / "assets";
    if (!fs::is_directory(folder, ec)) return out;
    std::set<std::string> used;
    const auto norm = [&](const std::string& p) {
        fs::path q(p);
        if (q.is_relative()) q = fs::path(dir) / q;
        return fs::weakly_canonical(q, ec).string();
    };
    for (const auto& id : d.ids_of<Asset>()) used.insert(norm(d.as<Asset>(id)->path));
    for (const auto& p : also_used) used.insert(norm(p));
    for (const auto& e : fs::directory_iterator(folder, ec)) {
        if (!e.is_regular_file(ec)) continue;
        if (used.count(norm(e.path().string()))) continue;
        out.push_back({e.path().string(), e.file_size(ec)});
    }
    std::sort(out.begin(), out.end(), [](const UnusedFile& a, const UnusedFile& b) { return a.path < b.path; });
    return out;
}

std::string bytes_text(std::uintmax_t n) {   // s097
    char b[32];
    if (n < 1024) return std::to_string(n) + " bytes";
    if (n < 1024 * 1024) { std::snprintf(b, sizeof b, "%.0f KB", static_cast<double>(n) / 1024); return b; }
    std::snprintf(b, sizeof b, "%.1f MB", static_cast<double>(n) / (1024.0 * 1024.0));
    return b;
}

ProjectSummary summarize_project(const std::string& dir, Doc* keep) {
    ProjectSummary out;
    Doc d;
    if (!load_project(d, dir)) return out;
    out.ok = true;
    out.title = d.settings().title.empty() ? fs::path(dir).filename().string() : d.settings().title;
    out.trim_w = d.settings().trim_w;
    out.units = d.settings().units;   // s096
    out.trim_h = d.settings().trim_h;
    out.pages = static_cast<int>(d.pages().size());
    std::error_code ec;
    const auto t = fs::last_write_time(fs::path(dir) / "document.json", ec);
    if (!ec) {
        const auto sys = std::chrono::time_point_cast<std::chrono::seconds>(
            t - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
        out.saved = sys.time_since_epoch().count();
    }
    if (keep) *keep = std::move(d);
    return out;
}

std::string ago_text(long long now, long long then) {
    const long long s = now - then;
    if (s < 60) return "just now";
    if (s < 3600) { const long long m = s / 60; return std::to_string(m) + (m == 1 ? " minute ago" : " minutes ago"); }
    if (s < 86400) { const long long h = s / 3600; return std::to_string(h) + (h == 1 ? " hour ago" : " hours ago"); }
    if (s < 2 * 86400) return "yesterday";
    if (s < 7 * 86400) return std::to_string(s / 86400) + " days ago";
    const std::time_t tt = static_cast<std::time_t>(then);
    std::tm tm{};
    localtime_r(&tt, &tm);
    char b[32];
    std::strftime(b, sizeof b, "%b %e, %Y", &tm);
    std::string r = b;
    if (const auto at = r.find("  "); at != std::string::npos) r.erase(at, 1);   // "Oct  3" -> "Oct 3"
    return r;
}

}  // namespace composix::core
