// layout::load_big_book -- the generated long test book (s100). See the header.
#include "layout/BigBook.hpp"
#include "layout/Flow.hpp"

#include <cairo.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace composix::layout {
namespace {

using core::Doc;
using core::kPt;

// ── greeking: deterministic lorem ipsum ─────────────────────────────────────
class Greek {
public:
    explicit Greek(unsigned seed) : m_s(seed * 2654435761u + 1) {}
    unsigned next() {   // xorshift32: the same stream on every machine
        m_s ^= m_s << 13;
        m_s ^= m_s >> 17;
        m_s ^= m_s << 5;
        return m_s;
    }
    int range(int lo, int hi) { return lo + static_cast<int>(next() % static_cast<unsigned>(hi - lo + 1)); }
    std::string word() { return kWords[next() % (sizeof kWords / sizeof *kWords)]; }
    std::string title(int n) {   // "Portus Ventorum"
        std::string t;
        for (int i = 0; i < n; ++i) {
            std::string w = word();
            w[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(w[0])));
            t += (i ? " " : "") + w;
        }
        return t;
    }
    // A sentence of 6..18 words; `emph`: the byte span of a phrase in it to
    // set in Emphasis (empty when none).
    std::string sentence(int* emph_a = nullptr, int* emph_b = nullptr) {
        const int n = range(6, 18);
        const int e = (emph_a && next() % 9 == 0) ? range(1, n - 2) : -1;
        std::string s;
        for (int i = 0; i < n; ++i) {
            std::string w = word();
            if (i == 0) w[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(w[0])));
            if (i) s += ' ';
            if (i == e) *emph_a = static_cast<int>(s.size());
            s += w;
            if (i == e + 1 && e >= 0) *emph_b = static_cast<int>(s.size());
            if (i + 1 < n && i > 2 && next() % 7 == 0) s += ',';
        }
        s += (next() % 11 == 0) ? '?' : '.';
        return s;
    }

private:
    unsigned m_s;
    static constexpr const char* kWords[] = {
        "lorem", "ipsum", "dolor", "sit", "amet", "consectetur", "adipiscing", "elit", "sed", "do",
        "eiusmod", "tempor", "incididunt", "ut", "labore", "et", "dolore", "magna", "aliqua", "enim",
        "ad", "minim", "veniam", "quis", "nostrud", "exercitation", "ullamco", "laboris", "nisi",
        "aliquip", "ex", "ea", "commodo", "consequat", "duis", "aute", "irure", "in", "reprehenderit",
        "voluptate", "velit", "esse", "cillum", "fugiat", "nulla", "pariatur", "excepteur", "sint",
        "occaecat", "cupidatat", "non", "proident", "sunt", "culpa", "qui", "officia", "deserunt",
        "mollit", "anim", "id", "est", "laborum", "portus", "navis", "ventus", "litus", "mare",
        "insula", "nauta", "tabula", "via", "terra", "caelum", "lux", "nox", "aurora", "flumen",
        "mercator", "aurantium", "oleum", "fumus", "turris", "signum", "custos", "longinquus",
        "praeterea", "quoniam", "tamen", "itaque", "autem", "vero", "quidem", "semper", "numquam",
    };
};

