// core::swatches -- s171: colour library formats, My Library, the system's
// palettes. See core/Swatches.hpp.
#include "core/Swatches.hpp"

#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <regex>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace composix::core::swatches {

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

// ── Big-endian bytes ─────────────────────────────────────────────────────────
struct Reader {
    const std::string& b;
    std::size_t        at = 0;
    bool               bad = false;
    bool has(std::size_t n) { if (at + n > b.size()) bad = true; return !bad; }
    std::uint32_t u(int n) {
        if (!has(static_cast<std::size_t>(n))) return 0;
        std::uint32_t v = 0;
        for (int i = 0; i < n; ++i) v = (v << 8) | static_cast<unsigned char>(b[at++]);
        return v;
    }
    std::uint16_t u16() { return static_cast<std::uint16_t>(u(2)); }
    std::uint32_t u32() { return u(4); }
    float f32() {
        const std::uint32_t v = u32();
        float f;
        std::memcpy(&f, &v, 4);
        return f;
    }
    // UTF-16BE, `units` code units (a terminating 0 dropped), as UTF-8.
    std::string utf16(std::size_t units) {
        std::u32string cps;
        for (std::size_t i = 0; i < units && !bad; ++i) {
            char32_t c = u16();
            if (c >= 0xD800 && c < 0xDC00 && i + 1 < units) {
                const char32_t lo = u16();
                ++i;
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
            }
            if (c == 0) continue;
            cps.push_back(c);
        }
        std::string out;
        for (char32_t c : cps) {
            if (c < 0x80) out += static_cast<char>(c);
            else if (c < 0x800) { out += static_cast<char>(0xC0 | (c >> 6)); out += static_cast<char>(0x80 | (c & 0x3F)); }
            else if (c < 0x10000) {
                out += static_cast<char>(0xE0 | (c >> 12));
                out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (c & 0x3F));
            } else {
                out += static_cast<char>(0xF0 | (c >> 18));
                out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (c & 0x3F));
            }
        }
        return out;
    }
};

void put(std::string& o, std::uint32_t v, int n) {
    for (int i = n - 1; i >= 0; --i) o += static_cast<char>((v >> (8 * i)) & 0xFF);
}
void put_f32(std::string& o, float f) {
    std::uint32_t v;
    std::memcpy(&v, &f, 4);
    put(o, v, 4);
}
// UTF-8 to UTF-16 code units (no terminator).
std::vector<std::uint16_t> utf16_of(const std::string& s) {
    std::vector<std::uint16_t> out;
    for (std::size_t i = 0; i < s.size();) {
        const auto c0 = static_cast<unsigned char>(s[i]);
        char32_t c = c0;
        int n = 1;
        if (c0 >= 0xF0) { c = c0 & 0x07; n = 4; }
        else if (c0 >= 0xE0) { c = c0 & 0x0F; n = 3; }
        else if (c0 >= 0xC0) { c = c0 & 0x1F; n = 2; }
        for (int k = 1; k < n && i + static_cast<std::size_t>(k) < s.size(); ++k)
            c = (c << 6) | (static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]) & 0x3F);
        i += static_cast<std::size_t>(n);
        if (c >= 0x10000) {
            c -= 0x10000;
            out.push_back(static_cast<std::uint16_t>(0xD800 + (c >> 10)));
            out.push_back(static_cast<std::uint16_t>(0xDC00 + (c & 0x3FF)));
        } else out.push_back(static_cast<std::uint16_t>(c));
    }
    return out;
}

Swatch rgb_swatch(const std::string& name, double r, double g, double b) {   // 0-255
    Swatch s;
    s.name = name;
    s.model = ColorModel::RGB;
    s.v = {std::clamp(r, 0.0, 255.0), std::clamp(g, 0.0, 255.0), std::clamp(b, 0.0, 255.0), 0};
    s.preview = doc::swatch_preview(s);
    return s;
}
Swatch cmyk_swatch(const std::string& name, double c, double m, double y, double k) {   // 0-100
    Swatch s;
    s.name = name;
    s.model = ColorModel::CMYK;
    s.v = {std::clamp(c, 0.0, 100.0), std::clamp(m, 0.0, 100.0), std::clamp(y, 0.0, 100.0), std::clamp(k, 0.0, 100.0)};
    s.preview = doc::swatch_preview(s);
    return s;
}
Swatch gray_swatch(const std::string& name, double ink) {   // 0-100, 100 = black
    Swatch s;
    s.name = name;
    s.model = ColorModel::Gray;
    s.v = {std::clamp(ink, 0.0, 100.0), 0, 0, 0};
    s.preview = doc::swatch_preview(s);
    return s;
}
Swatch lab_swatch(const std::string& name, double L, double a, double b) {
    const std::uint32_t rgb = lab_to_rgb(L, a, b);
    return rgb_swatch(name, (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}
// A process colour made a spot ink: its values become the CMYK alternate.
void make_spot(Swatch& s) {
    if (s.model == ColorModel::Spot) return;
    if (s.model != ColorModel::CMYK) {
        const std::uint32_t rgb = s.preview;
        s.model = ColorModel::CMYK;
        doc::swatch_from_rgb(s, rgb);
        s.preview = rgb;   // the screen keeps the library's colour
    }
    s.model = ColorModel::Spot;
    s.spot = s.name;
}

bool same_values(const Swatch& a, const Swatch& b) {
    if (a.model != b.model) return false;
    for (int i = 0; i < 4; ++i)
        if (std::abs(a.v[static_cast<std::size_t>(i)] - b.v[static_cast<std::size_t>(i)]) > 0.5) return false;
    return a.model != ColorModel::Spot || a.spot == b.spot;
}
std::string numbered(const std::string& name, const std::function<bool(const std::string&)>& taken) {
    for (int n = 2;; ++n) {
        const std::string t = name + " " + std::to_string(n);
        if (!taken(t)) return t;
    }
}

// ── XML, just enough: a tag's attributes, entities decoded ───────────────────
std::string unxml(const std::string& s) {
    std::string o;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { o += s[i]; continue; }
        const auto semi = s.find(';', i);
        if (semi == std::string::npos) { o += s[i]; continue; }
        const std::string e = s.substr(i + 1, semi - i - 1);
        char32_t c = 0;
        if (e == "amp") c = '&';
        else if (e == "lt") c = '<';
        else if (e == "gt") c = '>';
        else if (e == "quot") c = '"';
        else if (e == "apos") c = '\'';
        else if (!e.empty() && e[0] == '#')
            c = static_cast<char32_t>(std::strtoul(e.c_str() + (e.size() > 1 && (e[1] == 'x' || e[1] == 'X') ? 2 : 1), nullptr,
                                                   e.size() > 1 && (e[1] == 'x' || e[1] == 'X') ? 16 : 10));
        if (!c) { o += s[i]; continue; }
        if (c < 0x80) o += static_cast<char>(c);
        else if (c < 0x800) { o += static_cast<char>(0xC0 | (c >> 6)); o += static_cast<char>(0x80 | (c & 0x3F)); }
        else { o += static_cast<char>(0xE0 | (c >> 12)); o += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); o += static_cast<char>(0x80 | (c & 0x3F)); }
        i = semi;
    }
    return o;
}
// Each <tag ...> (case-insensitive name; namespace prefix allowed): its
// attributes, keys lower-cased with any prefix dropped.
std::vector<std::map<std::string, std::string>> tags(const std::string& xml, const std::string& name) {
    std::vector<std::map<std::string, std::string>> out;
    const std::string lx = lower(xml), ln = lower(name);
    static const std::regex attr(R"(([A-Za-z_][\w:.-]*)\s*=\s*(?:"([^"]*)\"|'([^']*)'))");
    for (std::size_t at = 0;;) {
        at = lx.find('<', at);
        if (at == std::string::npos) break;
        std::size_t p = at + 1;
        std::size_t e = p;
        while (e < lx.size() && !std::isspace(static_cast<unsigned char>(lx[e])) && lx[e] != '>' && lx[e] != '/') ++e;
        std::string tn = lx.substr(p, e - p);
        if (const auto c = tn.find(':'); c != std::string::npos) tn = tn.substr(c + 1);
        const auto close = xml.find('>', e);
        if (close == std::string::npos) break;
        if (tn == ln) {
            std::map<std::string, std::string> a;
            const std::string body = xml.substr(e, close - e);
            for (auto it = std::sregex_iterator(body.begin(), body.end(), attr); it != std::sregex_iterator(); ++it) {
                std::string k = lower((*it)[1].str());
                if (const auto c = k.find(':'); c != std::string::npos) k = k.substr(c + 1);
                a[k] = unxml((*it)[2].matched ? (*it)[2].str() : (*it)[3].str());
            }
            out.push_back(std::move(a));
        }
        at = close;
    }
    return out;
}
bool hex_rgb(std::string t, std::uint32_t& out) {
    t = trim(t);
    if (!t.empty() && t[0] == '#') t.erase(0, 1);
    if (t.size() != 6 || !std::all_of(t.begin(), t.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); })) return false;
    out = static_cast<std::uint32_t>(std::stoul(t, nullptr, 16));
    return true;
}
double num(const std::map<std::string, std::string>& a, const char* k, double fb = 0) {
    const auto it = a.find(k);
    if (it == a.end()) return fb;
    char* end = nullptr;
    const double v = std::strtod(it->second.c_str(), &end);
    return end == it->second.c_str() ? fb : v;
}
std::string attr_of(const std::map<std::string, std::string>& a, const char* k) {
    const auto it = a.find(k);
    return it == a.end() ? std::string() : it->second;
}

