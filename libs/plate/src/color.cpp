// Plate's colour management (s016): LittleCMS between sRGB and the press's
// CMYK output profile.
#include "internal.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace plate {

Document::Cms::~Cms() {
    if (img) cmsDeleteTransform(img);
    if (col) cmsDeleteTransform(col);
    if (out) cmsCloseProfile(out);
    if (srgb) cmsCloseProfile(srgb);
}

bool Document::Cms::rebuild(std::string* err) {
    if (img) { cmsDeleteTransform(img); img = nullptr; }
    if (col) { cmsDeleteTransform(col); col = nullptr; }
    static const cmsUInt32Number intents[] = {INTENT_PERCEPTUAL, INTENT_RELATIVE_COLORIMETRIC, INTENT_SATURATION,
                                              INTENT_ABSOLUTE_COLORIMETRIC};
    const auto in = intents[static_cast<int>(intent)];
    // Black point compensation keeps shadow detail with relative colorimetric.
    const cmsUInt32Number flags = intent == Intent::RelativeColorimetric ? cmsFLAGS_BLACKPOINTCOMPENSATION : 0;
    img = cmsCreateTransform(srgb, TYPE_RGB_8, out, TYPE_CMYK_8, in, flags);
    col = cmsCreateTransform(srgb, TYPE_RGB_DBL, out, TYPE_CMYK_DBL, in, flags);
    if (!img || !col) { if (err) *err = "couldn't build the colour transform"; return false; }
    return true;
}

bool Document::set_output_profile(const std::string& path, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { if (err) *err = "can't read " + path; return false; }
    std::stringstream ss;
    ss << f.rdbuf();
    return set_output_profile_bytes(ss.str(), err);
}

bool Document::set_output_profile_bytes(const std::string& icc, std::string* err) {
    auto c = std::make_unique<Cms>();
    c->out = cmsOpenProfileFromMem(icc.data(), static_cast<cmsUInt32Number>(icc.size()));
    if (!c->out) { if (err) *err = "not an ICC profile"; return false; }
    if (cmsGetColorSpace(c->out) != cmsSigCmykData || cmsGetDeviceClass(c->out) != cmsSigOutputClass) {
        if (err) *err = "not a CMYK output (press) profile";
        return false;
    }
    c->out_bytes = icc;
    c->srgb = cmsCreate_sRGBProfile();
    cmsUInt32Number n = 0;
    cmsSaveProfileToMem(c->srgb, nullptr, &n);
    c->srgb_bytes.resize(n);
    cmsSaveProfileToMem(c->srgb, c->srgb_bytes.data(), &n);
    char desc[256] = {0};
    cmsGetProfileInfoASCII(c->out, cmsInfoDescription, "en", "US", desc, sizeof desc);
    c->name = desc;
    if (m_cms) c->intent = m_cms->intent;
    if (!c->rebuild(err)) return false;
    m_cms = std::move(c);
    return true;
}

bool Document::has_output_profile() const { return m_cms != nullptr; }
std::string Document::output_profile_name() const { return m_cms ? m_cms->name : std::string{}; }

void Document::set_intent(Intent i) {
    if (!m_cms) return;
    m_cms->intent = i;
    m_cms->rebuild(nullptr);
}

Color Document::prepare(const Color& c) const {
    // PDF/X: an RGB colour never reaches the page (X-4 allows only ICC-tagged
    // RGB, which a bare colour isn't) -- with a profile, it is converted.
    const bool convert = convert_rgb() || (m_standard != Standard::None && m_cms);
    if (c.space != Color::Space::RGB || !convert) return c;
    double rgb[3] = {c.v[0], c.v[1], c.v[2]}, cmyk[4] = {0, 0, 0, 0};
    cmsDoTransform(m_cms->col, rgb, cmyk, 1);
    return Color::cmyk(cmyk[0] / 100, cmyk[1] / 100, cmyk[2] / 100, cmyk[3] / 100);   // lcms: 0..100
}

