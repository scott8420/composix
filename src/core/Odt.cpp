// core/Odt -- see the header.
#include "core/Odt.hpp"
#include "core/Doc.hpp"
#include "core/Xml.hpp"
#include "core/Zip.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <sstream>

namespace composix::core::odt {

constexpr double kOdtSuper = 33;   // s262: "super" / "sub" -- LibreOffice's 33%

std::string split_features(std::string& family) {   // s266
    const auto c = family.find(':');
    if (c == std::string::npos) return {};
    std::string feats = family.substr(c + 1);
    family.erase(c);
    while (!family.empty() && family.back() == ' ') family.pop_back();
    return feats;
}

double length(const std::string& v, double fallback) {
    if (v.empty()) return fallback;
    char* end = nullptr;
    const double n = std::strtod(v.c_str(), &end);
    if (end == v.c_str()) return fallback;
    const std::string u(end);
    if (u == "pt") return n;
    if (u == "in" || u == "inch") return n * 72;
    if (u == "cm") return n * 72 / 2.54;
    if (u == "mm") return n * 72 / 25.4;
    if (u == "pc") return n * 12;
    if (u == "px") return n * 0.75;
    if (u.empty()) return n;
    return fallback;
}

bool pixel_size(const std::string& b, int& w, int& h) {   // s244
    w = h = 0;
    const auto u8 = [&](std::size_t i) { return i < b.size() ? static_cast<unsigned>(static_cast<unsigned char>(b[i])) : 0u; };
    if (b.size() >= 24 && b.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0) {
        w = static_cast<int>(u8(16) << 24 | u8(17) << 16 | u8(18) << 8 | u8(19));
        h = static_cast<int>(u8(20) << 24 | u8(21) << 16 | u8(22) << 8 | u8(23));
        return w > 0 && h > 0;
    }
    if (b.size() >= 10 && (b.compare(0, 6, "GIF87a") == 0 || b.compare(0, 6, "GIF89a") == 0)) {
        w = static_cast<int>(u8(6) | u8(7) << 8);
        h = static_cast<int>(u8(8) | u8(9) << 8);
        return w > 0 && h > 0;
    }
    if (u8(0) == 0xFF && u8(1) == 0xD8) {   // JPEG: to a start-of-frame
        std::size_t i = 2;
        while (i + 9 < b.size()) {
            if (u8(i) != 0xFF) { ++i; continue; }
            const unsigned m = u8(i + 1);
            if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7) || m == 0xFF) { ++i; continue; }
            const unsigned len = u8(i + 2) << 8 | u8(i + 3);
            if ((m >= 0xC0 && m <= 0xCF) && m != 0xC4 && m != 0xC8 && m != 0xCC) {
                h = static_cast<int>(u8(i + 5) << 8 | u8(i + 6));
                w = static_cast<int>(u8(i + 7) << 8 | u8(i + 8));
                return w > 0 && h > 0;
            }
            i += 2 + len;
        }
    }
    return false;
}

std::string base64(const std::string& t) {   // s244
    std::string out;
    unsigned buf = 0;
    int bits = 0;
    for (char c : t) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else continue;   // white space, '='
        buf = buf << 6 | static_cast<unsigned>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((buf >> bits) & 0xFF);
        }
    }
    return out;
}

Props Book::resolved(const std::string& name) const {
    Props out;
    const auto take = [&](const Props& p) {
        if (out.family.empty()) out.family = p.family;
        if (out.size < 0) out.size = p.size;
        if (out.bold < 0) out.bold = p.bold;
        if (out.italic < 0) out.italic = p.italic;
        if (out.color.empty()) out.color = p.color;
        if (out.align < 0) out.align = p.align;
        for (auto [o, i] : {std::pair{&out.left, p.left}, {&out.right, p.right}, {&out.indent, p.indent}, {&out.before, p.before}, {&out.after, p.after}})
            if (*o <= -1e8) *o = i;
        if (out.leading < 0 && out.leading_pct < 0) { out.leading = p.leading; out.leading_pct = p.leading_pct; }
        if (out.page_break < 0) out.page_break = p.page_break;
        if (out.keep_next < 0) out.keep_next = p.keep_next;   // s257
        if (out.letter <= -1e8) out.letter = p.letter;   // s262
        if (out.rise <= -1e8) { out.rise = p.rise; out.scale = p.scale; }
        if (out.caps < 0) out.caps = p.caps;
        if (out.underline < 0) out.underline = p.underline;
        if (out.strike < 0) out.strike = p.strike;
        if (out.features.empty()) out.features = p.features;   // s266
        if (out.kerning < 0) out.kerning = p.kerning;
        if (out.orphans < 0) out.orphans = p.orphans;
        if (out.widows < 0) out.widows = p.widows;
        if (out.master.empty()) out.master = p.master;
    };
    std::string fam;
    std::string at = name;
    for (int guard = 0; guard < 64 && !at.empty(); ++guard) {
        const auto it = styles.find(at);
        if (it == styles.end()) break;
        if (fam.empty()) fam = it->second.family;
        take(it->second.props);
        at = it->second.parent;
    }
    if (const auto d = defaults.find(fam.empty() ? "paragraph" : fam); d != defaults.end()) take(d->second);
    return out;
}