// ── The formats ──────────────────────────────────────────────────────────────
std::optional<Library> parse_ase(const std::string& b, std::string* err) {
    Reader r{b};
    if (b.size() < 12 || b.compare(0, 4, "ASEF") != 0) { if (err) *err = "not an Adobe Swatch Exchange file"; return std::nullopt; }
    r.at = 4;
    r.u16(); r.u16();   // version 1.0
    const std::uint32_t blocks = r.u32();
    Library lib;
    lib.format = Format::ASE;
    for (std::uint32_t i = 0; i < blocks && !r.bad && r.at < b.size(); ++i) {
        const std::uint16_t type = r.u16();
        const std::uint32_t len = r.u32();
        const std::size_t next = r.at + len;
        if (next > b.size()) break;
        if (type == 0x0001) {   // a colour
            const std::uint16_t nlen = r.u16();
            const std::string name = r.utf16(nlen);
            if (!r.has(4)) break;
            const std::string model = b.substr(r.at, 4);
            r.at += 4;
            Swatch s;
            if (model == "CMYK") {
                const double c = r.f32(), m = r.f32(), y = r.f32(), k = r.f32();
                s = cmyk_swatch(name, c * 100, m * 100, y * 100, k * 100);
            } else if (model == "RGB ") {
                const double R = r.f32(), G = r.f32(), B = r.f32();
                s = rgb_swatch(name, R * 255, G * 255, B * 255);
            } else if (model == "LAB ") {
                const double L = r.f32(), A = r.f32(), Bb = r.f32();
                s = lab_swatch(name, L * 100, A, Bb);
            } else if (model == "Gray") {
                s = gray_swatch(name, (1 - r.f32()) * 100);   // 0 black, 1 white
            } else { r.at = next; continue; }
            const std::uint16_t kind = r.u16();   // 0 global, 1 spot, 2 normal
            if (kind == 1) make_spot(s);
            if (!r.bad) lib.colors.push_back(s);
        }   // groups (0xC001 / 0xC002): their colours come in, flat
        r.at = next;
    }
    if (r.bad && lib.colors.empty()) { if (err) *err = "the file is cut short"; return std::nullopt; }
    return lib;
}

std::optional<Library> parse_gpl(const std::string& text, std::string* err) {
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line) || trim(line).rfind("GIMP Palette", 0) != 0) {
        if (err) *err = "not a GIMP palette";
        return std::nullopt;
    }
    Library lib;
    lib.format = Format::GPL;
    int n = 0;
    while (std::getline(in, line)) {
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        if (t.rfind("Name:", 0) == 0) { lib.name = trim(t.substr(5)); continue; }
        if (t.rfind("Columns:", 0) == 0) continue;
        std::istringstream ls(t);
        int R, G, B;
        if (!(ls >> R >> G >> B)) continue;
        std::string name;
        std::getline(ls, name);
        name = trim(name);
        ++n;
        if (name.empty() || name == "Untitled") {
            char h[8];
            std::snprintf(h, sizeof h, "#%02X%02X%02X", R & 0xFF, G & 0xFF, B & 0xFF);
            name = h;
        }
        lib.colors.push_back(rgb_swatch(name, R, G, B));
    }
    return lib;
}