int Document::spot_index(const Color& c) {
    for (std::size_t i = 0; i < m_spots.size(); ++i)
        if (m_spots[i].name == c.name) return static_cast<int>(i);
    m_spots.push_back({c.name, {c.alt[0], c.alt[1], c.alt[2], c.alt[3]}});
    return static_cast<int>(m_spots.size() - 1);
}

std::vector<ProfileInfo> find_cmyk_profiles(const std::vector<std::string>& dirs) {
    namespace fs = std::filesystem;
    std::vector<ProfileInfo> out;
    std::error_code ec;
    for (const auto& dir : dirs) {
        if (!fs::is_directory(dir, ec)) continue;
        for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it.depth() > 1) { it.disable_recursion_pending(); continue; }
            if (!it->is_regular_file(ec)) continue;
            auto ext = it->path().extension().string();
            for (auto& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (ext != ".icc" && ext != ".icm") continue;
            cmsHPROFILE p = cmsOpenProfileFromFile(it->path().c_str(), "r");
            if (!p) continue;
            if (cmsGetColorSpace(p) == cmsSigCmykData && cmsGetDeviceClass(p) == cmsSigOutputClass) {
                char desc[256] = {0};
                cmsGetProfileInfoASCII(p, cmsInfoDescription, "en", "US", desc, sizeof desc);
                out.push_back({it->path().string(), desc[0] ? desc : it->path().stem().string()});
            }
            cmsCloseProfile(p);
        }
    }
    std::sort(out.begin(), out.end(), [](const ProfileInfo& a, const ProfileInfo& b) { return a.name < b.name; });
    return out;
}

namespace detail {
std::string pdf_name(const std::string& s) {
    std::string out = "/";
    for (unsigned char ch : s) {
        if (ch > 0x20 && ch < 0x7F && ch != '#' && ch != '/' && ch != '(' && ch != ')' && ch != '<' && ch != '>' &&
            ch != '[' && ch != ']' && ch != '{' && ch != '}' && ch != '%') {
            out += static_cast<char>(ch);
        } else {
            char buf[4];
            std::snprintf(buf, sizeof buf, "#%02X", ch);
            out += buf;
        }
    }
    return out == "/" ? "/Spot" : out;
}
}  // namespace detail

}  // namespace plate