// A plate: sky, sun, three ridges -- a stand-in landscape in its own hues.
bool draw_plate(const std::string& path, int k, int w, int h) {
    cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
    cairo_t* cr = cairo_create(s);
    const double hue = std::fmod(k * 0.173, 1.0);
    auto col = [&](double l, double sat, double shift) {   // hsl-ish, cheap
        const double hh = std::fmod(hue + shift + 1, 1.0) * 6;
        const double c = sat * (1 - std::abs(2 * l - 1));
        const double x = c * (1 - std::abs(std::fmod(hh, 2) - 1));
        double r = 0, g = 0, b = 0;
        if (hh < 1) { r = c; g = x; } else if (hh < 2) { r = x; g = c; } else if (hh < 3) { g = c; b = x; }
        else if (hh < 4) { g = x; b = c; } else if (hh < 5) { r = x; b = c; } else { r = c; b = x; }
        const double m = l - c / 2;
        cairo_set_source_rgb(cr, r + m, g + m, b + m);
    };
    cairo_pattern_t* sky = cairo_pattern_create_linear(0, 0, 0, h);
    cairo_pattern_add_color_stop_rgb(sky, 0, 0.55 + 0.3 * hue, 0.7, 0.9 - 0.3 * hue);
    cairo_pattern_add_color_stop_rgb(sky, 1, 0.98, 0.9, 0.78);
    cairo_set_source(cr, sky);
    cairo_paint(cr);
    cairo_pattern_destroy(sky);
    col(0.85, 0.9, 0.08);
    cairo_arc(cr, w * (0.25 + 0.5 * std::fmod(k * 0.37, 1.0)), h * 0.32, h * 0.09, 0, 2 * M_PI);
    cairo_fill(cr);
    for (int r = 0; r < 3; ++r) {
        col(0.55 - 0.15 * r, 0.45, 0.33 + 0.05 * r);
        const double base = h * (0.55 + 0.15 * r);
        cairo_move_to(cr, 0, h);
        for (int x = 0; x <= w; x += 8) {
            const double y = base + std::sin(x * (0.004 + 0.002 * r) + k + r) * h * 0.06 +
                             std::sin(x * 0.019 + r * 3.1) * h * 0.015;
            cairo_line_to(cr, x, y);
        }
        cairo_line_to(cr, w, h);
        cairo_close_path(cr);
        cairo_fill(cr);
    }
    cairo_destroy(cr);
    const bool ok = cairo_surface_write_to_png(s, path.c_str()) == CAIRO_STATUS_SUCCESS;
    cairo_surface_destroy(s);
    return ok;
}

constexpr const char* kBody = "story.1";
constexpr int         kPlates = 8;

// One body page as the fitting loop sets it up.
struct BodyPage {
    Doc::Id page, text, art, pic;   // its text frame; the opener's art / a wrapped picture ("" none)
};

}  // namespace