std::optional<Library> parse_soc(const std::string& xml, std::string* err) {
    Library lib;
    lib.format = Format::SOC;
    for (const auto& a : tags(xml, "color")) {
        std::uint32_t rgb;
        if (!hex_rgb(attr_of(a, "color"), rgb)) continue;
        std::string name = attr_of(a, "name");
        if (name.empty()) name = attr_of(a, "color");
        lib.colors.push_back(rgb_swatch(name, (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF));
    }
    if (lib.colors.empty() && xml.find("color-table") == std::string::npos) {
        if (err) *err = "not a LibreOffice palette";
        return std::nullopt;
    }
    return lib;
}

std::optional<Library> parse_scribus(const std::string& xml, std::string* err) {
    Library lib;
    lib.format = Format::Scribus;
    if (const auto roots = tags(xml, "SCRIBUSCOLORS"); !roots.empty()) lib.name = attr_of(roots.front(), "name");
    else { if (err) *err = "not a Scribus swatch file"; return std::nullopt; }
    for (const auto& a : tags(xml, "COLOR")) {
        const std::string name = attr_of(a, "name");
        if (name.empty()) continue;
        Swatch s;
        const std::string space = lower(attr_of(a, "space"));
        std::uint32_t rgb;
        if (const std::string c = attr_of(a, "cmyk"); c.size() == 9 && c[0] == '#') {   // #CCMMYYKK, 0-255 each
            const auto byte = [&](int i) { return std::stoul(c.substr(1 + 2 * static_cast<std::size_t>(i), 2), nullptr, 16) * 100.0 / 255; };
            s = cmyk_swatch(name, byte(0), byte(1), byte(2), byte(3));
        } else if (hex_rgb(attr_of(a, "rgb"), rgb)) {
            s = rgb_swatch(name, (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
        } else if (space == "cmyk") {
            s = cmyk_swatch(name, num(a, "c"), num(a, "m"), num(a, "y"), num(a, "k"));
        } else if (space == "rgb") {
            s = rgb_swatch(name, num(a, "r"), num(a, "g"), num(a, "b"));
        } else if (space == "lab") {
            s = lab_swatch(name, num(a, "l"), num(a, "a"), num(a, "b"));
        } else continue;
        if (attr_of(a, "spot") == "1") make_spot(s);
        lib.colors.push_back(s);
    }
    return lib;
}

std::optional<Library> parse_aco(const std::string& b, std::string* err) {
    Reader r{b};
    const std::uint16_t v1 = r.u16();
    const std::uint16_t n1 = r.u16();
    if (r.bad || (v1 != 1 && v1 != 2)) { if (err) *err = "not a Photoshop swatch file"; return std::nullopt; }
    const auto colour = [&](Reader& rr, const std::string& name) -> std::optional<Swatch> {
        const std::uint16_t space = rr.u16();
        const double w = rr.u16(), x = rr.u16(), y = rr.u16(), z = rr.u16();
        if (rr.bad) return std::nullopt;
        switch (space) {
        case 0: return rgb_swatch(name, w / 257, x / 257, y / 257);
        case 1: {   // HSB
            const double h = w / 65535 * 360, s = x / 65535, v = y / 65535;
            const double c = v * s, hp = h / 60, xx = c * (1 - std::abs(std::fmod(hp, 2) - 1)), m = v - c;
            double R = 0, G = 0, B = 0;
            if (hp < 1) { R = c; G = xx; } else if (hp < 2) { R = xx; G = c; } else if (hp < 3) { G = c; B = xx; }
            else if (hp < 4) { G = xx; B = c; } else if (hp < 5) { R = xx; B = c; } else { R = c; B = xx; }
            return rgb_swatch(name, (R + m) * 255, (G + m) * 255, (B + m) * 255);
        }
        case 2: return cmyk_swatch(name, 100 * (1 - w / 65535), 100 * (1 - x / 65535), 100 * (1 - y / 65535), 100 * (1 - z / 65535));
        case 7: return lab_swatch(name, w / 100, static_cast<std::int16_t>(static_cast<std::uint16_t>(x)) / 100.0,
                                  static_cast<std::int16_t>(static_cast<std::uint16_t>(y)) / 100.0);
        case 8: return gray_swatch(name, w / 100);
        default: return std::nullopt;
        }
    };
    Library lib;
    lib.format = Format::ACO;
    std::vector<Swatch> first;
    if (v1 == 1) {
        for (int i = 0; i < n1 && !r.bad; ++i)
            if (auto s = colour(r, "")) first.push_back(*s);
        // A version 2 section after: the same colours, named.
        if (r.at + 4 <= b.size()) {
            Reader r2{b, r.at};
            const std::uint16_t v2 = r2.u16();
            const std::uint16_t n2 = r2.u16();
            if (v2 == 2) {
                std::vector<Swatch> named;
                for (int i = 0; i < n2 && !r2.bad; ++i) {
                    const std::size_t at = r2.at;
                    r2.at += 10;
                    const std::uint32_t len = r2.u32();
                    const std::string name = r2.utf16(len);
                    const std::size_t after = r2.at;
                    r2.at = at;
                    auto s = colour(r2, name);
                    r2.at = after;
                    if (s && !r2.bad) named.push_back(*s);
                }
                if (!r2.bad && !named.empty()) first = std::move(named);
            }
        }
    } else {
        for (int i = 0; i < n1 && !r.bad; ++i) {
            const std::size_t at = r.at;
            r.at += 10;
            const std::uint32_t len = r.u32();
            const std::string name = r.utf16(len);
            const std::size_t after = r.at;
            r.at = at;
            auto s = colour(r, name);
            r.at = after;
            if (s && !r.bad) first.push_back(*s);
        }
    }
    int k = 0;
    for (auto& s : first) if (s.name.empty()) s.name = "Color " + std::to_string(++k);
    if (first.empty() && r.bad) { if (err) *err = "the file is cut short"; return std::nullopt; }
    lib.colors = std::move(first);
    return lib;
}

std::optional<Library> parse_json(const std::string& text, std::string* err) {
    try {
        const auto j = nlohmann::json::parse(text);
        if (!j.is_object() || j.value("kind", "") != "composix-swatches") throw std::runtime_error("kind");
        Library lib;
        lib.format = Format::Composix;
        lib.name = j.value("name", "");
        for (const auto& c : j.value("colors", nlohmann::json::array())) {
            Swatch s;
            s.name = c.value("name", "");
            const std::string m = c.value("model", "cmyk");
            s.model = m == "rgb" ? ColorModel::RGB : m == "gray" ? ColorModel::Gray : m == "spot" ? ColorModel::Spot : ColorModel::CMYK;
            s.v = c.value("v", std::array<double, 4>{});
            s.spot = c.value("spot", "");
            s.from = c.value("from", "");
            s.preview = c.value("preview", doc::swatch_preview(s));
            if (!s.name.empty()) lib.colors.push_back(s);
        }
        return lib;
    } catch (const std::exception&) {
        if (err) *err = "not a Composix colour library";
        return std::nullopt;
    }
}

std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace

const char* format_name(Format f) {
    switch (f) {
    case Format::ASE:      return "Adobe Swatch Exchange";
    case Format::GPL:      return "GIMP palette";
    case Format::SOC:      return "LibreOffice palette";
    case Format::Scribus:  return "Scribus swatches";
    case Format::ACO:      return "Photoshop swatches";
    case Format::Composix: return "Composix library";
    default:               return "Unknown";
    }
}

bool writable(Format f) { return f == Format::ASE || f == Format::GPL || f == Format::Composix; }

Format format_of(const std::string& path, const std::string& bytes) {
    if (bytes.compare(0, 4, "ASEF") == 0) return Format::ASE;
    std::size_t i = 0;
    if (bytes.compare(0, 3, "\xEF\xBB\xBF") == 0) i = 3;
    while (i < bytes.size() && std::isspace(static_cast<unsigned char>(bytes[i]))) ++i;
    if (bytes.compare(i, 12, "GIMP Palette") == 0) return Format::GPL;
    const std::string head = lower(bytes.substr(0, 4096));
    if (head.find("<scribuscolors") != std::string::npos) return Format::Scribus;
    if (head.find("color-table") != std::string::npos || head.find("<draw:color") != std::string::npos) return Format::SOC;
    if (head.find("composix-swatches") != std::string::npos) return Format::Composix;
    const std::string ext = lower(fs::path(path).extension().string());
    if (ext == ".aco" && bytes.size() >= 4 && bytes[0] == 0 && (bytes[1] == 1 || bytes[1] == 2)) return Format::ACO;
    return Format::Unknown;
}

std::optional<Library> parse(const std::string& bytes, Format f, std::string* err) {
    switch (f) {
    case Format::ASE:      return parse_ase(bytes, err);
    case Format::GPL:      return parse_gpl(bytes, err);
    case Format::SOC:      return parse_soc(bytes, err);
    case Format::Scribus:  return parse_scribus(bytes, err);
    case Format::ACO:      return parse_aco(bytes, err);
    case Format::Composix: return parse_json(bytes, err);
    default:
        if (err) *err = "not a colour library Composix reads";
        return std::nullopt;
    }
}

std::optional<Library> read_file(const std::string& path, std::string* err) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) { if (err) *err = "no such file"; return std::nullopt; }
    const std::string bytes = slurp(path);
    auto lib = parse(bytes, format_of(path, bytes), err);
    if (!lib) return std::nullopt;
    lib->path = path;
    if (lib->name.empty()) lib->name = fs::path(path).stem().string();
    return lib;
}

std::string to_ase(const std::vector<Swatch>& colors) {
    std::string o = "ASEF";
    put(o, 1, 2);
    put(o, 0, 2);
    put(o, static_cast<std::uint32_t>(colors.size()), 4);
    for (const auto& s : colors) {
        const std::string name = s.model == ColorModel::Spot && !s.spot.empty() ? s.spot : s.name;
        const auto u = utf16_of(name);
        std::string blk;
        put(blk, static_cast<std::uint32_t>(u.size() + 1), 2);
        for (auto c : u) put(blk, c, 2);
        put(blk, 0, 2);
        if (s.model == ColorModel::RGB) {
            blk += "RGB ";
            for (int i = 0; i < 3; ++i) put_f32(blk, static_cast<float>(s.v[static_cast<std::size_t>(i)] / 255));
        } else if (s.model == ColorModel::Gray) {
            blk += "Gray";
            put_f32(blk, static_cast<float>(1 - s.v[0] / 100));
        } else {
            blk += "CMYK";
            for (int i = 0; i < 4; ++i) put_f32(blk, static_cast<float>(s.v[static_cast<std::size_t>(i)] / 100));
        }
        put(blk, s.model == ColorModel::Spot ? 1 : 2, 2);
        put(o, 0x0001, 2);
        put(o, static_cast<std::uint32_t>(blk.size()), 4);
        o += blk;
    }
    return o;
}

std::string to_gpl(const std::string& name, const std::vector<Swatch>& colors) {
    std::string o = "GIMP Palette\nName: " + name + "\nColumns: 0\n#\n";
    for (const auto& s : colors) {
        const std::uint32_t c = s.preview;
        char line[32];
        std::snprintf(line, sizeof line, "%3u %3u %3u\t", (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
        o += line;
        std::string n = s.name;
        std::replace(n.begin(), n.end(), '\n', ' ');
        o += n + "\n";
    }
    return o;
}

std::string to_json(const std::string& name, const std::vector<Swatch>& colors) {
    nlohmann::json j{{"kind", "composix-swatches"}, {"version", 1}, {"name", name}, {"colors", nlohmann::json::array()}};
    for (const auto& s : colors) {
        const char* m = s.model == ColorModel::RGB ? "rgb" : s.model == ColorModel::Gray ? "gray" : s.model == ColorModel::Spot ? "spot" : "cmyk";
        nlohmann::json c{{"name", s.name}, {"model", m}, {"v", s.v}, {"preview", s.preview}};
        if (!s.spot.empty()) c["spot"] = s.spot;
        if (!s.from.empty()) c["from"] = s.from;
        j["colors"].push_back(c);
    }
    return j.dump(2) + "\n";
}

bool write_file(const std::string& path, const std::string& name, const std::vector<Swatch>& colors, Format f, std::string* err) {
    std::string bytes;
    if (f == Format::ASE) bytes = to_ase(colors);
    else if (f == Format::GPL) bytes = to_gpl(name, colors);
    else if (f == Format::Composix) bytes = to_json(name, colors);
    else { if (err) *err = std::string("Composix doesn't write ") + format_name(f) + " files"; return false; }
    std::error_code ec;
    if (fs::path(path).has_parent_path()) fs::create_directories(fs::path(path).parent_path(), ec);
    std::ofstream o(path, std::ios::binary | std::ios::trunc);
    if (!o) { if (err) *err = "can't write " + path; return false; }
    o.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!o) { if (err) *err = "can't write " + path; return false; }
    return true;
}

std::uint32_t lab_to_rgb(double L, double a, double b) {
    const double fy = (L + 16) / 116, fx = fy + a / 500, fz = fy - b / 200;
    const auto inv = [](double t) { return t > 6.0 / 29 ? t * t * t : 3 * (6.0 / 29) * (6.0 / 29) * (t - 4.0 / 29); };
    const double X = 0.96422 * inv(fx), Y = inv(fy), Z = 0.82521 * inv(fz);
    // XYZ (D50) to linear sRGB (Bradford-adapted to D65).
    const double r = 3.1338561 * X - 1.6168667 * Y - 0.4906146 * Z;
    const double g = -0.9787684 * X + 1.9161415 * Y + 0.0334540 * Z;
    const double bl = 0.0719453 * X - 0.2289914 * Y + 1.4052427 * Z;
    const auto enc = [](double v) {
        v = std::clamp(v, 0.0, 1.0);
        v = v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1 / 2.4) - 0.055;
        return static_cast<std::uint32_t>(std::lround(v * 255));
    };
    return (enc(r) << 16) | (enc(g) << 8) | enc(bl);
}

Library load_mine(const std::string& file) {
    Library lib;
    if (auto got = read_file(file)) lib = *got;
    lib.name = "My Library";
    lib.path = file;
    lib.kind = LibKind::Mine;
    lib.format = Format::Composix;
    lib.source = "Composix";
    return lib;
}

bool save_mine(const Library& lib) {
    return !lib.path.empty() && write_file(lib.path, "My Library", lib.colors, Format::Composix);
}

int merge_into(Library& lib, const std::vector<Swatch>& colors) {
    int n = 0;
    for (auto s : colors) {
        bool dup = false, taken = false;
        for (const auto& c : lib.colors)
            if (c.name == s.name) { taken = true; dup = dup || same_values(c, s); }
        if (dup) continue;
        if (taken)
            s.name = numbered(s.name, [&](const std::string& t) {
                return std::any_of(lib.colors.begin(), lib.colors.end(), [&](const Swatch& c) { return c.name == t; });
            });
        lib.colors.push_back(s);
        ++n;
    }
    return n;
}

std::vector<Found> system_palettes(const std::string& home, const std::vector<std::string>& data_dirs,
                                   const std::vector<Found>& extra) {
    std::vector<std::pair<fs::path, std::string>> dirs;   // folder, source
    for (const auto& d : data_dirs) {
        const fs::path p(d);
        dirs.push_back({p / "gimp" / "2.0" / "palettes", "GIMP"});
        dirs.push_back({p / "gimp" / "3.0" / "palettes", "GIMP"});
        dirs.push_back({p / "inkscape" / "palettes", "Inkscape"});
        dirs.push_back({p / "scribus" / "swatches", "Scribus"});
        dirs.push_back({p / "krita" / "palettes", "Krita"});
    }
    if (!home.empty()) {
        const fs::path h(home);
        for (const char* v : {"2.10", "2.8", "3.0", "3.2"}) {
            dirs.push_back({h / ".config" / "GIMP" / v / "palettes", "GIMP"});
            dirs.push_back({h / ".var" / "app" / "org.gimp.GIMP" / "config" / "GIMP" / v / "palettes", "GIMP"});
        }
        dirs.push_back({h / ".config" / "inkscape" / "palettes", "Inkscape"});
        dirs.push_back({h / ".var" / "app" / "org.inkscape.Inkscape" / "config" / "inkscape" / "palettes", "Inkscape"});
        dirs.push_back({h / ".config" / "libreoffice" / "4" / "user" / "config", "LibreOffice"});
    }
    for (const auto& e : extra) dirs.push_back({e.path, e.source});

    std::vector<Found> out;
    std::set<std::string> seen;   // canonical paths
    std::set<std::string> names;  // a palette shipped twice (2.0 and 3.0, flatpak) shows once: source + file name
    for (const auto& [dir, src] : dirs) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (it.depth() > 2) { it.disable_recursion_pending(); continue; }
            if (!it->is_regular_file(ec)) continue;
            const std::string ext = lower(it->path().extension().string());
            if (ext != ".gpl" && ext != ".ase" && ext != ".soc" && ext != ".aco" && ext != ".xml") continue;
            const std::string canon = fs::weakly_canonical(it->path(), ec).string();
            if (!seen.insert(canon).second) continue;
            if (!names.insert(src + "/" + lower(it->path().filename().string())).second) continue;
            if (ext == ".xml" || ext == ".soc") {   // only palettes, not any XML in the folder
                const std::string head = lower(slurp(it->path().string()).substr(0, 4096));
                if (head.find("<scribuscolors") == std::string::npos && head.find("color-table") == std::string::npos) continue;
            }
            out.push_back({it->path().string(), src});
        }
    }
    std::sort(out.begin(), out.end(), [](const Found& a, const Found& b) {
        if (a.source != b.source) return a.source < b.source;
        return lower(fs::path(a.path).stem().string()) < lower(fs::path(b.path).stem().string());
    });
    return out;
}

