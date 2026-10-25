// s351: a master's body frame and the page frames that follow it (InDesign's
// primary text frame). See Doc.hpp.
#include "core/Doc.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace composix::core::doc {

namespace {

// What a follower takes from the body frame.
void take_shape(const Frame& b, Frame& f) {
    f.x = b.x;
    f.y = b.y;
    f.w = b.w;
    f.h = b.h;
    f.columns = b.columns;
    f.gutter = b.gutter;
    f.widths = b.widths;
    f.balance = b.balance;
    f.inset = b.inset;
    f.valign = b.valign;
    f.col_rule = b.col_rule;
}

bool same_place(const Frame& a, const Frame& b) {
    return std::abs(a.x - b.x) < 0.01 && std::abs(a.y - b.y) < 0.01 && std::abs(a.w - b.w) < 0.01 && std::abs(a.h - b.h) < 0.01 &&
           a.columns == b.columns && std::abs(a.gutter - b.gutter) < 0.01;
}

}  // namespace

Doc::Id body_frame_for(const Doc& d, const Doc::Id& page) {
    const auto* p = d.as<Page>(page);
    if (!p || p->master.empty()) return {};
    const Side side = d.side_of(page);
    Doc::Id any;
    for (const auto& fid : d.frames_on(p->master)) {
        const auto* f = d.as<Frame>(fid);
        if (!f || !f->body || f->kind != FrameKind::Text) continue;
        if (f->side == side && side != Side::Any) return fid;
        if (f->side == Side::Any && any.empty()) any = fid;
    }
    return any;
}

std::vector<DocJournal::Settled> follow_body_frames(const Doc& d) {
    std::vector<DocJournal::Settled> out;
    // s352: body frames that keep to their margins -- set there first (what
    // the followers below take: the moved one)
    std::map<Doc::Id, Frame> moved;
    for (const auto& m : d.ids_of<Master>())
        for (const auto& fid : d.frames_on(m)) {
            const auto* b = d.as<Frame>(fid);
            if (!b || !b->body || !b->fit_margins) continue;
            const Rect r = margin_rect(d, m, b->side == Side::Any ? (d.as<Master>(m)->facing ? Side::Right : Side::Any) : b->side);
            if (r.w <= 0 || r.h <= 0) continue;
            const auto at = [](const Frame& f, double x, double y, double w, double h) {
                return std::abs(f.x - x) < 0.01 && std::abs(f.y - y) < 0.01 && std::abs(f.w - w) < 0.01 && std::abs(f.h - h) < 0.01;
            };
            Frame g = *b;
            if (!at(*b, b->fit_x, b->fit_y, b->fit_w, b->fit_h)) {
                g.fit_margins = false;   // moved by hand: its own place from now on
            } else {
                if (at(*b, r.x, r.y, r.w, r.h)) continue;   // where the margins are
                g.x = g.fit_x = r.x; g.y = g.fit_y = r.y; g.w = g.fit_w = r.w; g.h = g.fit_h = r.h;
            }
            moved[fid] = g;
            out.push_back({fid, Record{g}});
        }
    for (const auto& fid : d.ids_of<Frame>()) {
        const auto* f = d.as<Frame>(fid);
        if (!f->follow_body || f->kind != FrameKind::Text || !d.as<Page>(f->owner)) continue;
        const auto bid = body_frame_for(d, f->owner);
        const auto* b = d.as<Frame>(bid);
        if (!b) continue;   // no body frame there (yet): it stays as it is
        if (const auto mv = moved.find(bid); mv != moved.end()) b = &mv->second;
        Frame g = *f;
        take_shape(*b, g);
        if (!(g == *f)) out.push_back({fid, Record{g}});
    }
    return out;
}

bool set_body_frame(DocJournal& j, Doc& d, const Doc::Id& frame, bool on) {
    const auto* f = d.as<Frame>(frame);
    if (!f || f->kind != FrameKind::Text || !d.as<Master>(f->owner) || f->body == on) return false;
    std::vector<Doc::Id> ids{frame};
    if (on)
        for (const auto& o : d.frames_on(f->owner))
            if (const auto* g = d.as<Frame>(o); o != frame && g && g->body && g->side == f->side) ids.push_back(o);
    return j.run(on ? "Body Frame" : "Not the Body Frame", ids, [&] {
        for (const auto& id : ids) {
            Frame g = *d.as<Frame>(id);
            g.body = id == frame ? on : false;
            d.put(id, g);
        }
    });
}

