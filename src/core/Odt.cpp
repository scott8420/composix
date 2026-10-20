// core/Odt -- see the header.
#include "core/Odt.hpp"
#include "core/Doc.hpp"
#include "core/Xml.hpp"
#include "core/Zip.hpp"

#include <cmath>
#include <cstdlib>

namespace composix::core::odt {

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
    }
    if (const auto* tp = st.child("style:text-properties")) {
        if (const auto* fn = tp->attr("style:font-name")) {
            const auto f = fonts.find(*fn);
            p.family = f != fonts.end() ? f->second : *fn;
        } else if (const auto* ff = tp->attr("fo:font-family")) {
            p.family = unquote(*ff);
        }
        if (const auto* fs = tp->attr("fo:font-size"); fs && !fs->empty() && fs->back() != '%') p.size = length(*fs, -1);
        if (const auto* fw = tp->attr("fo:font-weight")) p.bold = (*fw == "bold" || std::atoi(fw->c_str()) >= 600) ? 1 : 0;
        if (const auto* fst = tp->attr("fo:font-style")) p.italic = (*fst == "italic" || *fst == "oblique") ? 1 : 0;
        if (const auto* c = tp->attr("fo:color"); c && c->size() == 7 && (*c)[0] == '#') p.color = *c;
    }
    if (const auto* m = st.attr("style:master-page-name"); m && !m->empty()) p.master = *m;
}

void read_styles(const Node* group, Book& b, bool automatic, const std::string& prefix = {}) {
    if (!group) return;
    for (const auto& k : group->kids) {
        if (k.name == "style:style") {
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
        } else if (k.name == "style:page-layout") {
            PageLayout L;
            if (const auto* pl = k.child("style:page-layout-properties")) {
                L.w = length(pl->attr_or("fo:page-width"), 0);
                L.h = length(pl->attr_or("fo:page-height"), 0);
                L.top = length(pl->attr_or("fo:margin-top"), -1);
                L.bottom = length(pl->attr_or("fo:margin-bottom"), -1);
                L.left = length(pl->attr_or("fo:margin-left"), -1);
                L.right = length(pl->attr_or("fo:margin-right"), -1);
            }
            L.mirrored = k.attr_or("style:page-usage") == "mirrored";
            b.layouts[k.attr_or("style:name")] = L;
        }
    }
}

// The words of a paragraph-like element into `p`: spans, spaces collapsed
// as ODF says (runs of white space are one space; text:s is n spaces).
struct Gather {
    Book& b;
    Para& p;
    bool  last_space = true;   // the paragraph's start counts as after a space
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
            } else if (nm == "text:page-number") {
                raw(kMarkPage);
            } else if (nm == "text:chapter") {
                raw(kMarkSection);
            } else if (nm == "draw:frame" || nm == "draw:image") {
                ++b.pictures;
            } else if (nm == "text:bookmark" || nm == "text:bookmark-start" || nm == "text:bookmark-end" || nm == "text:soft-page-break" ||
                       nm == "office:annotation" || nm == "office:annotation-end" || nm == "text:sequence-ref" || nm == "text:reference-mark") {
                // nothing to keep
            } else {
                node(k, span_style);   // a field: its shown text (dates, titles...)
            }
        }
    }
};

void read_block(const Node& n, Book& b, std::vector<Para>& out) {
    for (const auto& k : n.kids) {
        if (k.is_text()) continue;
        const auto& nm = k.name;
        if (nm == "text:p" || nm == "text:h") {
            Para p;
            p.style = k.attr_or("text:style-name");
            if (nm == "text:h") p.outline = std::max(1, std::atoi(k.attr_or("text:outline-level", "1").c_str()));
            Gather g{b, p};
            g.node(k, {});
            while (!p.text.empty() && p.text.back() == ' ' && p.notes.empty()) p.text.pop_back();   // a trailing collapsed space
            out.push_back(std::move(p));
        } else if (nm == "text:list" || nm == "text:list-item" || nm == "text:list-header" || nm == "text:section" ||
                   nm == "text:index-body") {
            read_block(k, b, out);
        } else if (nm == "table:table") {
            ++b.tables;
        } else if (nm == "text:table-of-content" || nm == "text:alphabetical-index" || nm == "text:bibliography") {
            ++b.contents;
        } else if (nm == "draw:frame" || nm == "draw:custom-shape" || nm == "draw:g") {
            ++b.pictures;
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