std::vector<Found> system_palettes() {
    const char* h = std::getenv("HOME");
    std::vector<std::string> data;
    const char* xh = std::getenv("XDG_DATA_HOME");
    if (xh && *xh) data.push_back(xh);
    else if (h && *h) data.push_back(std::string(h) + "/.local/share");
    const char* xd = std::getenv("XDG_DATA_DIRS");
    std::string dd = xd && *xd ? xd : "/usr/local/share:/usr/share";
    for (std::size_t a = 0; a <= dd.size();) {
        const auto b = dd.find(':', a);
        const std::string part = dd.substr(a, b == std::string::npos ? std::string::npos : b - a);
        if (!part.empty()) data.push_back(part);
        if (b == std::string::npos) break;
        a = b + 1;
    }
    std::vector<Found> fixed;   // installs outside the data folders
    for (const char* lo : {"/usr/lib64/libreoffice/share/palette", "/usr/lib/libreoffice/share/palette",
                           "/opt/libreoffice/share/palette", "/usr/share/libreoffice/share/palette"})
        fixed.push_back({lo, "LibreOffice"});
    for (const char* fp : {"/var/lib/flatpak/app/org.inkscape.Inkscape/current/active/files/share/inkscape/palettes",
                           "/var/lib/flatpak/app/org.gimp.GIMP/current/active/files/share/gimp/2.0/palettes",
                           "/var/lib/flatpak/app/org.gimp.GIMP/current/active/files/share/gimp/3.0/palettes",
                           "/var/lib/flatpak/app/org.libreoffice.LibreOffice/current/active/files/libreoffice/share/palette"})
        fixed.push_back({fp, std::string(fp).find("inkscape") != std::string::npos ? "Inkscape"
                            : std::string(fp).find("gimp") != std::string::npos ? "GIMP" : "LibreOffice"});
    return system_palettes(h ? h : "", data, fixed);
}

