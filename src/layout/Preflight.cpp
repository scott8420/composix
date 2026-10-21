#include "layout/Preflight.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "layout/ColorReport.hpp"
#include "layout/Export.hpp"
#include <plate/plate.hpp>
#include <cmath>
#include <cstdio>
#include <memory>

namespace composix::layout {

namespace {
// The first page (in order) a frame is on, through its master when it sits
// on one: a master's item is "on" the first page that shows the master.
std::string page_of(const core::Doc& d, const core::Frame& f) {
    if (d.as<core::Page>(f.owner)) return f.owner;
    for (const auto& pid : d.pages()) {
        const auto chain = core::master_chain(d, pid);
        if (std::find(chain.begin(), chain.end(), f.owner) != chain.end()) return pid;
    }
    return {};
}

// s053: one proof kept, rebuilt only when the profile changes (Preflight
// runs on every edit; reading a press profile is not free).
const plate::Proof* proof_for(const std::string& path, bool perceptual) {
    static std::unique_ptr<plate::Proof> p;
    static std::string at;
    static bool pc = false;
    if (!p || at != path || pc != perceptual) {   // s085: per intent
        p = std::make_unique<plate::Proof>();
        at = path;
        pc = perceptual;
        if (!p->load(path, perceptual)) p.reset();
    }
    return p.get();
}

// s312: on a page, and on a layer that prints (a hidden / non-printing layer's frames aren't checked)
bool shown(const core::Doc& d, const core::Frame& f) { return !page_of(d, f).empty() && core::doc::frame_prints(d, f); }

std::string where(const core::Doc& d, const std::string& page) {
    return page.empty() ? std::string{} : " — page " + d.folio(page);
}
}  // namespace

std::vector<Issue> preflight_issues(const core::Doc& d, FlowCache& flows, const PreflightOpts& o) {
    using L = Issue::Level;
    std::vector<Issue> out;
    // Overset: at the last frame of the thread.
    for (const auto& sid : d.ids_of<core::Story>()) {
        const auto& fl = flows.get(d, sid);
        if (!fl.overset || fl.frames.empty()) continue;
        const auto& last = fl.frames.back().frame;
        const auto* f = d.as<core::Frame>(last);
        const std::string pg = f ? page_of(d, *f) : std::string{};
        out.push_back({L::Error, "overset", "Overset text: " + d.as<core::Story>(sid)->name + where(d, pg), pg, last});
    }
    // s129: cross-references that point nowhere (their target deleted) or at
    // a place no page shows (overset, a story not placed).
    {
        std::map<std::string, StoryFlow> ref_flows;
        for (const auto& sid : d.ids_of<core::Story>()) {
            const auto& st = *d.as<core::Story>(sid);
            bool any = false;
            for (const auto& para : st.paras) any = any || !para.refs.empty();
            if (!any) continue;
            const auto& fl = flows.get(d, sid);
            for (int p = 0; p < static_cast<int>(st.paras.size()); ++p) {
                const auto& para = st.paras[static_cast<std::size_t>(p)];
                int k = 0;
                for (std::size_t i = para.text.find(core::kMarkRef); i != std::string::npos; i = para.text.find(core::kMarkRef, i + 3), ++k) {
                    if (k >= static_cast<int>(para.refs.size())) break;
                    const auto& r = para.refs[static_cast<std::size_t>(k)];
                    // Where the reference itself is.
                    std::string pg, fr;
                    for (const auto& ft : fl.frames)
                        for (const auto& l : ft.lines)
                            if (l.para == p && static_cast<int>(i) >= l.start && static_cast<int>(i) < std::max(l.end, l.start + 1)) {
                                fr = ft.frame;
                                if (const auto* f = d.as<core::Frame>(ft.frame)) pg = page_of(d, *f);
                            }
                    const auto at = core::doc::find_anchor(d, r.target);
                    // s143: a citation's other sources -- one gone breaks it as well.
                    const bool more_gone = std::any_of(r.more.begin(), r.more.end(), [&](const std::string& id) { return core::doc::find_anchor(d, id).story.empty(); });
                    if (at.story.empty() || more_gone) {
                        out.push_back({L::Warning, "xref", "A cross-reference points nowhere (its target is gone) in " + st.name + where(d, pg), pg, fr});
                        continue;
                    }
                    core::XRef page_only{r.target, core::RefShow::Page, {}};
                    if (ref_value(d, page_only, ref_flows) == "??")
                        out.push_back({L::Warning, "xref", "A cross-reference's target isn't on a page (overset or not placed) in " + st.name + where(d, pg),
                                       pg, fr});
                }
            }
        }
    }
    // Pictures: links, resolution; empty frames; transparency.
    std::set<std::string> seen_link;
    for (const auto& fid : d.ids_of<core::Frame>()) {
        const auto& f = *d.as<core::Frame>(fid);
        if (!shown(d, f)) continue;
        const std::string pg = page_of(d, f);
        if (f.kind == core::FrameKind::Image) {
            const auto* a = d.as<core::Asset>(f.asset);
            if (!a) {
                out.push_back({L::Info, "empty", "Empty picture frame" + where(d, pg), pg, fid});
                continue;
            }
            if (o.link && seen_link.insert(f.asset).second) {
                const auto st = o.link(*a);
                if (st == core::LinkStatus::Missing || st == core::LinkStatus::EmbeddedMissing)
                    out.push_back({L::Error, "missing", "Missing picture: " + a->name + where(d, pg), pg, fid});
                else if (st == core::LinkStatus::Modified)
                    out.push_back({L::Warning, "modified", "Picture changed on disk: " + a->name + where(d, pg), pg, fid});
            }
            const double ppi = core::effective_ppi(f, *a);
            if (ppi > 0 && ppi < core::kLowPpi)
                out.push_back({L::Warning, "lowres", "Low resolution: " + a->name + " (" +
                                                         std::to_string(static_cast<int>(std::lround(ppi))) + " ppi)" + where(d, pg),
                               pg, fid});
            if (o.standard == 1 && (a->color == "RGB" || a->color == "Lab") && !o.profile)   // s184: Lab too
                out.push_back({L::Error, "rgb", a->color + " picture (PDF/X-1a needs a profile to convert it): " + a->name + where(d, pg), pg, fid});
        } else if (f.kind == core::FrameKind::Text) {
            const auto* st = d.as<core::Story>(f.story);
            bool empty = !st;
            if (st) {
                empty = true;
                for (const auto& p : st->paras) empty = empty && p.text.empty();
            }
            if (empty && d.as<core::Page>(f.owner))
                out.push_back({L::Info, "empty", "Empty text frame" + where(d, pg), pg, fid});
        }
        if (o.standard == 1 && f.opacity < 1)   // s193: flattened on export now -- a warning, not a stop
            out.push_back({L::Warning, "transparency", "Transparency (" + core::tint_text(f.opacity) +
                                                           " opacity) \u2014 flattened for PDF/X-1a (300 ppi, process colour there)" +
                                                           where(d, pg), pg, fid});
    }
    // Colours: spot plates (info); RGB swatches in use under X-1a.
    std::map<std::string, std::string> used;   // swatch -> a frame using it
    for (const auto& fid : d.ids_of<core::Frame>()) {
        const auto& f = *d.as<core::Frame>(fid);
        if (!shown(d, f)) continue;
        for (const auto& sw : {f.fill, f.stroke}) if (!sw.empty()) used.emplace(sw, fid);
    }
    for (const auto& sid : d.ids_of<core::Style>())
        if (const auto& c = d.as<core::Style>(sid)->color; !c.empty()) used.emplace(c, "");
    for (const auto& [sw, fid] : used) {
        const auto* s = d.as<core::Swatch>(sw);
        if (!s) continue;
        const auto* f = d.as<core::Frame>(fid);
        const std::string pg = f ? page_of(d, *f) : std::string{};
        if (s->model == core::ColorModel::Spot)
            out.push_back({L::Info, "spot", "Spot colour " + (s->spot.empty() ? s->name : s->spot) + " \u2014 its own plate", pg, fid});
        if (s->model == core::ColorModel::RGB && o.convert && !o.profile_path.empty()) {   // s053
            if (const auto* pf = proof_for(o.profile_path, o.perceptual); pf) {
                const auto c = [](double v) { return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0, 255.0))); };
                const auto r = pf->check(c(s->v[0]), c(s->v[1]), c(s->v[2]));
                if (shift_of(r.delta_e) >= Shift::Noticeable) {
                    char de[16];
                    std::snprintf(de, sizeof de, "%.1f", r.delta_e);
                    out.push_back({L::Warning, "shift", "RGB colour " + s->name + " \u2014 " + shift_text(shift_of(r.delta_e)) +
                                                           " in CMYK (\u0394E " + de + "); see Colour Report", pg, fid});
                }
            }
        }
        if (s->model == core::ColorModel::RGB && o.standard == 1 && !o.profile)
            out.push_back({L::Error, "rgb", "RGB colour " + s->name + " \u2014 PDF/X-1a needs a profile to convert it", pg, fid});
    }
    // s089: a contents list that no longer says what the headings do.
    for (const auto& s : stale_lists(d, flows)) {
        const auto& l = *d.as<core::GenList>(s.list);
        std::string fr;
        for (const auto& fid : d.frames_on(l.placed_on))
            if (const auto* f = d.as<core::Frame>(fid); f->story == l.story && !l.story.empty()) { fr = fid; break; }
        Issue i{L::Warning, "stale_list", "Contents out of date: " + l.name + " \u2014 " + s.why, l.placed_on, fr};
        i.item = s.list;
        out.push_back(i);
    }
    if (!o.missing_profile.empty())
        out.push_back({L::Error, "profile", "Output profile \u201c" + o.missing_profile + "\u201d is not on this computer (Export PDF)", {}, {}});
    if (o.standard != 0 && !o.profile && o.missing_profile.empty())
        out.push_back({L::Error, "profile", "PDF/X needs an output profile (Export PDF)", {}, {}});
    // s312: hyperlinks that go nowhere (their anchor or page gone) -- at the link's words
    for (const auto& l : core::doc::hyperlinks(d)) {
        if (!l.broken) continue;
        std::string pg, fr;
        for (const auto& ft : flows.get(d, l.story).frames)
            for (const auto& ln : ft.lines)
                if (pg.empty() && ln.para == l.para && l.from >= ln.start && l.from < std::max(ln.end, ln.start + 1)) {
                    fr = ft.frame;
                    if (const auto* f = d.as<core::Frame>(ft.frame)) pg = page_of(d, *f);
                }
        std::string why = core::doc::link_where(d, l.target);
        if (why.rfind("Gone ", 0) == 0) why = why.substr(5);   // "(its anchor deleted)"
        else why = "(" + why + ")";
        out.push_back({L::Warning, "link", "A hyperlink goes nowhere " + why + ": \u201c" + l.words + "\u201d" + where(d, pg), pg, fr});
    }
    // s312: layers left out of the PDF
    for (const auto& lid : core::doc::layers(d)) {
        const auto& ly = *d.as<core::Layer>(lid);
        if (ly.visible && ly.print) continue;
        const int n = core::doc::frames_on_layer(d, lid);
        if (n == 0) continue;
        out.push_back({L::Info, "layer", "Layer \u201c" + ly.name + "\u201d " + (ly.visible ? "doesn't print" : "is hidden") + " -- its " +
                                             std::to_string(n) + (n == 1 ? " frame is" : " frames are") + " left out of the PDF", {}, {}, lid});
    }
    // s312: misspelled words (with a dictionary) -- worth knowing before it goes
    if (const auto c = core::doc::misspellings(d); c.words > 0)
        out.push_back({L::Info, "spelling", std::to_string(c.words) + (c.words == 1 ? " misspelled word" : " misspelled words") + " in " +
                                                std::to_string(c.stories) + (c.stories == 1 ? " story" : " stories") + " (Edit \u203a Check Spelling)",
                       {}, {}, {}});
    // Errors first, then warnings, then info; each in page order.
    std::map<std::string, int> at;
    const auto pages = d.pages();
    for (std::size_t i = 0; i < pages.size(); ++i) at[pages[i]] = static_cast<int>(i);
    std::stable_sort(out.begin(), out.end(), [&](const Issue& a, const Issue& b) {
        if (a.level != b.level) return a.level < b.level;
        const int pa = at.count(a.page) ? at[a.page] : 1 << 20, pb = at.count(b.page) ? at[b.page] : 1 << 20;
        return pa < pb;
    });
    return out;
}

int problem_count(const std::vector<Issue>& v) {
    return static_cast<int>(std::count_if(v.begin(), v.end(), [](const Issue& i) { return i.level != Issue::Level::Info; }));
}

}  // namespace composix::layout