BigBookInfo load_big_book(core::Doc& d, const BigBookOpts& o) {
    BigBookInfo info;
    d.reset();
    Doc::Loading load(d);
    const double in = kPt;

    core::Settings s;
    s.title = "The Long Book";
    d.put("doc", s);
    const double m = 0.75 * in, tw = s.trim_w - 2 * m, th = s.trim_h - 2 * m;
    const double sink = 1.75 * in;   // the chapter master's deeper top margin, less the body's

    // ── masters (as the sample's) ───────────────────────────────────────────
    d.put("master.1", core::Master{"A", "Front", 1, s.trim_w, s.trim_h, {}, 1, ""});
    {
        core::Master b{"B", "Body", 2, s.trim_w, s.trim_h, {}, 1, ""};
        b.grid = 14;
        d.put("master.2", b);
        core::Master c{"C", "Chapter", 3, s.trim_w, s.trim_h, {}, 1, "master.2"};
        c.margins.top = m + sink;
        c.grid = 14;
        d.put("master.3", c);
    }

    // ── styles ─────────────────────────────────────────────────────────────
    {
        core::Style b{"Body", core::StyleKind::Paragraph, 1, "Serif", 11, 14, false, false, core::Align::Justify};
        b.first_indent = 18;
        b.skip_after_heading = true;
        b.hyphenate = true;
        b.keep_lines = 2;
        d.put("style.1", b);
        core::Style first = b;
        first.name = "Body First";
        first.pos = 10;
        first.first_indent = 0;
        first.drop_lines = 3;
        first.based_on = "style.1";
        first.own = core::doc::style_differences(first, b);
        first.next = "style.1";
        d.put("style.10", first);
    }
    {
        core::Style h{"Heading 1", core::StyleKind::Paragraph, 2, "Serif", 24, 28, true, false, core::Align::Left, 12, 6, "swatch.3"};
        h.next = "style.10";
        h.keep_next = true;
        h.start = core::ParaStart::NextRightPage;
        h.rule_below = {1, "swatch.3", 1, 4, false};
        d.put("style.2", h);
        core::Style h2{"Heading 2", core::StyleKind::Paragraph, 2.5, "Serif", 13, 16, true, false, core::Align::Left, 14, 4};
        h2.next = "style.1";
        h2.keep_next = true;
        d.put("style.12", h2);
    }
    d.put("style.3", core::Style{"Caption", core::StyleKind::Paragraph, 3, "Serif", 9, 11, false, true, core::Align::Left});
    d.put("style.5", core::Style{"Emphasis", core::StyleKind::Character, 5, "", 0, 0, false, true, core::Align::Left});
    d.put("style.7", core::Style{"Folio", core::StyleKind::Paragraph, 7, "Serif", 9, 11, false, false, core::Align::Center});
    {
        core::Style c{"Contents Entry", core::StyleKind::Paragraph, 8, "Serif", 11, 18, false, false, core::Align::Left};
        c.tabs = {{tw, core::TabAlign::Right, "."}};
        d.put("style.8", c);
        d.put("style.9", core::Style{"Contents Title", core::StyleKind::Paragraph, 9, "Serif", 18, 22, false, false, core::Align::Left, 0, 12});
        d.put("style.13", core::Style{"Book Title", core::StyleKind::Paragraph, 9.5, "Serif", 30, 36, false, false, core::Align::Center, 0, 18});
    }

    // ── swatches ────────────────────────────────────────────────────────────
    d.put("swatch.1", core::Swatch{"Black", 1, core::ColorModel::CMYK, {0, 0, 0, 100}, "", 0x1a1a1a});
    d.put("swatch.2", core::Swatch{"Paper", 2, core::ColorModel::CMYK, {0, 1, 5, 0}, "", 0xfbf8f1});
    d.put("swatch.3", core::Swatch{"Guild Rust", 3, core::ColorModel::Spot, {0, 70, 90, 20}, "PMS 7593 C", 0xb5512b});

    // ── the plates (pictures) ──────────────────────────────────────────────
    std::vector<Doc::Id> plates;
    {
        std::error_code ec;
        if (!o.picture_dir.empty()) std::filesystem::create_directories(o.picture_dir, ec);
        for (int k = 1; k <= kPlates; ++k) {
            const std::string name = "plate-" + std::string(k < 10 ? "0" : "") + std::to_string(k) + ".png";
            const int pw = 1500, ph = (k % 2) ? 1000 : 1125;
            std::string path = "Pictures/" + name;   // missing (no folder given)
            std::int64_t stamp = 0;
            if (!o.picture_dir.empty()) {
                const std::string p = (std::filesystem::path(o.picture_dir) / name).string();
                if (std::filesystem::exists(p) || draw_plate(p, k, pw, ph)) {
                    path = p;
                    stamp = core::file_stamp(p);
                }
            }
            core::Asset a{name, static_cast<double>(k), path, false, "RGB", pw, ph};
            a.stamp = stamp;
            const Doc::Id id = "asset." + std::to_string(k);
            d.put(id, a);
            plates.push_back(id);
        }
    }

    // ── front matter: title, dedication, contents (iii, running on to iv) ───
    std::vector<Doc::Id> front;
    for (int i = 1; i <= 4; ++i) {
        core::Page p;
        p.pos = i;
        p.w = s.trim_w;
        p.h = s.trim_h;
        p.master = "master.1";
        const Doc::Id id = "page." + std::to_string(i);
        d.put(id, p);
        front.push_back(id);
    }
    d.put("story.2", core::Story{"Title", {{"style.13", s.title}, {"style.3", "A test of length, set in greeking"}}});
    {
        core::Frame t{front[0], core::FrameKind::Text, m, 2.5 * in, tw, 2 * in, 0, 1, "story.2", "", ""};
        d.put(d.fresh_id("frame"), t);
    }
    {
        core::Story ded{"Dedication", {{"style.1", "For every book that ran long."}}};
        ded.paras.front().over.align = core::Align::Center;
        d.put("story.3", ded);
        core::Frame f{front[1], core::FrameKind::Text, m, 3 * in, tw, 1 * in, 0, 1, "story.3", "", ""};
        f.valign = core::VAlign::Center;
        d.put(d.fresh_id("frame"), f);
    }
    d.put("story.4", core::Story{"Contents", {{"style.9", "Contents"}}});
    {
        const Doc::Id c2 = d.fresh_id("frame");
        const Doc::Id c1 = d.fresh_id("frame");
        d.put(c1, core::Frame{front[2], core::FrameKind::Text, m, m, tw, th, 0, 1, "story.4", c2, ""});
        d.put(c2, core::Frame{front[3], core::FrameKind::Text, m, m, tw, th, 0, 1, "story.4", "", ""});
    }

    // ── folios on the masters ──────────────────────────────────────────────
    d.put("story.5", core::Story{"Folio (front)", {{"style.7", core::kMarkPage}}});
    d.put("story.6", core::Story{"Folio (body, right)", {{"style.7", core::kMarkPage}}});
    d.put("story.7", core::Story{"Folio (body, left)", {{"style.7", core::kMarkPage}}});
    d.put(d.fresh_id("frame"), core::Frame{"master.1", core::FrameKind::Text, m, s.trim_h - 0.5 * in, tw, 0.25 * in, 0, 1, "story.5", "", ""});
    {
        core::Frame r{"master.2", core::FrameKind::Text, m + tw - 0.5 * in, s.trim_h - 0.5 * in, 0.5 * in, 0.25 * in, 0, 1, "story.6", "", ""};
        r.side = core::Side::Right;
        d.put(d.fresh_id("frame"), r);
        core::Frame l{"master.2", core::FrameKind::Text, m, s.trim_h - 0.5 * in, 0.5 * in, 0.25 * in, 0, 2, "story.7", "", ""};
        l.side = core::Side::Left;
        d.put(d.fresh_id("frame"), l);
    }

    // ── how much text a full page holds, measured (fonts differ by machine) ─
    Greek g(o.seed);
    double per_page = 2400;
    {
        core::Story cal{"calibration", {}};
        for (int i = 0; i < 14; ++i) {
            std::string t;
            for (int k = 0; k < 5; ++k) t += (k ? " " : "") + g.sentence();
            cal.paras.push_back({"style.1", t});
        }
        d.put("story.99", cal);
        d.put("page.99", core::Page{99, s.trim_w, s.trim_h, {}, "master.2"});
        d.put("frame.99", core::Frame{"page.99", core::FrameKind::Text, m, m, tw, th, 0, 1, "story.99", "", ""});
        const StoryFlow f = flow_story(d, "story.99");
        std::size_t bytes = 0;
        for (const auto& ft : f.frames)
            for (const auto& ln : ft.lines) bytes += static_cast<std::size_t>(ln.end - ln.start);
        if (bytes > 500) per_page = static_cast<double>(bytes);
        d.put("frame.99", std::nullopt);
        d.put("page.99", std::nullopt);
        d.put("story.99", std::nullopt);
    }

    // ── the long story: chapters of greeking, until it's about long enough ──
    core::Story body{"The Long Story", {}};
    const int target = std::max(8, o.pages - 4);
    double est = 0;
    while (est < target) {
        ++info.chapters;
        body.paras.push_back({"style.2", "Chapter " + std::to_string(info.chapters)});
        const int pages = std::min(g.range(9, 21), std::max(2, static_cast<int>(target - est)));
        const double want = (pages - 0.5) * per_page - sink / th * per_page;
        std::size_t have = 0;
        bool first = true;
        int since_head = 0;
        while (static_cast<double>(have) < want) {
            if (!first && since_head > 6 && g.next() % 5 == 0) {   // a section head
                body.paras.push_back({"style.12", g.title(g.range(2, 4))});
                ++info.sections;
                since_head = 0;
            }
            core::Para p{first ? "style.10" : "style.1", ""};
            const int n = g.range(2, 7);
            for (int k = 0; k < n; ++k) {
                int a = -1, b = -1;
                std::string sn = g.sentence(&a, &b);
                if (!p.text.empty()) p.text += ' ';
                if (a >= 0 && b > a)
                    p.runs.push_back({static_cast<int>(p.text.size()) + a, static_cast<int>(p.text.size()) + b, "style.5"});
                p.text += sn;
            }
            have += p.text.size();
            body.paras.push_back(std::move(p));
            first = false;
            ++since_head;
        }
        est += pages;
    }
    for (const auto& p : body.paras) info.bytes += p.text.size();
    d.put(kBody, body);

    // ── body pages, fitted to the text ─────────────────────────────────────
    // Chapter by chapter: a chapter always starts at the top of a fresh
    // right-hand page, so its lines break the same alone as in the whole
    // story. Each is fitted through its own pages as a scratch story (a few
    // cheap rounds: enough pages, its pictures, none empty at its end), then
    // handed to the long story and threaded on. One whole-story flow checks
    // the result; if it disagrees, whole-story rounds put it right.
    d.put("section.1", core::Section{"Front matter", front[0], core::Numbering::RomanLower, 1, core::Side::Right, "master.1"});
    constexpr const char* kScratch = "story.98";
    std::vector<BodyPage> bp;
    std::vector<char> is_open, is_pic;   // per body page
    int plate = 0;
    auto look = [&](std::size_t k) {   // set page k up as its flags say
        auto pg = *d.as<core::Page>(bp[k].page);
        auto tf = *d.as<core::Frame>(bp[k].text);
        const bool op = is_open[k];
        pg.master = op ? "master.3" : "master.2";
        tf.y = op ? m + sink : m;
        tf.h = op ? th - sink : th;
        if (!(pg == *d.as<core::Page>(bp[k].page))) d.put(bp[k].page, pg);
        if (!(tf == *d.as<core::Frame>(bp[k].text))) d.put(bp[k].text, tf);
        if (op && bp[k].art.empty()) {   // the opener's picture, in the sink
            bp[k].art = d.fresh_id("frame");
            core::Frame a{bp[k].page, core::FrameKind::Image, m, m, tw, sink - 0.25 * in, 0, 2, "", "",
                          plates[static_cast<std::size_t>(plate++ % kPlates)]};
            a.fit = core::ImageFit::Fill;
            d.put(bp[k].art, a);
        } else if (!op && !bp[k].art.empty()) {
            d.put(bp[k].art, std::nullopt);
            bp[k].art.clear();
        }
        if (is_pic[k] && bp[k].pic.empty()) {   // wrapped: around (in the column) or jumped (across it)
            bp[k].pic = d.fresh_id("frame");
            const bool around = (k / 9) % 2 == 0;
            core::Frame p{bp[k].page, core::FrameKind::Image, around ? m + tw / 2 - 0.9 * in : m,
                          around ? m + 2.2 * in : m + th - 2.6 * in, around ? 1.8 * in : tw, around ? 1.3 * in : 2.2 * in,
                          0, 3, "", "", plates[static_cast<std::size_t>((k / 9 + 3) % kPlates)]};
            p.fit = core::ImageFit::Fill;
            p.wrap = around ? core::Wrap::Around : core::Wrap::Jump;
            p.wrap_off = {9, 9, 9, 9};
            d.put(bp[k].pic, p);
        } else if (!is_pic[k] && !bp[k].pic.empty()) {
            d.put(bp[k].pic, std::nullopt);
            bp[k].pic.clear();
        }
    };
    auto link = [&](std::size_t from, std::size_t to) {   // body page from's frame -> to's ("" past the end)
        auto f = *d.as<core::Frame>(bp[from].text);
        f.next = to < bp.size() ? bp[to].text : "";
        d.put(bp[from].text, f);
    };
    auto add_page = [&](const char* story) {
        BodyPage b;
        b.page = d.fresh_id("page");
        core::Page p;
        p.pos = 5 + static_cast<double>(bp.size());
        p.w = s.trim_w;
        p.h = s.trim_h;
        p.master = "master.2";
        d.put(b.page, p);
        b.text = d.fresh_id("frame");
        d.put(b.text, core::Frame{b.page, core::FrameKind::Text, m, m, tw, th, 0, 1, story, "", ""});
        bp.push_back(b);
        is_open.push_back(0);
        is_pic.push_back(0);
        return bp.size() - 1;
    };
    auto drop_from = [&](std::size_t k) {   // body pages k.. off the end
        for (std::size_t i = k; i < bp.size(); ++i) {
            for (const auto& id : {bp[i].text, bp[i].art, bp[i].pic})
                if (!id.empty()) d.put(id, std::nullopt);
            d.put(bp[i].page, std::nullopt);
        }
        bp.resize(k);
        is_open.resize(k);
        is_pic.resize(k);
    };
    // Which body pages a flow fills, and which it opens with a chapter heading.
    struct Seen { std::vector<char> filled, opens; int last = -1; std::size_t placed = 0; };
    auto seen = [&](const StoryFlow& f, const core::Story& st, std::size_t from) {
        Seen sn;
        std::map<std::string, const FrameText*> by_frame;
        for (const auto& ft : f.frames) by_frame[ft.frame] = &ft;
        for (std::size_t k = from; k < bp.size(); ++k) {
            const auto it = by_frame.find(bp[k].text);
            const bool any = it != by_frame.end() && !it->second->lines.empty();
            bool op = false;
            if (any) {
                sn.last = static_cast<int>(k);
                const Line& l0 = it->second->lines.front();
                op = l0.start == 0 && st.paras[static_cast<std::size_t>(l0.para)].style == "style.2";
                for (const auto& ln : it->second->lines) sn.placed += static_cast<std::size_t>(ln.end - ln.start);
            }
            sn.filled.push_back(any);
            sn.opens.push_back(op);
        }
        return sn;
    };
    // Every ninth body page that is full and not its chapter's last.
    auto wants_pic = [](std::size_t k, bool open, bool filled, bool chapter_end) {
        return !open && filled && !chapter_end && k % 9 == 4;
    };

    // The chapters' paragraph ranges.
    std::vector<std::pair<std::size_t, std::size_t>> chapters;
    for (std::size_t i = 0; i < body.paras.size(); ++i)
        if (body.paras[i].style == "style.2") chapters.push_back({i, i});
    for (std::size_t c = 0; c < chapters.size(); ++c)
        chapters[c].second = c + 1 < chapters.size() ? chapters[c + 1].first : body.paras.size();

    for (const auto& [p0, p1] : chapters) {
        if (bp.size() % 2 == 1) {   // a left-hand page: blank before the chapter (the flow passes it)
            const auto k = add_page(kBody);
            if (k > 0) link(k - 1, k);
        }
        const std::size_t first = bp.size();
        core::Story ch{"chapter", {body.paras.begin() + static_cast<long>(p0), body.paras.begin() + static_cast<long>(p1)}};
        std::size_t bytes = 0;
        for (const auto& p : ch.paras) bytes += p.text.size();
        d.put(kScratch, ch);
        auto more = [&](int n) {
            for (int i = 0; i < n; ++i) {
                const auto k = add_page(kScratch);
                if (k > first) link(k - 1, k);
            }
        };
        more(static_cast<int>(std::ceil(static_cast<double>(bytes) / per_page + sink / th)) + 1);
        is_open[first] = 1;
        look(first);
        for (int round = 1; round <= 10; ++round) {
            ++info.rounds;
            const StoryFlow f = flow_story(d, kScratch);
            const Seen sn = seen(f, ch, first);
            if (f.overset) {
                const double rest = static_cast<double>(bytes > sn.placed ? bytes - sn.placed : 0);
                more(std::max(1, static_cast<int>(std::ceil(rest / per_page * 1.1)) + 1));
                continue;
            }
            const std::size_t end = static_cast<std::size_t>(std::max(sn.last, static_cast<int>(first))) + 1;
            if (end < bp.size()) {
                drop_from(end);
                link(end - 1, end);
            }
            bool same = true;
            for (std::size_t k = first; k < bp.size(); ++k) {
                const bool want = wants_pic(k, k == first, sn.filled[k - first], k + 1 >= bp.size());
                if (want != static_cast<bool>(is_pic[k])) {
                    is_pic[k] = want;
                    same = false;
                    look(k);
                }
            }
            if (same) break;
        }
        for (std::size_t k = first; k < bp.size(); ++k) {   // hand the pages to the long story
            auto f = *d.as<core::Frame>(bp[k].text);
            f.story = kBody;
            d.put(bp[k].text, f);
        }
        if (first > 0) link(first - 1, first);
    }
    d.put(kScratch, std::nullopt);
    d.put("section.2", core::Section{"Body", bp.front().page, core::Numbering::Arabic, 1, core::Side::Right, "master.2"});

    // The check: the whole story, flowed once. Whole-story rounds only if it
    // came out otherwise than the chapters did.
    StoryFlow final;
    for (int round = 1; round <= 12; ++round) {
        ++info.whole;
        final = flow_story(d, kBody);
        const StoryFlow& f = final;
        const auto& st = *d.as<core::Story>(kBody);
        const Seen sn = seen(f, st, 0);
        if (f.overset) {
            const double rest = static_cast<double>(info.bytes > sn.placed ? info.bytes - sn.placed : 0);
            const std::size_t k0 = bp.size();
            for (int i = 0, n = std::max(2, static_cast<int>(std::ceil(rest / per_page * 1.1)) + 2); i < n; ++i) add_page(kBody);
            for (std::size_t k = k0; k < bp.size(); ++k) link(k - 1, k);
            continue;
        }
        if (sn.last + 1 < static_cast<int>(bp.size())) {   // empty pages off the end
            drop_from(static_cast<std::size_t>(sn.last + 1));
            link(bp.size() - 1, bp.size());
        }
        bool same = true;
        for (std::size_t k = 0; k < bp.size(); ++k) {
            const bool chapter_end = k + 1 >= bp.size() || !sn.filled[k + 1] || sn.opens[k + 1];
            const char op = sn.opens[k], pic = wants_pic(k, op, sn.filled[k], chapter_end);
            if (op != is_open[k] || pic != is_pic[k]) {
                is_open[k] = op;
                is_pic[k] = pic;
                same = false;
                look(k);
            }
        }
        if (same) break;
    }
    for (std::size_t k = 0; k < bp.size(); ++k) info.pictures += is_open[k] + is_pic[k];
    info.pages = static_cast<int>(front.size() + bp.size());

    // ── the contents list, built where the headings landed ─────────────────
    d.put("list.1", core::GenList{"Table of Contents", core::ListKind::Contents, 1, {"style.2"}, front[2], -1, "story.4"});
    {   // from the final flow (what list_entries would find, without
        // looking each heading up again): a heading's first line, its page
        const auto& paras = d.as<core::Story>(kBody)->paras;
        auto st = *d.as<core::Story>("story.4");
        st.paras.resize(1);
        int n = 0;
        for (const auto& ft : final.frames)
            for (const auto& ln : ft.lines)
                if (ln.start == 0 && paras[static_cast<std::size_t>(ln.para)].style == "style.2") {
                    st.paras.push_back({"style.8", paras[static_cast<std::size_t>(ln.para)].text + "\t" +
                                                       d.folio(d.as<core::Frame>(ft.frame)->owner)});
                    ++n;
                }
        d.put("story.4", st);
        auto l = *d.as<core::GenList>("list.1");
        l.built_entries = n;
        d.put("list.1", l);
    }
    // ── margins as guides on the body master (as the sample) ───────────────
    d.put("guide.1", core::Guide{"master.2", true, m});
    d.put("guide.2", core::Guide{"master.2", true, s.trim_w - m});
    d.put("guide.3", core::Guide{"master.2", false, m});
    d.put("guide.4", core::Guide{"master.2", false, s.trim_h - m});
    return info;
}

}  // namespace composix::layout