// ── s177: a picture's colours ────────────────────────────────────────────────
std::vector<PictureColor> picture_colors(const std::uint32_t* px, int w, int h, int stride, int most) {
    std::vector<PictureColor> out;
    if (!px || w <= 0 || h <= 0 || stride < w || most <= 0) return out;
    // Every pixel of a small picture; a big one sampled evenly (about 250k).
    const long total = static_cast<long>(w) * h;
    const int step = std::max(1, static_cast<int>(std::sqrt(static_cast<double>(total) / 250000.0)));
    struct Bin { double r = 0, g = 0, b = 0; long n = 0; };
    std::vector<Bin> bins(1 << 15);   // 5 bits a channel
    long counted = 0;
    for (int y = 0; y < h; y += step)
        for (int x = 0; x < w; x += step) {
            const std::uint32_t c = px[static_cast<std::size_t>(y) * static_cast<std::size_t>(stride) + static_cast<std::size_t>(x)];
            const int a = static_cast<int>(c >> 24);
            if (a < 128) continue;
            // Premultiplied (cairo): back to the colour.
            const auto un = [a](std::uint32_t v) { return a >= 255 ? static_cast<double>(v) : std::min(255.0, v * 255.0 / a); };
            const double r = un((c >> 16) & 0xff), g = un((c >> 8) & 0xff), b = un(c & 0xff);
            const int k = (static_cast<int>(r) >> 3) << 10 | (static_cast<int>(g) >> 3) << 5 | (static_cast<int>(b) >> 3);
            auto& bin = bins[static_cast<std::size_t>(k)];
            bin.r += r; bin.g += g; bin.b += b; ++bin.n;
            ++counted;
        }
    if (counted == 0) return out;
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < bins.size(); ++i) if (bins[i].n) order.push_back(i);
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return bins[a].n > bins[b].n; });
    // Greedy, busiest first: a bin near a chosen colour joins it; else it's a new one.
    struct Pick { double r, g, b; long n; };
    std::vector<Pick> picks;
    constexpr double kNear = 40.0;   // RGB distance that reads as the same colour
    for (auto i : order) {
        const auto& bn = bins[i];
        const double r = bn.r / bn.n, g = bn.g / bn.n, b = bn.b / bn.n;
        Pick* at = nullptr;
        double best = kNear;
        for (auto& p : picks) {
            const double d = std::sqrt((p.r - r) * (p.r - r) + (p.g - g) * (p.g - g) + (p.b - b) * (p.b - b));
            if (d < best) { best = d; at = &p; }
        }
        if (at) {   // pooled: the chosen colour stays the busiest bin's own
            at->n += bn.n;
        } else {
            picks.push_back({r, g, b, bn.n});
        }
    }
    std::stable_sort(picks.begin(), picks.end(), [](const Pick& a, const Pick& b) { return a.n > b.n; });
    for (const auto& p : picks) {
        const double share = static_cast<double>(p.n) / counted;
        if (share < 0.01 || static_cast<int>(out.size()) >= most) break;
        const auto c8 = [](double v) { return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0, 255.0))); };
        out.push_back({(c8(p.r) << 16) | (c8(p.g) << 8) | c8(p.b), share});
    }
    return out;
}