namespace {
using xml::Node;

// s244: a draw:frame holding a picture -- its anchor, place, size, wrap, file.
bool read_frame(const Node& k, const Book& b, Picture& pic) {
    const Node* img = k.child("draw:image");
    if (!img) return false;
    pic.name = k.attr_or("draw:name");
    pic.anchor = k.attr_or("text:anchor-type", "paragraph");
    pic.page = std::max(1, std::atoi(k.attr_or("text:anchor-page-number", "1").c_str()));
    pic.x = length(k.attr_or("svg:x"), 0);
    pic.y = length(k.attr_or("svg:y"), 0);
    pic.w = length(k.attr_or("svg:width"), 0);
    pic.h = length(k.attr_or("svg:height"), 0);
    auto it = b.graphics.find(k.attr_or("draw:style-name"));
    if (it == b.graphics.end()) it = b.graphics.find("S:" + k.attr_or("draw:style-name"));   // s248: a header's, from styles.xml
    if (it != b.graphics.end()) {
        pic.wrap = it->second.wrap;
        pic.hpos = it->second.hpos;
        pic.vpos = it->second.vpos;
        pic.whole_page = it->second.whole_page;
    }
    pic.href = img->attr_or("xlink:href");
    if (pic.href.rfind("./", 0) == 0) pic.href = pic.href.substr(2);
    if (const auto* bin = img->child("office:binary-data")) {   // a flat file: inline
        std::string t;
        for (const auto& q : bin->kids) if (q.is_text()) t += q.text;
        pic.bytes = base64(t);
    }
    return pic.w > 0 && pic.h > 0;
}

std::string unquote(std::string s) {
    if (s.size() >= 2 && (s.front() == '\'' || s.front() == '"') && s.back() == s.front()) s = s.substr(1, s.size() - 2);
    return s;
}

void read_props(const Node& st, Props& p, const std::map<std::string, std::string>& fonts) {
    if (const auto* pp = st.child("style:paragraph-properties")) {
        const auto al = pp->attr_or("fo:text-align");
        if (al == "start" || al == "left") p.align = 0;
        else if (al == "center") p.align = 1;
        else if (al == "end" || al == "right") p.align = 2;
        else if (al == "justify") p.align = 3;
        const auto L = [&](const char* k, double& v) { if (const auto* a = pp->attr(k)) v = length(*a, v); };
        L("fo:margin-left", p.left);
        L("fo:margin-right", p.right);
        L("fo:text-indent", p.indent);
        L("fo:margin-top", p.before);
        L("fo:margin-bottom", p.after);
        if (const auto* lh = pp->attr("fo:line-height")) {
            if (!lh->empty() && lh->back() == '%') p.leading_pct = std::strtod(lh->c_str(), nullptr);
            else if (*lh != "normal") p.leading = length(*lh, -1);
        }
        if (pp->attr_or("fo:break-before") == "page") p.page_break = 1;
        if (const auto* k = pp->attr("fo:keep-with-next")) p.keep_next = *k == "always" ? 1 : 0;   // s257
        if (const auto* o = pp->attr("fo:orphans")) p.orphans = std::max(0, std::atoi(o->c_str()));
        if (const auto* w = pp->attr("fo:widows")) p.widows = std::max(0, std::atoi(w->c_str()));
    }
    if (const auto* tp = st.child("style:text-properties")) {
        if (const auto* fn = tp->attr("style:font-name")) {
            const auto f = fonts.find(*fn);
            p.family = f != fonts.end() ? f->second : *fn;
        } else if (const auto* ff = tp->attr("fo:font-family")) {
            p.family = unquote(*ff);
        }
        if (!p.family.empty()) {   // s266: its features, apart
            const auto feats = split_features(p.family);
            if (!feats.empty()) p.features = feats;
        }
        if (const auto* lk = tp->attr("style:letter-kerning")) p.kerning = *lk == "false" ? 0 : 1;
        if (const auto* fs = tp->attr("fo:font-size"); fs && !fs->empty() && fs->back() != '%') p.size = length(*fs, -1);
        if (const auto* fw = tp->attr("fo:font-weight")) p.bold = (*fw == "bold" || std::atoi(fw->c_str()) >= 600) ? 1 : 0;
        if (const auto* fst = tp->attr("fo:font-style")) p.italic = (*fst == "italic" || *fst == "oblique") ? 1 : 0;
        if (const auto* c = tp->attr("fo:color"); c && c->size() == 7 && (*c)[0] == '#') p.color = *c;
        // s262: character attributes.
        if (const auto* ls = tp->attr("fo:letter-spacing")) p.letter = *ls == "normal" ? 0 : length(*ls, 0);
        if (const auto* tpos = tp->attr("style:text-position")) {
            std::istringstream in(*tpos);
            std::string a, s;
            in >> a >> s;
            if (a == "super") p.rise = kOdtSuper;
            else if (a == "sub") p.rise = -kOdtSuper;
            else p.rise = std::strtod(a.c_str(), nullptr);   // "33%", "-33%", "0%"
            p.scale = s.empty() ? (p.rise != 0 ? 58 : 100) : std::strtod(s.c_str(), nullptr);
        }
        const auto* fv = tp->attr("fo:font-variant");
        const auto* tt = tp->attr("fo:text-transform");
        if (fv && *fv == "small-caps") p.caps = 2;
        else if (tt && *tt == "uppercase") p.caps = 1;
        else if (fv || tt) p.caps = 0;
        if (const auto* u = tp->attr("style:text-underline-style")) p.underline = *u == "none" ? 0 : 1;
        if (const auto* k = tp->attr("style:text-line-through-style")) p.strike = *k == "none" ? 0 : 1;
    }
    if (const auto* m = st.attr("style:master-page-name"); m && !m->empty()) p.master = *m;
}

void read_styles(const Node* group, Book& b, bool automatic, const std::string& prefix = {}) {
    if (!group) return;
    for (const auto& k : group->kids) {
        if (k.name == "style:style" && k.attr_or("style:family") != "graphic") {
            Style s;
            s.name = prefix + k.attr_or("style:name");
            s.display = k.attr_or("style:display-name", s.name);
            s.family = k.attr_or("style:family");
            s.parent = k.attr_or("style:parent-style-name");   // (named styles: no prefix -- they're shared)
            s.next = k.attr_or("style:next-style-name");
            s.outline = std::atoi(k.attr_or("style:default-outline-level", "0").c_str());
            s.automatic = automatic;
            read_props(k, s.props, b.fonts);
            if (!s.name.empty() && (s.family == "paragraph" || s.family == "text")) b.styles[s.name] = std::move(s);
        } else if (k.name == "style:default-style") {
            read_props(k, b.defaults[k.attr_or("style:family")], b.fonts);
        } else if (k.name == "style:style" && k.attr_or("style:family") == "graphic") {   // s244: a picture's frame style
            Picture g;
            if (const auto* gp = k.child("style:graphic-properties")) {
                const auto w = gp->attr_or("style:wrap", "none");
                g.wrap = w == "run-through" ? 0 : (w == "none" ? 2 : 1);
                const auto hp = gp->attr_or("style:horizontal-pos", "from-left");
                g.hpos = hp == "left" ? 1 : hp == "center" ? 2 : hp == "right" ? 3 : 0;
                const auto vp = gp->attr_or("style:vertical-pos", "from-top");
                g.vpos = vp == "top" ? 1 : vp == "middle" ? 2 : vp == "bottom" ? 3 : 0;
                const auto hr = gp->attr_or("style:horizontal-rel");
                g.whole_page = hr == "page" || hr == "page-content" ? hr == "page" : false;
            }
            b.graphics[prefix + k.attr_or("style:name")] = g;
        } else if (k.name == "text:list-style") {   // s243
            ListStyle L;
            L.name = prefix + k.attr_or("style:name");
            for (const auto& lv : k.kids) {
                if (lv.name != "text:list-level-style-bullet" && lv.name != "text:list-level-style-number") continue;
                const int n = std::clamp(std::atoi(lv.attr_or("text:level", "1").c_str()), 1, 10);
                if (static_cast<int>(L.levels.size()) < n) L.levels.resize(static_cast<std::size_t>(n));
                ListLevel& v = L.levels[static_cast<std::size_t>(n - 1)];
                v.number = lv.name == "text:list-level-style-number";
                if (!v.number) v.bullet = lv.attr_or("text:bullet-char", "\u2022");
                v.format = lv.attr_or("style:num-format", "1");
                v.prefix = lv.attr_or("style:num-prefix");
                v.suffix = lv.attr_or("style:num-suffix", v.number ? "." : "");
                v.display = std::clamp(std::atoi(lv.attr_or("text:display-levels", "1").c_str()), 1, 10);   // s251
                v.text_style = lv.attr_or("text:style-name");
                if (const auto* pp = lv.child("style:list-level-properties")) {
                    if (const auto* la = pp->child("style:list-level-label-alignment")) {   // ODF 1.2's
                        v.indent = length(la->attr_or("fo:margin-left"), -1);
                        const double ti = length(la->attr_or("fo:text-indent"), 0);
                        if (ti < 0) v.label = -ti;
                    } else {   // the older
                        const double sb = length(pp->attr_or("text:space-before"), 0), ml = length(pp->attr_or("text:min-label-width"), -1);
                        if (ml >= 0) { v.label = ml; v.indent = sb + ml; }
                    }
                }
            }
            if (!L.name.empty()) b.lists[L.name] = std::move(L);
        } else if (k.name == "style:page-layout") {
            PageLayout L;
            if (const auto* pl = k.child("style:page-layout-properties")) {
                L.w = length(pl->attr_or("fo:page-width"), 0);
                L.h = length(pl->attr_or("fo:page-height"), 0);
                L.top = length(pl->attr_or("fo:margin-top"), -1);
                L.bottom = length(pl->attr_or("fo:margin-bottom"), -1);
                L.left = length(pl->attr_or("fo:margin-left"), -1);
                L.right = length(pl->attr_or("fo:margin-right"), -1);
                if (const auto* cs = pl->child("style:columns")) {   // s289
                    L.columns = std::clamp(std::atoi(cs->attr_or("fo:column-count", "1").c_str()), 1, 12);
                    L.column_gap = std::max(0.0, length(cs->attr_or("fo:column-gap"), 0));
                }
            }
            if (const auto* hs = k.child("style:header-style"))   // s288
                if (const auto* hp = hs->child("style:header-footer-properties")) {
                    L.head = true;
                    L.head_min = std::max(0.0, length(hp->attr_or("fo:min-height", hp->attr_or("svg:height")), 0));
                    L.head_gap = std::max(0.0, length(hp->attr_or("fo:margin-bottom"), 0));
                }
            if (const auto* fs = k.child("style:footer-style"))
                if (const auto* fp = fs->child("style:header-footer-properties")) {
                    L.foot = true;
                    L.foot_min = std::max(0.0, length(fp->attr_or("fo:min-height", fp->attr_or("svg:height")), 0));
                    L.foot_gap = std::max(0.0, length(fp->attr_or("fo:margin-top"), 0));
                }
            L.mirrored = k.attr_or("style:page-usage") == "mirrored";
            L.side = k.attr_or("style:page-usage") == "right" ? 1 : k.attr_or("style:page-usage") == "left" ? 2 : 0;   // s268
            b.layouts[k.attr_or("style:name")] = L;
        }
    }
}

// The words of a paragraph-like element into `p`: spans, spaces collapsed
// as ODF says (runs of white space are one space; text:s is n spaces).
// s286: an index mark as Composix keeps it -- under key1 when there is one.
IndexMark index_mark(const std::string& entry, const std::string& k1, const std::string& k2) {
    IndexMark m;
    if (k1.empty()) { m.term = entry; return m; }
    m.term = k1;
    m.sub = k2.empty() ? entry : entry.empty() ? k2 : k2 + ", " + entry;
    return m;
}

struct Gather {
    Book& b;
    Para& p;
    bool  last_space = true;   // the paragraph's start counts as after a space
    struct OpenMark { std::size_t at = 0, from = 0; std::string entry, k1, k2; };
    std::map<std::string, OpenMark> open_marks;   // s286: index marks started, by id
    void text(const std::string& t) {
        for (char c : t) {
            const bool sp = c == ' ' || c == '\n' || c == '\t' || c == '\r';
            if (sp) {
                if (!last_space) p.text += ' ';
                last_space = true;
            } else {
                p.text += c;
                last_space = false;
            }
        }
    }
    void raw(const std::string& t) { p.text += t; last_space = false; }
    void node(const Node& n, const std::string& span_style) {
        for (const auto& k : n.kids) {
            if (k.is_text()) { text(k.text); continue; }
            const auto& nm = k.name;
            if (nm == "text:span" || nm == "text:a" || nm == "text:ruby" || nm == "text:ruby-base" || nm == "text:meta") {
                const std::string st = nm == "text:span" ? k.attr_or("text:style-name") : span_style;
                const int a = static_cast<int>(p.text.size());
                node(k, st);
                const int e = static_cast<int>(p.text.size());
                if (nm == "text:span" && !st.empty() && e > a) p.spans.push_back({a, e, st});
            } else if (nm == "text:s") {
                const int c = std::max(1, std::atoi(k.attr_or("text:c", "1").c_str()));
                p.text.append(static_cast<std::size_t>(std::min(c, 1000)), ' ');
                last_space = true;
            } else if (nm == "text:tab") {
                raw("\t");
                last_space = true;
            } else if (nm == "text:line-break") {
                raw("\n");
                last_space = true;
            } else if (nm == "text:soft-hyphen") {
                raw("\xC2\xAD");
            } else if (nm == "text:note") {
                Note note;
                note.endnote = k.attr_or("text:note-class") == "endnote";
                if (const auto* body = k.child("text:note-body")) {
                    bool first = true;
                    for (const auto& q : body->kids) {
                        if (q.name != "text:p" && q.name != "text:h") continue;
                        Para np;
                        Gather g{b, np};
                        g.node(q, {});
                        if (!first) note.text += '\n';
                        const int off = static_cast<int>(note.text.size());
                        note.text += np.text;
                        for (auto s : np.spans) { s.start += off; s.end += off; note.spans.push_back(s); }
                        first = false;
                    }
                }
                raw(kMarkNote);
                p.notes.push_back(std::move(note));
            } else if (nm == "text:alphabetical-index-mark" || nm == "text:alphabetical-index-mark-start") {   // s286
                std::string entry = k.attr_or("text:string-value");
                const std::string k1 = k.attr_or("text:key1"), k2 = k.attr_or("text:key2");
                raw(kMarkIndex);
                p.index.push_back({});
                if (nm == "text:alphabetical-index-mark-start")   // its words: up to the end mark of the same id
                    open_marks[k.attr_or("text:id")] = {p.index.size() - 1, p.text.size(), entry, k1, k2};
                else
                    p.index.back() = index_mark(entry, k1, k2);
            } else if (nm == "text:bibliography-mark") {   // s291: a citation -- its source by identifier; the shown words dropped (Composix numbers it)
                const std::string id = k.attr_or("text:identifier");
                if (!id.empty()) {
                    if (!b.sources.count(id)) {
                        Book::Source src;
                        src.type = k.attr_or("text:bibliography-type");
                        src.author = k.attr_or("text:author");
                        src.title = k.attr_or("text:title");
                        src.year = k.attr_or("text:year");
                        src.publisher = k.attr_or("text:publisher");
                        src.journal = k.attr_or("text:journal");
                        src.volume = k.attr_or("text:volume");
                        src.pages = k.attr_or("text:pages");
                        src.url = k.attr_or("text:url");
                        b.sources[id] = src;
                        b.source_order.push_back(id);
                    }
                    raw(kMarkRef);
                    p.cites.push_back(id);
                }
            } else if (nm == "text:alphabetical-index-mark-end") {
                const auto it = open_marks.find(k.attr_or("text:id"));
                if (it != open_marks.end()) {
                    auto& om = it->second;
                    std::string words = om.entry;
                    if (words.empty()) {   // the text between, without the marks
                        const std::string between = p.text.substr(std::min(om.from, p.text.size()));
                        for (std::size_t i = 0; i < between.size();) {
                            const auto c = static_cast<unsigned char>(between[i]);
                            if (c == 0xEE && i + 2 < between.size()) { i += 3; continue; }   // a mark (U+E000..)
                            words += between[i++];
                        }
                        while (!words.empty() && words.back() == ' ') words.pop_back();
                        while (!words.empty() && words.front() == ' ') words.erase(words.begin());
                    }
                    if (om.at < p.index.size()) p.index[om.at] = index_mark(words, om.k1, om.k2);
                    open_marks.erase(it);
                }
            } else if (nm == "text:page-number") {
                raw(kMarkPage);
            } else if (nm == "text:author-name" || nm == "text:initial-creator" || nm == "text:creator") {   // s239
                raw(kMarkAuthor);
            } else if (nm == "text:title") {
                raw(kMarkTitle);
            } else if (nm == "text:chapter") {   // s240: the chapter's (level 1) or its subtitle's (deeper) words
                raw(std::atoi(k.attr_or("text:outline-level", "1").c_str()) <= 1 ? kMarkChapter : kMarkSubhead);
            } else if (nm == "draw:frame" || nm == "draw:image") {
                ++b.pictures;
                if (Picture pic; nm == "draw:frame" && read_frame(k, b, pic)) {   // s244
                    pic.at = static_cast<int>(p.text.size());
                    p.pictures.push_back(std::move(pic));
                }
            } else if (nm == "text:bookmark" || nm == "text:bookmark-start" || nm == "text:bookmark-end" || nm == "text:soft-page-break" ||
                       nm == "office:annotation" || nm == "office:annotation-end" || nm == "text:sequence-ref" || nm == "text:reference-mark") {
                // nothing to keep
            } else {
                node(k, span_style);   // a field: its shown text (dates, titles...)
            }
        }
    }
};

struct ListAt { std::string style; int level = 0; bool header = false; int start = 0; };

void read_block(const Node& n, Book& b, std::vector<Para>& out, ListAt list = {}) {
    for (const auto& k : n.kids) {
        if (k.is_text()) continue;
        const auto& nm = k.name;
        if (nm == "text:p" || nm == "text:h") {
            Para p;
            p.style = k.attr_or("text:style-name");
            if (nm == "text:h") p.outline = std::max(1, std::atoi(k.attr_or("text:outline-level", "1").c_str()));
            if (list.level > 0 && !list.header && nm == "text:p") {   // s243: an item's paragraph -- numbered / bulleted
                p.list = list.style;
                p.list_level = list.level;
                p.start = list.start;
                list.start = 0;   // (an item's later paragraphs aren't numbered again)
                list.header = true;
            }
            Gather g{b, p};
            g.node(k, {});
            while (!p.text.empty() && p.text.back() == ' ' && p.notes.empty()) p.text.pop_back();   // a trailing collapsed space
            if (&out == &b.body)   // s244: the body's pictures, anchored to this paragraph (or a page)
                for (auto& pic : p.pictures) {
                    if (pic.anchor != "page") pic.para = static_cast<int>(out.size());
                    pic.at = std::min(pic.at, static_cast<int>(p.text.size()));
                    b.images.push_back(std::move(pic));
                }
            if (&out == &b.body) p.pictures.clear();   // s248: a header's / footer's kept on its paragraph
            out.push_back(std::move(p));
        } else if (nm == "text:list") {   // s243: a list (inside one: a level deeper), its style or its parent's
            ListAt in = list;
            in.style = k.attr_or("text:style-name", list.style);
            in.level = list.level + 1;
            in.header = false;
            read_block(k, b, out, in);
        } else if (nm == "text:list-item" || nm == "text:list-header") {
            ListAt in = list;
            in.header = nm == "text:list-header";
            in.start = std::atoi(k.attr_or("text:start-value", "0").c_str());
            read_block(k, b, out, in);
        } else if (nm == "text:section" || nm == "text:index-body") {
            read_block(k, b, out, list);
        } else if (nm == "table:table") {
            ++b.tables;
        } else if (nm == "text:table-of-content" && &out == &b.body) {   // s271: kept -- made a contents list on import
            Toc t;
            t.at = static_cast<int>(out.size());
            if (const auto* src = k.child("text:table-of-content-source")) {
                t.levels = std::clamp(std::atoi(src->attr_or("text:outline-level", "10").c_str()), 1, 10);
                if (const auto* tt = src->child("text:index-title-template")) {
                    t.title_style = tt->attr_or("text:style-name");
                    for (const auto& x : tt->kids) if (x.is_text()) t.title += x.text;
                }
                for (const auto* e : src->children("text:table-of-content-entry-template"))
                    if (e->attr_or("text:outline-level") == "1") {
                        t.entry_style = e->attr_or("text:style-name");
                        if (const auto* ts = e->child("text:index-entry-tab-stop"); ts && ts->attr_or("style:type") == "right") {   // s272
                            t.right_tab = true;
                            t.leader = ts->attr_or("style:leader-char");
                            if (t.leader == " ") t.leader.clear();
                        }
                    }
            }
            if (const auto* body = k.child("text:index-body")) {
                if (const auto* it = body->child("text:index-title"))
                    if (const auto* p = it->child("text:p")) {
                        std::string words;
                        std::function<void(const Node&)> collect = [&](const Node& x) {
                            if (x.is_text()) { words += x.text; return; }
                            for (const auto& y : x.kids) collect(y);
                        };
                        collect(*p);
                        if (!words.empty()) t.title = words;
                        t.title_style = p->attr_or("text:style-name", t.title_style);
                    }
                t.entries = static_cast<int>(body->children("text:p").size());
            }
            b.tocs.push_back(std::move(t));
        } else if (nm == "text:alphabetical-index" && &out == &b.body) {   // s286: kept -- an index made on import
            Book::Index ix;
            ix.at = static_cast<int>(out.size());
            if (const auto* src = k.child("text:alphabetical-index-source"))
                if (const auto* tt = src->child("text:index-title-template")) {
                    ix.title_style = tt->attr_or("text:style-name");
                    for (const auto& x : tt->kids) if (x.is_text()) ix.title += x.text;
                }
            if (const auto* body = k.child("text:index-body"))
                if (const auto* it = body->child("text:index-title"))
                    if (const auto* p = it->child("text:p")) {
                        std::string words;
                        std::function<void(const Node&)> collect = [&](const Node& x) {
                            if (x.is_text()) { words += x.text; return; }
                            for (const auto& y : x.kids) collect(y);
                        };
                        collect(*p);
                        if (!words.empty()) ix.title = words;
                        ix.title_style = p->attr_or("text:style-name", ix.title_style);
                    }
            b.indexes.push_back(std::move(ix));
        } else if (nm == "text:bibliography" && &out == &b.body) {   // s291: kept -- a bibliography made on import
            Book::Index bx;
            bx.at = static_cast<int>(out.size());
            if (const auto* src = k.child("text:bibliography-source"))
                if (const auto* tt = src->child("text:index-title-template")) {
                    bx.title_style = tt->attr_or("text:style-name");
                    for (const auto& x : tt->kids) if (x.is_text()) bx.title += x.text;
                }
            if (const auto* body = k.child("text:index-body"))
                if (const auto* it = body->child("text:index-title"))
                    if (const auto* p = it->child("text:p")) {
                        std::string words;
                        std::function<void(const Node&)> collect = [&](const Node& x) {
                            if (x.is_text()) { words += x.text; return; }
                            for (const auto& y : x.kids) collect(y);
                        };
                        collect(*p);
                        if (!words.empty()) bx.title = words;
                        bx.title_style = p->attr_or("text:style-name", bx.title_style);
                    }
            b.bibliographies.push_back(std::move(bx));
        } else if (nm == "text:table-of-content" || nm == "text:alphabetical-index" || nm == "text:bibliography") {
            ++b.contents;
        } else if (nm == "draw:frame" || nm == "draw:custom-shape" || nm == "draw:g") {
            ++b.pictures;
            if (Picture pic; &out == &b.body && nm == "draw:frame" && read_frame(k, b, pic)) {   // s244: on a page (body level)
                if (pic.anchor != "page") pic.para = static_cast<int>(out.size());   // (its next paragraph)
                b.images.push_back(std::move(pic));
            }
        }
    }
}

std::optional<Node> load(const std::string& zip, const char* name, std::string* err) {
    bool ok = false;
    const std::string x = zip::entry(zip, name, &ok);
    if (!ok) return std::nullopt;
    std::string e;
    auto n = xml::parse(x, &e);
    if (!n && err) *err = std::string(name) + ": " + e;
    return n;
}
}  // namespace