// ── s053: soft proof ────────────────────────────────────────────────────────
namespace plate {

struct Proof::Impl {
    cmsHPROFILE   out = nullptr, srgb = nullptr, lab = nullptr;
    cmsHTRANSFORM to_cmyk = nullptr, back = nullptr, rgb_lab = nullptr, cmyk_lab = nullptr;
    cmsHTRANSFORM fast = nullptr;   // s099: sRGB 8-bit -> press -> sRGB 8-bit
    std::string   name;
    ~Impl() {
        for (auto t : {to_cmyk, back, rgb_lab, cmyk_lab, fast}) if (t) cmsDeleteTransform(t);
        for (auto p : {out, srgb, lab}) if (p) cmsCloseProfile(p);
    }
};

Proof::Proof() = default;
Proof::~Proof() = default;

bool Proof::load(const std::string& path, bool perceptual, std::string* err) {
    m.reset();
    auto i = std::make_unique<Impl>();
    i->out = cmsOpenProfileFromFile(path.c_str(), "r");
    if (!i->out) { if (err) *err = "can't read " + path; return false; }
    if (cmsGetColorSpace(i->out) != cmsSigCmykData || cmsGetDeviceClass(i->out) != cmsSigOutputClass) {
        if (err) *err = "not a CMYK output (press) profile";
        return false;
    }
    char desc[256] = {0};
    cmsGetProfileInfoASCII(i->out, cmsInfoDescription, "en", "US", desc, sizeof desc);
    i->name = desc;
    i->srgb = cmsCreate_sRGBProfile();
    i->lab = cmsCreateLab4Profile(nullptr);   // D50
    const cmsUInt32Number in = perceptual ? INTENT_PERCEPTUAL : INTENT_RELATIVE_COLORIMETRIC;
    const cmsUInt32Number fl = perceptual ? 0 : cmsFLAGS_BLACKPOINTCOMPENSATION;
    // As the export converts (see Cms::rebuild), then what that ink looks like.
    i->to_cmyk = cmsCreateTransform(i->srgb, TYPE_RGB_DBL, i->out, TYPE_CMYK_DBL, in, fl);
    i->back = cmsCreateTransform(i->out, TYPE_CMYK_DBL, i->srgb, TYPE_RGB_DBL, INTENT_RELATIVE_COLORIMETRIC, 0);
    i->rgb_lab = cmsCreateTransform(i->srgb, TYPE_RGB_DBL, i->lab, TYPE_Lab_DBL, INTENT_RELATIVE_COLORIMETRIC, 0);
    i->cmyk_lab = cmsCreateTransform(i->out, TYPE_CMYK_DBL, i->lab, TYPE_Lab_DBL, INTENT_RELATIVE_COLORIMETRIC, 0);
    {   // s099: the round trip in one transform, each leg with its own intent
        // The press profile twice: into its CMYK, then out of it (lcms runs a
        // chain's profiles alternately as output and input).
        cmsHPROFILE chain[4] = {i->srgb, i->out, i->out, i->srgb};
        cmsBool bpc[4] = {perceptual ? FALSE : TRUE, perceptual ? FALSE : TRUE, FALSE, FALSE};
        cmsUInt32Number intents[4] = {in, in, INTENT_RELATIVE_COLORIMETRIC, INTENT_RELATIVE_COLORIMETRIC};
        cmsFloat64Number adapt[4] = {0, 0, 0, 0};
        i->fast = cmsCreateExtendedTransform(nullptr, 4, chain, bpc, intents, adapt, nullptr, 0, TYPE_RGB_8, TYPE_RGB_8, 0);
    }
    if (!i->to_cmyk || !i->back || !i->rgb_lab || !i->cmyk_lab) {   // (no fast one: simulate goes pixel by pixel)
        if (err) *err = "couldn't build the proofing transforms";
        return false;
    }
    m = std::move(i);
    return true;
}

bool Proof::ok() const { return m != nullptr; }
std::string Proof::name() const { return m ? m->name : std::string{}; }

Proof::Result Proof::check(std::uint8_t r, std::uint8_t g, std::uint8_t b) const {
    Result res;
    if (!m) return res;
    const double rgb[3] = {r / 255.0, g / 255.0, b / 255.0};
    double cmyk[4] = {0, 0, 0, 0}, back[3] = {0, 0, 0};
    cmsCIELab l0{}, l1{};
    cmsDoTransform(m->to_cmyk, rgb, cmyk, 1);   // 0..100
    cmsDoTransform(m->back, cmyk, back, 1);
    cmsDoTransform(m->rgb_lab, rgb, &l0, 1);
    cmsDoTransform(m->cmyk_lab, cmyk, &l1, 1);
    for (int k = 0; k < 4; ++k) res.cmyk[k] = std::clamp(cmyk[k], 0.0, 100.0);
    for (int k = 0; k < 3; ++k)
        res.after[k] = static_cast<std::uint8_t>(std::lround(std::clamp(back[k], 0.0, 1.0) * 255.0));
    res.delta_e = cmsCIE2000DeltaE(&l0, &l1, 1, 1, 1);
    return res;
}

void Proof::simulate(std::uint8_t* rgb, std::size_t n) const {   // s099
    if (!m || !rgb || n == 0) return;
    if (m->fast) { cmsDoTransform(m->fast, rgb, rgb, static_cast<cmsUInt32Number>(n)); return; }
    proof_pixels(rgb, n);
}

void Proof::proof_pixels(std::uint8_t* rgb, std::size_t n, std::vector<float>* de) const {
    if (de) de->assign(n, 0.0f);
    if (!m || !rgb) return;
    for (std::size_t p = 0; p < n; ++p) {
        auto* px = rgb + p * 3;
        const auto r = check(px[0], px[1], px[2]);
        px[0] = r.after[0];
        px[1] = r.after[1];
        px[2] = r.after[2];
        if (de) (*de)[p] = static_cast<float>(r.delta_e);
    }
}

}  // namespace plate