Library picture_library(const std::string& name, const std::vector<PictureColor>& colors) {
    Library lib;
    lib.name = name;
    lib.source = "Pictures";
    lib.kind = LibKind::Imported;
    for (const auto& c : colors) {
        char hex[8];
        std::snprintf(hex, sizeof hex, "#%06X", c.rgb & 0xFFFFFF);
        lib.colors.push_back(rgb_swatch(hex, (c.rgb >> 16) & 0xFF, (c.rgb >> 8) & 0xFF, c.rgb & 0xFF));
    }
    return lib;
}

// ── s181 (C1b): a vector picture's colours ───────────────────────────────────
namespace {
struct Named { const char* name; std::uint32_t rgb; };
constexpr Named kCssNames[] = {
    {"aliceblue", 0xF0F8FF}, {"antiquewhite", 0xFAEBD7}, {"aqua", 0x00FFFF}, {"aquamarine", 0x7FFFD4},
    {"azure", 0xF0FFFF}, {"beige", 0xF5F5DC}, {"bisque", 0xFFE4C4}, {"black", 0x000000},
    {"blanchedalmond", 0xFFEBCD}, {"blue", 0x0000FF}, {"blueviolet", 0x8A2BE2}, {"brown", 0xA52A2A},
    {"burlywood", 0xDEB887}, {"cadetblue", 0x5F9EA0}, {"chartreuse", 0x7FFF00}, {"chocolate", 0xD2691E},
    {"coral", 0xFF7F50}, {"cornflowerblue", 0x6495ED}, {"cornsilk", 0xFFF8DC}, {"crimson", 0xDC143C},
    {"cyan", 0x00FFFF}, {"darkblue", 0x00008B}, {"darkcyan", 0x008B8B}, {"darkgoldenrod", 0xB8860B},
    {"darkgray", 0xA9A9A9}, {"darkgreen", 0x006400}, {"darkgrey", 0xA9A9A9}, {"darkkhaki", 0xBDB76B},
    {"darkmagenta", 0x8B008B}, {"darkolivegreen", 0x556B2F}, {"darkorange", 0xFF8C00}, {"darkorchid", 0x9932CC},
    {"darkred", 0x8B0000}, {"darksalmon", 0xE9967A}, {"darkseagreen", 0x8FBC8F}, {"darkslateblue", 0x483D8B},
    {"darkslategray", 0x2F4F4F}, {"darkslategrey", 0x2F4F4F}, {"darkturquoise", 0x00CED1}, {"darkviolet", 0x9400D3},
    {"deeppink", 0xFF1493}, {"deepskyblue", 0x00BFFF}, {"dimgray", 0x696969}, {"dimgrey", 0x696969},
    {"dodgerblue", 0x1E90FF}, {"firebrick", 0xB22222}, {"floralwhite", 0xFFFAF0}, {"forestgreen", 0x228B22},
    {"fuchsia", 0xFF00FF}, {"gainsboro", 0xDCDCDC}, {"ghostwhite", 0xF8F8FF}, {"gold", 0xFFD700},
    {"goldenrod", 0xDAA520}, {"gray", 0x808080}, {"green", 0x008000}, {"greenyellow", 0xADFF2F}, {"grey", 0x808080},
    {"honeydew", 0xF0FFF0}, {"hotpink", 0xFF69B4}, {"indianred", 0xCD5C5C}, {"indigo", 0x4B0082},
    {"ivory", 0xFFFFF0}, {"khaki", 0xF0E68C}, {"lavender", 0xE6E6FA}, {"lavenderblush", 0xFFF0F5},
    {"lawngreen", 0x7CFC00}, {"lemonchiffon", 0xFFFACD}, {"lightblue", 0xADD8E6}, {"lightcoral", 0xF08080},
    {"lightcyan", 0xE0FFFF}, {"lightgoldenrodyellow", 0xFAFAD2}, {"lightgray", 0xD3D3D3}, {"lightgreen", 0x90EE90},
    {"lightgrey", 0xD3D3D3}, {"lightpink", 0xFFB6C1}, {"lightsalmon", 0xFFA07A}, {"lightseagreen", 0x20B2AA},
    {"lightskyblue", 0x87CEFA}, {"lightslategray", 0x778899}, {"lightslategrey", 0x778899},
    {"lightsteelblue", 0xB0C4DE}, {"lightyellow", 0xFFFFE0}, {"lime", 0x00FF00}, {"limegreen", 0x32CD32},
    {"linen", 0xFAF0E6}, {"magenta", 0xFF00FF}, {"maroon", 0x800000}, {"mediumaquamarine", 0x66CDAA},
    {"mediumblue", 0x0000CD}, {"mediumorchid", 0xBA55D3}, {"mediumpurple", 0x9370DB}, {"mediumseagreen", 0x3CB371},
    {"mediumslateblue", 0x7B68EE}, {"mediumspringgreen", 0x00FA9A}, {"mediumturquoise", 0x48D1CC},
    {"mediumvioletred", 0xC71585}, {"midnightblue", 0x191970}, {"mintcream", 0xF5FFFA}, {"mistyrose", 0xFFE4E1},
    {"moccasin", 0xFFE4B5}, {"navajowhite", 0xFFDEAD}, {"navy", 0x000080}, {"oldlace", 0xFDF5E6},
    {"olive", 0x808000}, {"olivedrab", 0x6B8E23}, {"orange", 0xFFA500}, {"orangered", 0xFF4500},
    {"orchid", 0xDA70D6}, {"palegoldenrod", 0xEEE8AA}, {"palegreen", 0x98FB98}, {"paleturquoise", 0xAFEEEE},
    {"palevioletred", 0xDB7093}, {"papayawhip", 0xFFEFD5}, {"peachpuff", 0xFFDAB9}, {"peru", 0xCD853F},
    {"pink", 0xFFC0CB}, {"plum", 0xDDA0DD}, {"powderblue", 0xB0E0E6}, {"purple", 0x800080},
    {"rebeccapurple", 0x663399}, {"red", 0xFF0000}, {"rosybrown", 0xBC8F8F}, {"royalblue", 0x4169E1},
    {"saddlebrown", 0x8B4513}, {"salmon", 0xFA8072}, {"sandybrown", 0xF4A460}, {"seagreen", 0x2E8B57},
    {"seashell", 0xFFF5EE}, {"sienna", 0xA0522D}, {"silver", 0xC0C0C0}, {"skyblue", 0x87CEEB},
    {"slateblue", 0x6A5ACD}, {"slategray", 0x708090}, {"slategrey", 0x708090}, {"snow", 0xFFFAFA},
    {"springgreen", 0x00FF7F}, {"steelblue", 0x4682B4}, {"tan", 0xD2B48C}, {"teal", 0x008080}, {"thistle", 0xD8BFD8},
    {"tomato", 0xFF6347}, {"turquoise", 0x40E0D0}, {"violet", 0xEE82EE}, {"wheat", 0xF5DEB3}, {"white", 0xFFFFFF},
    {"whitesmoke", 0xF5F5F5}, {"yellow", 0xFFFF00}, {"yellowgreen", 0x9ACD32},
};   // CSS Color Module 4's named colours, sorted

std::string lower_trim(std::string t) {
    while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
    std::size_t a = 0;
    while (a < t.size() && std::isspace(static_cast<unsigned char>(t[a]))) ++a;
    t.erase(0, a);
    for (auto& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return t;
}
// The numbers in "a, b c / d" (commas, spaces or slashes between); a "%"
// after one marks it a percentage.
struct Num { double v; bool pct; };
std::vector<Num> numbers_in(const std::string& t) {
    std::vector<Num> out;
    const char* p = t.c_str();
    while (*p) {
        if (std::isdigit(static_cast<unsigned char>(*p)) || *p == '.' || *p == '-' || *p == '+') {
            char* end = nullptr;
            const double v = std::strtod(p, &end);
            if (end == p) { ++p; continue; }
            p = end;
            const bool pct = *p == '%';
            if (pct) ++p;
            out.push_back({v, pct});
        } else {
            ++p;
        }
    }
    return out;
}
std::uint32_t pack(double r, double g, double b) {   // 0..255
    const auto q = [](double x) { return static_cast<std::uint32_t>(std::lround(std::clamp(x, 0.0, 255.0))); };
    return (q(r) << 16) | (q(g) << 8) | q(b);
}
}  // namespace

std::optional<std::uint32_t> css_color(std::string t) {
    t = lower_trim(std::move(t));
    if (t.empty()) return std::nullopt;
    if (t[0] == '#') {
        std::string h = t.substr(1);
        for (char c : h) if (!std::isxdigit(static_cast<unsigned char>(c))) return std::nullopt;
        if (h.size() == 3 || h.size() == 4) h = {h[0], h[0], h[1], h[1], h[2], h[2]};
        else if (h.size() == 8) h.resize(6);
        if (h.size() != 6) return std::nullopt;
        return static_cast<std::uint32_t>(std::stoul(h, nullptr, 16));
    }
    const auto open = t.find('(');
    if (open != std::string::npos) {
        const std::string fn = t.substr(0, open);
        const auto close = t.find(')', open);
        if (close == std::string::npos) return std::nullopt;
        const auto n = numbers_in(t.substr(open + 1, close - open - 1));
        if (n.size() < 3) return std::nullopt;
        if (fn == "rgb" || fn == "rgba") {
            const auto ch = [](const Num& x) { return x.pct ? x.v * 255 / 100 : x.v; };
            return pack(ch(n[0]), ch(n[1]), ch(n[2]));
        }
        if (fn == "hsl" || fn == "hsla") {
            const double h = std::fmod(std::fmod(n[0].v, 360.0) + 360.0, 360.0) / 360.0;
            const double s = std::clamp(n[1].v / 100.0, 0.0, 1.0), l = std::clamp(n[2].v / 100.0, 0.0, 1.0);
            const double q = l < 0.5 ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
            const auto hue = [&](double x) {
                x = x < 0 ? x + 1 : x > 1 ? x - 1 : x;
                return x < 1.0 / 6 ? p + (q - p) * 6 * x : x < 0.5 ? q : x < 2.0 / 3 ? p + (q - p) * (2.0 / 3 - x) * 6 : p;
            };
            return pack(hue(h + 1.0 / 3) * 255, hue(h) * 255, hue(h - 1.0 / 3) * 255);
        }
        return std::nullopt;
    }
    const auto it = std::lower_bound(std::begin(kCssNames), std::end(kCssNames), t,
                                     [](const Named& a, const std::string& b) { return a.name < b; });
    if (it != std::end(kCssNames) && t == it->name) return it->rgb;
    return std::nullopt;   // none, currentcolor, transparent, inherit, url(...) all land here or above
}

std::vector<VectorColor> svg_colors(const std::string& svg, int most) {
    static const char* const kProps[] = {"fill", "stroke", "stop-color", "flood-color", "lighting-color", "solid-color", "color"};
    struct Seen { VectorColor c; std::size_t first; };
    std::vector<Seen> seen;
    const auto add = [&](const VectorColor& c, std::size_t at) {
        for (auto& s : seen)
            if (s.c.cmyk == c.cmyk && (c.cmyk ? s.c.v == c.v : s.c.rgb == c.rgb)) { ++s.c.uses; return; }
        seen.push_back({c, at});
        seen.back().c.uses = 1;
    };
    const auto name_char = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == ':'; };
    for (const char* prop : kProps) {
        const std::size_t n = std::strlen(prop);
        for (std::size_t at = svg.find(prop); at != std::string::npos; at = svg.find(prop, at + n)) {
            if (at > 0 && name_char(svg[at - 1])) continue;          // data-fill, stop-color's "color"
            std::size_t p = at + n;
            if (p < svg.size() && svg[p] != ':' && name_char(svg[p])) continue;   // fill-opacity, color-interpolation
            while (p < svg.size() && std::isspace(static_cast<unsigned char>(svg[p]))) ++p;
            if (p >= svg.size()) break;
            std::string value;
            if (svg[p] == '=') {   // an attribute: its quoted value
                ++p;
                while (p < svg.size() && std::isspace(static_cast<unsigned char>(svg[p]))) ++p;
                if (p >= svg.size() || (svg[p] != '"' && svg[p] != '\'')) continue;
                const char q = svg[p];
                const auto end = svg.find(q, p + 1);
                if (end == std::string::npos) continue;
                value = svg.substr(p + 1, end - p - 1);
            } else if (svg[p] == ':') {   // a declaration: up to ; } or the attribute's quote
                ++p;
                std::size_t end = p;
                int depth = 0;
                while (end < svg.size()) {
                    const char c = svg[end];
                    if (c == '(') ++depth;
                    else if (c == ')') --depth;
                    else if (depth <= 0 && (c == ';' || c == '}' || c == '"' || c == '\'' || c == '<' || c == '\n')) break;
                    ++end;
                }
                value = svg.substr(p, end - p);
            } else {
                continue;
            }
            // "!important", and an icc-color(...) after the fallback.
            if (const auto bang = value.find('!'); bang != std::string::npos) value.resize(bang);
            std::string fallback = value;
            VectorColor c;
            const std::string lv = lower_trim(value);
            if (const auto icc = lv.find("icc-color("); icc != std::string::npos) {
                fallback = value.substr(0, icc);
                const auto close = lv.find(')', icc);
                const auto comma = lv.find(',', icc);
                if (close != std::string::npos && comma != std::string::npos && comma < close) {
                    const auto nums = numbers_in(lv.substr(comma + 1, close - comma - 1));
                    if (nums.size() == 4) {
                        c.cmyk = true;
                        for (int k = 0; k < 4; ++k)
                            c.v[static_cast<std::size_t>(k)] = std::round(std::clamp(nums[static_cast<std::size_t>(k)].v, 0.0, 1.0) * 1000) / 10;
                    }
                }
            }
            const auto rgb = css_color(fallback);
            if (c.cmyk) {
                c.rgb = rgb ? *rgb : cmyk_swatch("", c.v[0], c.v[1], c.v[2], c.v[3]).preview;
            } else {
                if (!rgb) continue;
                c.rgb = *rgb;
            }
            add(c, at);
        }
    }
    std::stable_sort(seen.begin(), seen.end(), [](const Seen& a, const Seen& b) {
        return a.c.uses != b.c.uses ? a.c.uses > b.c.uses : a.first < b.first;
    });
    std::vector<VectorColor> out;
    for (const auto& s : seen) {
        if (static_cast<int>(out.size()) >= most) break;
        out.push_back(s.c);
    }
    return out;
}

