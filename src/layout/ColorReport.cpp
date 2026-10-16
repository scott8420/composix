// layout::color_report (s053) -- see ColorReport.hpp.
#include "layout/ColorReport.hpp"

#include <plate/plate.hpp>

#include <algorithm>
#include <cmath>

namespace composix::layout {

Shift shift_of(double de) {
    return de < 2 ? Shift::Holds : de < 5 ? Shift::Slight : de < 10 ? Shift::Noticeable : Shift::Large;
}

const char* shift_text(Shift s) {
    switch (s) {
    case Shift::Holds:      return "Holds";
    case Shift::Slight:     return "Slight shift";
    case Shift::Noticeable: return "Noticeable shift";
    case Shift::Large:      return "Large shift — out of gamut";
    }
    return "";
}

namespace {
// As Preflight: a frame's first page, through its master.
std::string page_of(const core::Doc& d, const core::Frame& f) {
    if (d.as<core::Page>(f.owner)) return f.owner;
    for (const auto& pid : d.pages()) {
        const auto chain = core::master_chain(d, pid);
        if (std::find(chain.begin(), chain.end(), f.owner) != chain.end()) return pid;
    }
    return {};
}
constexpr int kSample = 160;   // a picture is judged on at most this many pixels a side
}  // namespace

ColorReportResult color_report(const core::Doc& d, const std::string& icc, bool perceptual, const PixelSource& pixels) {
    ColorReportResult out;
    plate::Proof proof;
    if (!proof.load(icc, perceptual, &out.err)) return out;
    out.ok = true;
    out.profile_name = proof.name();

    // RGB swatches, by name.
    std::vector<ColorRow> sw;
    for (const auto& id : d.ids_of<core::Swatch>()) {
        const auto& s = *d.as<core::Swatch>(id);
        if (s.model != core::ColorModel::RGB) continue;
        ColorRow r;
        r.kind = ColorRow::Kind::Swatch;
        r.id = id;
        r.name = s.name;
        const int uses = core::doc::swatch_uses(d, id);
        r.used = uses > 0;
        r.where = uses == 0 ? "unused" : uses == 1 ? "1 place" : std::to_string(uses) + " places";
        const auto c = [](double v) { return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0, 255.0))); };
        const auto R = c(s.v[0]), G = c(s.v[1]), B = c(s.v[2]);
        r.before = (std::uint32_t{R} << 16) | (std::uint32_t{G} << 8) | B;
        const auto p = proof.check(R, G, B);
        r.after = (std::uint32_t{p.after[0]} << 16) | (std::uint32_t{p.after[1]} << 8) | p.after[2];
        std::copy(std::begin(p.cmyk), std::end(p.cmyk), r.cmyk);
        r.delta_e = r.worst = p.delta_e;
        r.share = p.delta_e >= 5 ? 1 : 0;
        r.shift = shift_of(p.delta_e);
        sw.push_back(std::move(r));
    }
    std::stable_sort(sw.begin(), sw.end(), [](const ColorRow& a, const ColorRow& b) { return a.name < b.name; });

    // Placed RGB pictures, by the first page they're on.
    const auto pages = d.pages();
    const auto order = [&](const std::string& pid) {
        const auto it = std::find(pages.begin(), pages.end(), pid);
        return it == pages.end() ? pages.size() : static_cast<std::size_t>(it - pages.begin());
    };
    std::vector<ColorRow> im;
    for (const auto& aid : d.ids_of<core::Asset>()) {
        const auto& a = *d.as<core::Asset>(aid);
        if (a.color != "RGB") continue;
        std::string page;
        for (const auto& fid : d.ids_of<core::Frame>()) {
            const auto& f = *d.as<core::Frame>(fid);
            if (f.kind != core::FrameKind::Image || f.asset != aid) continue;
            const auto p = page_of(d, f);
            if (!p.empty() && (page.empty() || order(p) < order(page))) page = p;
        }
        if (page.empty()) continue;   // not placed: it won't print
        ColorRow r;
        r.kind = ColorRow::Kind::Image;
        r.id = aid;
        r.name = a.name;
        r.page = page;
        r.where = "page " + d.folio(page);
        int w = 0, h = 0;
        std::vector<std::uint8_t> rgb;
        if (!pixels || !pixels(a, w, h, rgb) || w <= 0 || h <= 0 ||
            rgb.size() < static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3) {
            r.readable = false;
            im.push_back(std::move(r));
            continue;
        }
        // Down to a sample (nearest pixel), so a large photo costs little.
        const int step = std::max(1, (std::max(w, h) + kSample - 1) / kSample);
        r.w = (w + step - 1) / step;
        r.h = (h + step - 1) / step;
        r.px_before.resize(static_cast<std::size_t>(r.w) * r.h * 3);
        for (int y = 0; y < r.h; ++y)
            for (int x = 0; x < r.w; ++x) {
                const auto* s = &rgb[(static_cast<std::size_t>(y * step) * w + x * step) * 3];
                std::copy(s, s + 3, &r.px_before[(static_cast<std::size_t>(y) * r.w + x) * 3]);
            }
        r.px_after = r.px_before;
        std::vector<float> de;
        proof.proof_pixels(r.px_after.data(), static_cast<std::size_t>(r.w) * r.h, &de);
        double sum = 0;
        int over = 0;
        for (const float e : de) {
            sum += e;
            r.worst = std::max(r.worst, static_cast<double>(e));
            over += e >= 5;
        }
        r.delta_e = de.empty() ? 0 : sum / static_cast<double>(de.size());
        r.share = de.empty() ? 0 : static_cast<double>(over) / static_cast<double>(de.size());
        // A picture is judged by its average and by how much of it moves.
        r.shift = r.share >= 0.25 ? std::max(shift_of(r.delta_e), Shift::Noticeable) : shift_of(r.delta_e);
        im.push_back(std::move(r));
    }
    std::stable_sort(im.begin(), im.end(), [&](const ColorRow& a, const ColorRow& b) { return order(a.page) < order(b.page); });

    out.rows = std::move(sw);
    out.rows.insert(out.rows.end(), std::make_move_iterator(im.begin()), std::make_move_iterator(im.end()));
    for (const auto& r : out.rows) out.worst_count += r.used && r.readable && r.shift >= Shift::Noticeable;
    return out;
}

}  // namespace composix::layout