int body_frames_into(Doc& d, const Doc::Id& story, std::vector<Doc::Id>& made) {   // s352
    if (!d.as<Story>(story)) return 0;
    // the story's page frames, by (master, side)
    std::map<std::pair<Doc::Id, Side>, std::vector<Doc::Id>> groups;
    for (const auto& fid : d.ids_of<Frame>()) {
        const auto* f = d.as<Frame>(fid);
        if (f->kind != FrameKind::Text || f->story != story) continue;
        const auto* p = d.as<Page>(f->owner);
        if (!p || p->master.empty() || !d.as<Master>(p->master)) continue;
        groups[{p->master, d.side_of(f->owner)}].push_back(fid);
    }
    int n = 0;
    for (const auto& [key, frames] : groups) {
        const auto& [master, side] = key;
        // the place most of them share
        std::vector<std::pair<int, Doc::Id>> votes;
        for (const auto& fid : frames) {
            int same = 0;
            for (const auto& other : frames) same += same_place(*d.as<Frame>(fid), *d.as<Frame>(other));
            votes.emplace_back(same, fid);
        }
        const auto best = std::max_element(votes.begin(), votes.end(), [](const auto& a, const auto& b) { return a.first < b.first; })->second;
        const Frame model = *d.as<Frame>(best);
        // the master's body frame for that side: made, or set to it
        Doc::Id bid;
        for (const auto& o : d.frames_on(master))
            if (const auto* g = d.as<Frame>(o); g && g->body && g->kind == FrameKind::Text && g->side == side) bid = o;
        Frame b;
        if (!bid.empty()) b = *d.as<Frame>(bid);
        else {
            bid = d.fresh_id("frame");
            made.push_back(bid);
            b.owner = master;
            b.kind = FrameKind::Text;
            b.side = side;
            b.body = true;
        }
        take_shape(model, b);
        const Rect m = margin_rect(d, model.owner);   // where its page's margins are
        b.fit_margins = std::abs(model.x - m.x) < 0.5 && std::abs(model.y - m.y) < 0.5 && std::abs(model.w - m.w) < 0.5 && std::abs(model.h - m.h) < 0.5;
        if (b.fit_margins) { b.fit_x = b.x; b.fit_y = b.y; b.fit_w = b.w; b.fit_h = b.h; }
        d.put(bid, b);
        for (const auto& fid : frames) {
            Frame f = *d.as<Frame>(fid);
            if (!same_place(f, model)) continue;   // placed its own way: left so
            f.follow_body = true;
            d.put(fid, f);
            ++n;
        }
    }
    return n;
}

int make_body_frames(DocJournal& j, Doc& d, const Doc::Id& story) {
    if (!d.as<Story>(story)) return 0;
    std::vector<Doc::Id> touched;
    for (const auto& fid : d.ids_of<Frame>())
        if (const auto* f = d.as<Frame>(fid); f->kind == FrameKind::Text && f->story == story) touched.push_back(fid);
    for (const auto& m : d.ids_of<Master>())
        for (const auto& o : d.frames_on(m))
            if (const auto* g = d.as<Frame>(o); g && g->body) touched.push_back(o);
    if (touched.empty()) return 0;
    int n = 0;
    j.run("Body Frames", touched, [&] {
        std::vector<Doc::Id> made;
        n = body_frames_into(d, story, made);
        return made;
    });
    return n;
}

Doc::Id body_thread_from(const Doc& d, const Doc::Id& page) {   // s354
    for (const auto& fid : d.frames_on(page)) {
        const auto* f = d.as<Frame>(fid);
        if (f && f->kind == FrameKind::Text && f->follow_body && !f->story.empty() && f->owner == page) return fid;
    }
    return {};
}

std::vector<Doc::Id> thread_new_pages(Doc& d, const std::vector<Doc::Id>& pages) {   // s354
    std::vector<Doc::Id> made;
    const auto order = d.pages();
    for (std::size_t k = 1; k < order.size(); ++k) {
        const auto& pid = order[k];
        if (std::find(pages.begin(), pages.end(), pid) == pages.end()) continue;
        const auto bid = body_frame_for(d, pid);
        if (bid.empty()) continue;
        const Doc::Id from = body_thread_from(d, order[k - 1]);   // the page before's
        if (from.empty()) continue;
        Frame prev = *d.as<Frame>(from);
        Frame nf = prev;
        nf.owner = pid;
        nf.overrides.clear();
        nf.body = false;
        nf.fit_margins = false;
        nf.follow_body = true;
        take_shape(*d.as<Frame>(bid), nf);
        nf.next = prev.next;
        const auto fid = d.fresh_id("frame");
        d.put(fid, nf);
        prev.next = fid;
        d.put(from, prev);
        made.push_back(fid);
    }
    return made;
}

}  // namespace composix::core::doc