Library vector_library(const std::string& name, const std::vector<VectorColor>& colors) {
    Library lib;
    lib.name = name;
    lib.source = "Pictures";
    lib.kind = LibKind::Imported;
    for (const auto& c : colors) {
        if (c.cmyk) {
            char nm[64];
            std::snprintf(nm, sizeof nm, "C%g M%g Y%g K%g", c.v[0], c.v[1], c.v[2], c.v[3]);
            lib.colors.push_back(cmyk_swatch(nm, c.v[0], c.v[1], c.v[2], c.v[3]));
        } else {
            char hex[8];
            std::snprintf(hex, sizeof hex, "#%06X", c.rgb & 0xFFFFFF);
            lib.colors.push_back(rgb_swatch(hex, (c.rgb >> 16) & 0xFF, (c.rgb >> 8) & 0xFF, c.rgb & 0xFF));
        }
    }
    return lib;
}

// ── s172: the shelf ───────────────────────────────────────────────────────────
std::string mine_file(const std::string& data_home) {
    return (fs::path(data_home) / "composix" / "swatches" / "my-library.json").string();
}

std::vector<std::string> imported(const AppSettings& s) {
    std::vector<std::string> out;
    const auto it = s.find("colors.imported");
    if (it == s.end()) return out;
    std::istringstream in(it->second);
    for (std::string line; std::getline(in, line);) {
        line = trim(line);
        if (!line.empty() && std::find(out.begin(), out.end(), line) == out.end()) out.push_back(line);
    }
    return out;
}