std::optional<Book> read(const std::string& bytes, std::string* err) {
    std::optional<Node> content, styles, meta;
    if (bytes.compare(0, 4, "PK\x03\x04") == 0) {
        bool ok = false;
        const auto mime = zip::entry(bytes, "mimetype", &ok);
        if (ok && mime.find("opendocument.text") == std::string::npos) {
            if (err) *err = "not a text document (" + mime + ")";
            return std::nullopt;
        }
        content = load(bytes, "content.xml", err);
        if (!content) {
            if (err && err->empty()) *err = "no content.xml -- not an OpenDocument file";
            return std::nullopt;
        }
        styles = load(bytes, "styles.xml", nullptr);
        meta = load(bytes, "meta.xml", nullptr);
    } else {   // a flat .fodt: all in one
        std::string e;
        content = xml::parse(bytes, &e);
        if (!content || content->name != "office:document") {
            if (err) *err = content ? "not an OpenDocument file" : e;
            return std::nullopt;
        }
        styles = content;
        meta = content;
    }
    Book b;
    const auto fonts = [&](const Node* root) {
        if (!root) return;
        if (const auto* fd = root->child("office:font-face-decls"))
            for (const auto* ff : fd->children("style:font-face"))
                b.fonts[ff->attr_or("style:name")] = unquote(ff->attr_or("svg:font-family", ff->attr_or("style:name")));
    };
    fonts(styles ? &*styles : nullptr);
    fonts(&*content);
    if (styles) {
        read_styles(styles->child("office:styles"), b, false);
        // styles.xml's own automatic styles (the headers' and footers') share names with
        // content.xml's ("P1"...): kept apart, "S:" before theirs
        read_styles(styles->child("office:automatic-styles"), b, true, "S:");
        if (const auto* ms = styles->child("office:master-styles"))
            for (const auto* m : ms->children("style:master-page")) {
                MasterPage mp;
                mp.name = m->attr_or("style:name");
                mp.display = m->attr_or("style:display-name", mp.name);
                mp.layout = m->attr_or("style:page-layout-name");
                mp.next = m->attr_or("style:next-style-name");
                const auto part = [&](const char* tag, std::vector<Para>& into) {
                    if (const auto* h = m->child(tag); h && h->attr_or("style:display") != "false") read_block(*h, b, into);
                    const auto own = [&](std::string& st) { if (b.styles.count("S:" + st)) st = "S:" + st; };
                    for (auto& p : into) {
                        own(p.style);
                        for (auto& s : p.spans) own(s.style);
                    }
                };
                part("style:header", mp.header);
                part("style:footer", mp.footer);
                part("style:header-left", mp.header_left);
                part("style:footer-left", mp.footer_left);
                b.masters.push_back(std::move(mp));
            }
    }
    if (&*content != (styles ? &*styles : nullptr)) {
        read_styles(content->child("office:styles"), b, false);   // (a flat file has them here already)
    }
    read_styles(content->child("office:automatic-styles"), b, true);
    if (const auto* body = content->child("office:body"))
        if (const auto* text = body->child("office:text")) read_block(*text, b, b.body);
    const auto fetch = [&](Picture& pic) {   // s244: their files, from the zip
        if (pic.bytes.empty() && !pic.href.empty() && bytes.compare(0, 4, "PK\x03\x04") == 0 && pic.href.find("://") == std::string::npos &&
            pic.href.rfind("../", 0) != 0)
            pic.bytes = zip::entry(bytes, pic.href);
    };
    for (auto& pic : b.images) fetch(pic);
    for (auto& mp : b.masters)   // s248: the headers' and footers' too
        for (auto* part : {&mp.header, &mp.footer, &mp.header_left, &mp.footer_left})
            for (auto& p : *part)
                for (auto& pic : p.pictures) fetch(pic);
    if (meta)
        if (const auto* m = meta->child("office:meta")) {
            const auto words = [](const Node* n) {
                std::string s;
                if (n) for (const auto& k : n->kids) if (k.is_text()) s += k.text;
                return s;
            };
            b.title = words(m->child("dc:title"));
            b.author = words(m->child("meta:initial-creator"));
            if (b.author.empty()) b.author = words(m->child("dc:creator"));
        }
    return b;
}

}  // namespace composix::core::odt
