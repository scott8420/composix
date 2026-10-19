#include "layout/Psd.hpp"
#include "core/Swatches.hpp"   // lab_to_rgb

#include <zlib.h>

#include <algorithm>
#include <cmath>

// Psd.cpp -- s185 (G5b): a PSD's merged picture. The file, section by
// section: the header (26 bytes); colour mode data (an indexed file's
// palette); image resources (skipped); layer and mask information (only the
// layer count's sign: negative = the first extra channel is the merged
// picture's transparency); then the image data -- each channel's rows, one
// channel after another.

namespace composix::layout {

namespace {
unsigned u8(const std::string& s, std::size_t i) { return i < s.size() ? static_cast<unsigned char>(s[i]) : 0u; }
std::uint32_t be16(const std::string& s, std::size_t i) { return (u8(s, i) << 8) | u8(s, i + 1); }
std::uint32_t be32(const std::string& s, std::size_t i) { return (be16(s, i) << 16) | be16(s, i + 2); }

// PackBits: one row's bytes from `src` (n of them), into `out` (rb of them).
bool unpack_row(const unsigned char* src, std::size_t n, unsigned char* out, std::size_t rb) {
    std::size_t i = 0, o = 0;
    while (i < n && o < rb) {
        const int c = static_cast<signed char>(src[i++]);
        if (c >= 0) {
            const std::size_t k = static_cast<std::size_t>(c) + 1;
            if (i + k > n) return false;
            const std::size_t take = std::min(k, rb - o);
            std::copy_n(src + i, take, out + o);
            i += k;
            o += take;
        } else if (c != -128) {
            if (i >= n) return false;
            const std::size_t k = static_cast<std::size_t>(1 - c);
            std::fill_n(out + o, std::min(k, rb - o), src[i++]);
            o += std::min(k, rb - o);
        }
    }
    if (o < rb) std::fill(out + o, out + rb, 0);
    return true;
}
}  // namespace

PsdInfo psd_info(const std::string& h) {
    PsdInfo i;
    if (h.size() < 26 || h.compare(0, 4, "8BPS") != 0 || be16(h, 4) != 1) return i;
    i.channels = static_cast<int>(be16(h, 12));
    i.h = static_cast<int>(be32(h, 14));
    i.w = static_cast<int>(be32(h, 18));
    i.depth = static_cast<int>(be16(h, 22));
    i.mode = static_cast<int>(be16(h, 24));
    i.ok = i.w > 0 && i.h > 0 && i.channels > 0 && i.channels <= 56;
    return i;
}

std::vector<std::uint32_t> psd_decode(const std::string& b, int& w, int& h, int max_side, std::string* why) {
    w = h = 0;
    const auto fail = [why](const char* m) { if (why) *why = m; return std::vector<std::uint32_t>{}; };
    if (b.size() >= 6 && b.compare(0, 4, "8BPS") == 0 && be16(b, 4) == 2) return fail("a large-document (PSB) file");
    const PsdInfo info = psd_info(b);
    if (!info.ok) return fail("not a Photoshop file");
    if (info.depth != 1 && info.depth != 8 && info.depth != 16) return fail("32-bit pictures aren't read");
    int base = 0;
    switch (info.mode) {
    case 0: case 1: case 2: case 8: base = 1; break;   // bitmap, gray, indexed, duotone (shown as its gray)
    case 3: case 9: base = 3; break;                   // RGB, Lab
    case 4: base = 4; break;                           // CMYK
    default: return fail("its colour mode (multichannel) isn't read");
    }
    if (info.channels < base) return fail("too few channels for its mode");
    // The sections before the picture.
    std::size_t at = 26;
    const std::uint32_t cm_len = be32(b, at);
    const std::size_t cm_at = at + 4;
    at = cm_at + cm_len;
    if (at + 4 > b.size()) return fail("cut short");
    at += 4 + be32(b, at);   // image resources
    if (at + 4 > b.size()) return fail("cut short");
    const std::uint32_t lm_len = be32(b, at);
    bool alpha = false;
    if (lm_len >= 6 && be32(b, at + 4) >= 2) alpha = static_cast<std::int16_t>(be16(b, at + 8)) < 0;
    at += 4 + lm_len;
    if (at + 2 > b.size()) return fail("cut short");
    const unsigned comp = be16(b, at);
    at += 2;
    const int nc = base + (alpha && info.channels > base ? 1 : 0);   // the channels used
    const std::size_t W = static_cast<std::size_t>(info.w), H = static_cast<std::size_t>(info.h);
    const std::size_t rb = info.depth == 1 ? (W + 7) / 8 : W * static_cast<std::size_t>(info.depth / 8);
    const std::size_t k = max_side > 0 ? std::max<std::size_t>(1, (std::max(W, H) + static_cast<std::size_t>(max_side) - 1) /
                                                                       static_cast<std::size_t>(max_side))
                                       : 1;
    const std::size_t w2 = (W + k - 1) / k, h2 = (H + k - 1) / k;
    std::vector<std::vector<unsigned char>> planes(static_cast<std::size_t>(nc), std::vector<unsigned char>(w2 * h2));
    const auto put_row = [&](std::size_t ch, std::size_t y, const unsigned char* row) {
        if (ch >= static_cast<std::size_t>(nc) || y % k) return;
        auto* out = planes[ch].data() + (y / k) * w2;
        for (std::size_t x2 = 0; x2 < w2; ++x2) {
            const std::size_t x = x2 * k;
            out[x2] = info.depth == 8 ? row[x] : info.depth == 16 ? row[2 * x]
                    : ((row[x >> 3] >> (7 - (x & 7))) & 1) ? 0 : 255;   // bitmap: 1 = black
        }
    };
    const std::size_t chans = static_cast<std::size_t>(info.channels);
    if (comp == 0) {   // raw
        if (at + static_cast<std::size_t>(nc) * H * rb > b.size()) return fail("cut short");
        for (std::size_t ch = 0; ch < static_cast<std::size_t>(nc); ++ch)
            for (std::size_t y = 0; y < H; ++y)
                put_row(ch, y, reinterpret_cast<const unsigned char*>(b.data()) + at + (ch * H + y) * rb);
    } else if (comp == 1) {   // PackBits, a byte count for every row of every channel first
        std::size_t data = at + chans * H * 2;
        if (data > b.size()) return fail("cut short");
        std::vector<unsigned char> row(rb);
        for (std::size_t ch = 0; ch < chans && ch < static_cast<std::size_t>(nc); ++ch)
            for (std::size_t y = 0; y < H; ++y) {
                const std::size_t n = be16(b, at + (ch * H + y) * 2);
                if (data + n > b.size()) return fail("cut short");
                if (y % k == 0) {
                    if (!unpack_row(reinterpret_cast<const unsigned char*>(b.data()) + data, n, row.data(), rb)) return fail("damaged");
                    put_row(ch, y, row.data());
                }
                data += n;
            }
    } else if (comp == 2 || comp == 3) {   // ZIP (3: each row's bytes as differences)
        std::vector<unsigned char> all(chans * H * rb);
        uLongf got = static_cast<uLongf>(all.size());
        if (uncompress(all.data(), &got, reinterpret_cast<const Bytef*>(b.data()) + at, static_cast<uLong>(b.size() - at)) != Z_OK)
            return fail("damaged");
        for (std::size_t ch = 0; ch < static_cast<std::size_t>(nc); ++ch)
            for (std::size_t y = 0; y < H; ++y) {
                unsigned char* row = all.data() + (ch * H + y) * rb;
                if (comp == 3 && info.depth == 8)
                    for (std::size_t x = 1; x < rb; ++x) row[x] = static_cast<unsigned char>(row[x] + row[x - 1]);
                if (comp == 3 && info.depth == 16)
                    for (std::size_t x = 1; x < W; ++x) {
                        const unsigned v = ((row[2 * x] << 8) | row[2 * x + 1]) + ((row[2 * x - 2] << 8) | row[2 * x - 1]);
                        row[2 * x] = static_cast<unsigned char>((v >> 8) & 0xFF);
                        row[2 * x + 1] = static_cast<unsigned char>(v & 0xFF);
                    }
                put_row(ch, y, row);
            }
    } else {
        return fail("its compression isn't read");
    }
    // The planes as cairo pixels.
    const std::string pal = info.mode == 2 && cm_len >= 768 ? b.substr(cm_at, 768) : std::string{};
    std::vector<std::uint32_t> px(w2 * h2);
    for (std::size_t i = 0; i < px.size(); ++i) {
        unsigned r = 0, g = 0, bl = 0;
        const auto p = [&](int c) { return static_cast<unsigned>(planes[static_cast<std::size_t>(c)][i]); };
        switch (info.mode) {
        case 3: r = p(0); g = p(1); bl = p(2); break;
        case 4: {   // stored as 255 - ink
            const unsigned kk = p(3);
            r = p(0) * kk / 255; g = p(1) * kk / 255; bl = p(2) * kk / 255;
            break;
        }
        case 9: {
            const std::uint32_t c = core::swatches::lab_to_rgb(p(0) * 100.0 / 255, static_cast<double>(p(1)) - 128, static_cast<double>(p(2)) - 128);
            r = (c >> 16) & 0xFF; g = (c >> 8) & 0xFF; bl = c & 0xFF;
            break;
        }
        case 2:
            if (!pal.empty()) { r = u8(pal, p(0)); g = u8(pal, 256 + p(0)); bl = u8(pal, 512 + p(0)); }
            else r = g = bl = p(0);
            break;
        default: r = g = bl = p(0); break;
        }
        const unsigned a = nc > base ? p(base) : 255;
        px[i] = (a << 24) | ((r * a / 255) << 16) | ((g * a / 255) << 8) | (bl * a / 255);
    }
    w = static_cast<int>(w2);
    h = static_cast<int>(h2);
    return px;
}

}  // namespace composix::layout