namespace {
void put_imported(AppSettings& s, const std::vector<std::string>& v) {
    std::string t;
    for (const auto& p : v) t += (t.empty() ? "" : "\n") + p;
    if (t.empty()) s.erase("colors.imported"); else s["colors.imported"] = t;
}
}  // namespace

bool remember_imported(AppSettings& s, const std::string& path) {
    auto v = imported(s);
    if (path.empty() || std::find(v.begin(), v.end(), path) != v.end()) return false;
    v.push_back(path);
    put_imported(s, v);
    return true;
}

bool forget_imported(AppSettings& s, const std::string& path) {
    auto v = imported(s);
    const auto n = std::erase(v, path);
    if (n) put_imported(s, v);
    return n > 0;
}

Format save_format(const std::string& path) {
    const auto ext = lower(fs::path(path).extension().string());
    return ext == ".gpl" ? Format::GPL : ext == ".json" ? Format::Composix : Format::ASE;
}

std::string with_extension(const std::string& path, Format f) {
    const auto ext = lower(fs::path(path).extension().string());
    if (ext == ".ase" || ext == ".gpl" || ext == ".json") return path;
    return path + (f == Format::GPL ? ".gpl" : f == Format::Composix ? ".json" : ".ase");
}

int remove_at(Library& lib, std::vector<std::size_t> at) {
    std::sort(at.begin(), at.end());
    at.erase(std::unique(at.begin(), at.end()), at.end());
    int n = 0;
    for (auto it = at.rbegin(); it != at.rend(); ++it)
        if (*it < lib.colors.size()) { lib.colors.erase(lib.colors.begin() + static_cast<std::ptrdiff_t>(*it)); ++n; }
    return n;
}

std::vector<Swatch> document_colors(const Doc& d) {
    std::vector<Swatch> out;
    for (const auto& id : d.ids_of<Swatch>()) out.push_back(*d.as<Swatch>(id));
    std::stable_sort(out.begin(), out.end(), [](const Swatch& a, const Swatch& b) { return a.pos < b.pos; });
    return out;
}

}  // namespace composix::core::swatches

namespace composix::core::doc {

std::vector<Doc::Id> add_library_swatches(DocJournal& j, Doc& d, const std::vector<Swatch>& colors, const std::string& library) {
    // Which are there already, which are new (and under what name).
    std::vector<Doc::Id> ids(colors.size());
    std::vector<std::pair<std::size_t, Swatch>> fresh;
    std::vector<std::string> names;
    for (const auto& id : d.ids_of<Swatch>()) names.push_back(d.as<Swatch>(id)->name);
    const auto taken = [&](const std::string& t) { return std::find(names.begin(), names.end(), t) != names.end(); };
    for (std::size_t i = 0; i < colors.size(); ++i) {
        Swatch s = colors[i];
        bool dup = false;
        for (const auto& id : d.ids_of<Swatch>()) {
            const auto* c = d.as<Swatch>(id);
            if (c->name == s.name && swatches::same_values(*c, s)) { ids[i] = id; dup = true; break; }
        }
        // The same colour asked twice in one go.
        for (const auto& [k, f] : fresh)
            if (!dup && colors[k].name == s.name && swatches::same_values(colors[k], s)) { dup = true; ids[i] = "#" + std::to_string(k); }
        if (dup) continue;
        if (taken(s.name)) s.name = swatches::numbered(s.name, taken);
        names.push_back(s.name);
        if (s.from.empty()) s.from = library;
        fresh.push_back({i, s});
    }
    if (fresh.empty()) return ids;
    double pos = 0;
    for (const auto& id : d.ids_of<Swatch>()) pos = std::max(pos, d.as<Swatch>(id)->pos);
    std::map<std::size_t, Doc::Id> made;
    const std::string label = fresh.size() == 1 ? "Add swatch " + fresh.front().second.name
                                                : "Add " + std::to_string(fresh.size()) + " swatches" + (library.empty() ? "" : " from " + library);
    j.run(label, {}, [&]() -> std::vector<Doc::Id> {
        std::vector<Doc::Id> out;
        for (auto& [i, s] : fresh) {
            s.pos = ++pos;
            const Doc::Id id = d.fresh_id("swatch");
            d.put(id, s);
            made[i] = id;
            out.push_back(id);
        }
        return out;
    });
    for (auto& id : ids)
        if (!id.empty() && id[0] == '#') id = made[static_cast<std::size_t>(std::stoul(id.substr(1)))];
    for (const auto& [i, id] : made) ids[i] = id;
    return ids;
}

}  // namespace composix::core::doc
