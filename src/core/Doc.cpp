#include "core/Doc.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <filesystem>
#include <set>

namespace composix::core {

// ─────────────────────────────────────────────────────────────────────────────
// The store
// ─────────────────────────────────────────────────────────────────────────────
std::optional<Record> Doc::get(const Id& id) const {
    const auto it = m_recs.find(id);
    if (it == m_recs.end()) return std::nullopt;
    return it->second;
}

void Doc::put(const Id& id, const std::optional<Record>& v) {
    // The gate: a write with the door shut, outside a load, is a stray.
    if (m_loading == 0 && m_door && !m_door()) ++m_strays;
    if (v) m_recs[id] = *v;
    else   m_recs.erase(id);
    ++m_rev;
    // Keep fresh ids ahead of anything that comes back (undo of a delete).
    const auto dot = id.rfind('.');
    if (dot != std::string::npos) {
        try {
            const int n = std::stoi(id.substr(dot + 1));
            int& next = m_next[id.substr(0, dot)];
            next = std::max(next, n + 1);
        } catch (...) {}
    }
    if (m_changed) m_changed();
}

void Doc::reset() {
    m_recs.clear();
    m_next.clear();
    ++m_rev;
    if (m_changed) m_changed();
}

const Settings& Doc::settings() const {
    static const Settings kDefault{};
    const auto* s = as<Settings>("doc");
    return s ? *s : kDefault;
}

Doc::Id Doc::fresh_id(const std::string& prefix) {
    int& n = m_next[prefix];
    if (n == 0) n = 1;
    Id id;
    do { id = prefix + "." + std::to_string(n++); } while (m_recs.count(id));
    return id;
}

namespace {
template <class T> double pos_of(const T& r) {
    if constexpr (requires { r.pos; }) return r.pos;
    else return 0;
}
}  // namespace

template <class T> std::vector<Doc::Id> Doc::ids_of() const {
    std::vector<Id> out;
    for (const auto& [id, r] : m_recs)
        if (std::holds_alternative<T>(r)) out.push_back(id);
    if constexpr (std::is_same_v<T, Section>) {
        std::stable_sort(out.begin(), out.end(), [this](const Id& a, const Id& b) {
            return page_index(as<Section>(a)->start_page) < page_index(as<Section>(b)->start_page);
        });
    } else {
        std::stable_sort(out.begin(), out.end(), [this](const Id& a, const Id& b) {
            return pos_of(*as<T>(a)) < pos_of(*as<T>(b));
        });
    }
    return out;
}

template <class T> double Doc::pos_after(const Id& after) const {
    const auto order = ids_of<T>();
    if (order.empty()) return 1.0;
    const auto it = std::find(order.begin(), order.end(), after);
    if (after.empty() || it == order.end()) return pos_of(*as<T>(order.back())) + 1.0;
    const double here = pos_of(*as<T>(*it));
    if (it + 1 == order.end()) return here + 1.0;
    return (here + pos_of(*as<T>(*(it + 1)))) / 2.0;
}

// Explicit instantiations for every kind (the header declares the templates).
template std::vector<Doc::Id> Doc::ids_of<Settings>() const;
template std::vector<Doc::Id> Doc::ids_of<Section>() const;
template std::vector<Doc::Id> Doc::ids_of<Page>() const;
template std::vector<Doc::Id> Doc::ids_of<Master>() const;
template std::vector<Doc::Id> Doc::ids_of<Frame>() const;
template std::vector<Doc::Id> Doc::ids_of<Story>() const;
template std::vector<Doc::Id> Doc::ids_of<Style>() const;
template std::vector<Doc::Id> Doc::ids_of<Swatch>() const;
template std::vector<Doc::Id> Doc::ids_of<Asset>() const;
template std::vector<Doc::Id> Doc::ids_of<GenList>() const;
template std::vector<Doc::Id> Doc::ids_of<Guide>() const;
template double Doc::pos_after<Page>(const Id&) const;
template double Doc::pos_after<Master>(const Id&) const;
template double Doc::pos_after<Style>(const Id&) const;
template double Doc::pos_after<Swatch>(const Id&) const;
template double Doc::pos_after<Asset>(const Id&) const;
template double Doc::pos_after<GenList>(const Id&) const;

int Doc::page_index(const Id& page) const {
    if (!as<Page>(page)) return -1;
    const double p = as<Page>(page)->pos;
    int n = 0;
    for (const auto& [id, r] : m_recs)
        if (const auto* pg = std::get_if<Page>(&r))
            if (pg->pos < p || (pg->pos == p && id < page)) ++n;
    return n;
}

Doc::Id Doc::section_of(const Id& page) const {
    const int at = page_index(page);
    if (at < 0) return {};
    Id best;
    int best_at = -1;
    for (const auto& sid : ids_of<Section>()) {
        const int s = page_index(as<Section>(sid)->start_page);
        if (s >= 0 && s <= at && s > best_at) { best = sid; best_at = s; }
    }
    return best;
}

std::string Doc::folio(const Id& page) const {
    const int at = page_index(page);
    if (at < 0) return {};
    const Id sec = section_of(page);
    if (sec.empty()) return format_folio(at + 1, Numbering::Arabic);
    const auto* s = as<Section>(sec);
    return format_folio(s->start_at + (at - page_index(s->start_page)), s->numbering);
}

Side Doc::side_of(const Id& page) const {
    if (!settings().facing) return Side::Any;
    const int at = page_index(page);
    return at % 2 == 0 ? Side::Right : Side::Left;   // the first page is a right-hand page
}

std::vector<Doc::Id> Doc::pages_of(const Id& section) const {
    std::vector<Id> out;
    for (const auto& p : pages())
        if (section_of(p) == section) out.push_back(p);
    return out;
}

std::vector<Doc::Id> Doc::frames_on(const Id& owner) const {
    std::vector<Id> out;
    for (const auto& id : ids_of<Frame>())
        if (as<Frame>(id)->owner == owner) out.push_back(id);
    std::stable_sort(out.begin(), out.end(),
                     [this](const Id& a, const Id& b) { return as<Frame>(a)->z < as<Frame>(b)->z; });
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// Display helpers
// ─────────────────────────────────────────────────────────────────────────────
std::string format_folio(int n, Numbering how) {
    if (n <= 0) return std::to_string(n);
    switch (how) {
    case Numbering::Arabic: return std::to_string(n);
    case Numbering::RomanLower:
    case Numbering::RomanUpper: {
        static const std::pair<int, const char*> k[] = {
            {1000, "m"}, {900, "cm"}, {500, "d"}, {400, "cd"}, {100, "c"}, {90, "xc"},
            {50, "l"},   {40, "xl"},  {10, "x"},  {9, "ix"},   {5, "v"},   {4, "iv"}, {1, "i"}};
        std::string out;
        for (const auto& [v, s] : k)
            while (n >= v) { out += s; n -= v; }
        if (how == Numbering::RomanUpper)
            for (auto& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return out;
    }
    case Numbering::AlphaLower:
    case Numbering::AlphaUpper: {   // a..z, aa..zz (as InDesign)
        const char base = how == Numbering::AlphaLower ? 'a' : 'A';
        const int reps = (n - 1) / 26 + 1;
        return std::string(static_cast<std::size_t>(reps), static_cast<char>(base + (n - 1) % 26));
    }
    }
    return std::to_string(n);
}

std::optional<double> parse_length(std::string_view text, double unit) {
    std::string t;
    for (char c : text) if (!std::isspace(static_cast<unsigned char>(c))) t += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (t.empty()) return std::nullopt;
    std::size_t i = 0;
    const auto number = [&](double& out) {
        const std::size_t a = i;
        if (i < t.size() && (t[i] == '-' || t[i] == '+')) ++i;
        bool digits = false;
        while (i < t.size() && std::isdigit(static_cast<unsigned char>(t[i]))) { ++i; digits = true; }
        if (i < t.size() && t[i] == '.') {
            ++i;
            while (i < t.size() && std::isdigit(static_cast<unsigned char>(t[i]))) { ++i; digits = true; }
        }
        if (!digits) { i = a; return false; }
        out = std::stod(t.substr(a, i - a));
        return true;
    };
    double v = 0;
    const bool lead = number(v);
    std::string rest = t.substr(i);
    // Picas: "3p6", "3p", "p6".
    if (!rest.empty() && rest[0] == 'p' && rest.rfind("pt", 0) != 0 && rest.rfind("pc", 0) != 0 && rest.rfind("pica", 0) != 0) {
        ++i;
        double pts = 0;
        const bool tail = number(pts);
        if (i != t.size() || (!lead && !tail)) return std::nullopt;
        if (lead && v < 0) pts = -pts;
        return (lead ? v : 0) * 12 + pts;
    }
    if (!lead) return std::nullopt;
    static const std::pair<const char*, double> units[] = {
        {"", -1}, {"pt", 1}, {"pts", 1}, {"point", 1}, {"points", 1}, {"in", 72}, {"inch", 72}, {"inches", 72},
        {"\"", 72}, {"mm", 72 / 25.4}, {"cm", 72 / 2.54}, {"pc", 12}, {"pica", 12}, {"picas", 12}};
    for (const auto& [name, k] : units)
        if (rest == name) return v * (k < 0 ? unit : k);
    return std::nullopt;
}

std::string num_text(double v) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.3f", std::round(v * 1000) / 1000);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s == "-0" ? "0" : s;
}

std::string inches(double pt) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.3f", pt / kPt);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

std::string size_text(double w, double h, std::size_t folds) {
    std::string s = inches(w) + " × " + inches(h) + " in";
    if (folds > 0) s += " (" + std::to_string(folds + 1) + "-panel)";
    return s;
}

std::string numbering_text(Numbering n) {
    switch (n) {
    case Numbering::Arabic:     return "Arabic (1, 2, 3)";
    case Numbering::RomanLower: return "Roman (i, ii, iii)";
    case Numbering::RomanUpper: return "Roman (I, II, III)";
    case Numbering::AlphaLower: return "Letters (a, b, c)";
    case Numbering::AlphaUpper: return "Letters (A, B, C)";
    }
    return "?";
}

std::string side_text(Side s) {
    switch (s) {
    case Side::Any:   return "Any";
    case Side::Right: return "Right";
    case Side::Left:  return "Left";
    }
    return "?";
}

std::string master_label(const Doc& d, const Doc::Id& master) {
    const auto* m = d.as<Master>(master);
    if (!m) return "None";
    return m->prefix + "-" + m->name;
}

// ─────────────────────────────────────────────────────────────────────────────
// The verbs
// ─────────────────────────────────────────────────────────────────────────────
// ── columns (s012) ──────────────────────────────────────────────────────────
int column_count(const Frame& f) { return std::clamp(f.columns, 1, 24); }
std::vector<double> column_weights(const Frame& f, int n) {
    std::vector<double> w(static_cast<std::size_t>(n), 1.0);
    if (static_cast<int>(f.widths.size()) == n &&
        std::all_of(f.widths.begin(), f.widths.end(), [](double v) { return v > 0; }))
        w = f.widths;
    return w;
}
double column_width(const Frame& f, int col) {
    const int n = column_count(f);
    const double g = n > 1 ? std::max(0.0, f.gutter) : 0;
    const auto w = column_weights(f, n);
    double sum = 0;
    for (double v : w) sum += v;
    // s040: inside the frame's left / right insets.
    const double room = std::max(1.0 * n, f.w - std::max(0.0, f.inset.left) - std::max(0.0, f.inset.right) - g * (n - 1));
    return std::max(1.0, room * w[static_cast<std::size_t>(std::clamp(col, 0, n - 1))] / sum);
}
double column_x(const Frame& f, int col) {
    const int n = column_count(f);
    const double g = n > 1 ? std::max(0.0, f.gutter) : 0;
    double x = std::max(0.0, f.inset.left);   // s040
    for (int c = 0; c < std::clamp(col, 0, n - 1); ++c) x += column_width(f, c) + g;
    return x;
}
int column_at(const Frame& f, double x) {
    const int n = column_count(f);
    int best = 0;
    double dist = 1e300;
    for (int c = 0; c < n; ++c) {
        const double a = column_x(f, c), b = a + column_width(f, c);
        const double dd = x < a ? a - x : x > b ? x - b : 0;
        if (dd < dist) { dist = dd; best = c; }
    }
    return best;
}

// ── text positions (s010) ───────────────────────────────────────────────────
namespace {
bool cont(unsigned char c) { return (c & 0xC0) == 0x80; }   // a UTF-8 continuation byte
// A "word" character: letters, digits, anything non-ASCII; not space / punctuation.
bool wordy(unsigned char c) { return c >= 0x80 || std::isalnum(c) || c == '_' || c == '\''; }
}  // namespace

TextPos clamp_pos(const Story& s, TextPos p) {
    if (s.paras.empty()) return {0, 0};
    p.para = std::clamp(p.para, 0, static_cast<int>(s.paras.size()) - 1);
    const auto& t = s.paras[static_cast<std::size_t>(p.para)].text;
    p.byte = std::clamp(p.byte, 0, static_cast<int>(t.size()));
    while (p.byte > 0 && p.byte < static_cast<int>(t.size()) && cont(static_cast<unsigned char>(t[static_cast<std::size_t>(p.byte)])))
        --p.byte;
    return p;
}

TextPos story_end(const Story& s) {
    if (s.paras.empty()) return {0, 0};
    return {static_cast<int>(s.paras.size()) - 1, static_cast<int>(s.paras.back().text.size())};
}

TextPos next_char(const Story& s, TextPos p) {
    p = clamp_pos(s, p);
    if (s.paras.empty()) return p;
    const auto& t = s.paras[static_cast<std::size_t>(p.para)].text;
    if (p.byte >= static_cast<int>(t.size())) {
        if (p.para + 1 < static_cast<int>(s.paras.size())) return {p.para + 1, 0};
        return p;
    }
    ++p.byte;
    while (p.byte < static_cast<int>(t.size()) && cont(static_cast<unsigned char>(t[static_cast<std::size_t>(p.byte)]))) ++p.byte;
    return p;
}

TextPos prev_char(const Story& s, TextPos p) {
    p = clamp_pos(s, p);
    if (s.paras.empty()) return p;
    if (p.byte == 0) {
        if (p.para > 0) return {p.para - 1, static_cast<int>(s.paras[static_cast<std::size_t>(p.para - 1)].text.size())};
        return p;
    }
    const auto& t = s.paras[static_cast<std::size_t>(p.para)].text;
    --p.byte;
    while (p.byte > 0 && cont(static_cast<unsigned char>(t[static_cast<std::size_t>(p.byte)]))) --p.byte;
    return p;
}

namespace {
unsigned char byte_at(const Story& s, TextPos p) {   // '\n' at a paragraph's end
    const auto& t = s.paras[static_cast<std::size_t>(p.para)].text;
    return p.byte < static_cast<int>(t.size()) ? static_cast<unsigned char>(t[static_cast<std::size_t>(p.byte)]) : '\n';
}
}  // namespace

TextPos next_word(const Story& s, TextPos p) {
    p = clamp_pos(s, p);
    if (s.paras.empty()) return p;
    const TextPos end = story_end(s);
    while (p < end && wordy(byte_at(s, p))) p = next_char(s, p);    // the rest of this word
    while (p < end && !wordy(byte_at(s, p))) p = next_char(s, p);   // the gap after it
    return p;
}

TextPos prev_word(const Story& s, TextPos p) {
    p = clamp_pos(s, p);
    if (s.paras.empty()) return p;
    const auto before = [&](TextPos q) { return byte_at(s, prev_char(s, q)); };
    while (p > TextPos{} && !wordy(before(p))) p = prev_char(s, p);
    while (p > TextPos{} && wordy(before(p))) p = prev_char(s, p);
    return p;
}

std::pair<TextPos, TextPos> word_at(const Story& s, TextPos p) {
    p = clamp_pos(s, p);
    if (s.paras.empty()) return {p, p};
    const auto& t = s.paras[static_cast<std::size_t>(p.para)].text;
    int a = p.byte, b = p.byte;
    const auto w = [&](int i) { return wordy(static_cast<unsigned char>(t[static_cast<std::size_t>(i)])); };
    if (b < static_cast<int>(t.size()) && w(b)) {
        while (a > 0 && w(a - 1)) --a;
        while (b < static_cast<int>(t.size()) && w(b)) ++b;
    } else if (a > 0 && w(a - 1)) {
        while (a > 0 && w(a - 1)) --a;
    } else if (b < static_cast<int>(t.size())) {   // on a space / punctuation: just that character
        b = next_char(s, p).byte;
    }
    return {{p.para, a}, {p.para, b}};
}

std::string text_between(const Story& s, TextPos a, TextPos b) {
    if (s.paras.empty()) return {};
    a = clamp_pos(s, a);
    b = clamp_pos(s, b);
    if (b < a) std::swap(a, b);
    std::string out;
    for (int p = a.para; p <= b.para; ++p) {
        const auto& t = s.paras[static_cast<std::size_t>(p)].text;
        const int from = p == a.para ? a.byte : 0;
        const int to = p == b.para ? b.byte : static_cast<int>(t.size());
        out += t.substr(static_cast<std::size_t>(from), static_cast<std::size_t>(to - from));
        if (p < b.para) out += '\n';
    }
    return out;
}

TextPos changed_end(const Story& before, const Story& after) {
    const auto flat = [](const Story& s) {
        std::string t;
        for (std::size_t i = 0; i < s.paras.size(); ++i) { if (i) t += '\n'; t += s.paras[i].text; }
        return t;
    };
    const std::string a = flat(before), b = flat(after);
    std::size_t pre = 0;
    while (pre < a.size() && pre < b.size() && a[pre] == b[pre]) ++pre;
    std::size_t suf = 0;
    while (suf < a.size() - pre && suf < b.size() - pre && a[a.size() - 1 - suf] == b[b.size() - 1 - suf]) ++suf;
    std::size_t at = b.size() - suf;   // the end of what differs, in `after`
    if (a == b) at = 0;
    while (at > 0 && at < b.size() && (static_cast<unsigned char>(b[at]) & 0xC0) == 0x80) --at;   // not inside a character
    TextPos p{0, 0};
    for (std::size_t i = 0; i < at; ++i) {
        if (b[i] == '\n') { ++p.para; p.byte = 0; }
        else ++p.byte;
    }
    return p;
}

std::string char_style_at(const Para& p, int byte) {
    const int at = byte > 0 ? byte - 1 : 0;
    for (const auto& r : p.runs)
        if (at >= r.start && at < r.end) return r.style;
    return {};
}

Local local_at(const Para& p, int byte) {
    const int at = byte > 0 ? byte - 1 : 0;
    for (const auto& r : p.runs)
        if (at >= r.start && at < r.end) return r.local;
    return {};
}

namespace {
template <class F> bool all_chars(const Doc& d, const Story& s, TextPos a, TextPos b, F&& on) {
    if (b < a) std::swap(a, b);
    bool any = false;
    for (TextPos q = a; q < b; q = next_char(s, q)) {
        const auto& para = s.paras[static_cast<std::size_t>(q.para)];
        if (q.byte >= static_cast<int>(para.text.size())) continue;
        any = true;
        const Local l = local_at(para, q.byte + 1);
        const auto* cs = d.as<Style>(char_style_at(para, q.byte + 1));
        const auto* ps = d.as<Style>(para.style);
        if (!on(l, cs, ps)) return false;
    }
    return any;
}
}  // namespace

std::vector<WordLook> word_looks(const Doc& d, const Para& p) {
    std::vector<WordLook> out;
    for (const auto& r : p.runs) {
        const auto* cs = d.as<Style>(r.style);
        WordLook w{r.start, r.end, false, false, cs ? r.style : std::string{}};
        w.bold = r.local.bold >= 0 ? r.local.bold == 1 : (cs && cs->bold);
        w.italic = r.local.italic >= 0 ? r.local.italic == 1 : (cs && cs->italic);
        if (w.bold || w.italic || !w.cstyle.empty()) out.push_back(w);
    }
    return out;
}

bool all_bold(const Doc& d, const Story& s, TextPos a, TextPos b) {
    return all_chars(d, s, a, b, [](const Local& l, const Style* cs, const Style* ps) {
        if (l.bold >= 0) return l.bold == 1;
        if (cs && cs->bold) return true;
        return ps && ps->bold;
    });
}

bool all_italic(const Doc& d, const Story& s, TextPos a, TextPos b) {
    return all_chars(d, s, a, b, [](const Local& l, const Style* cs, const Style* ps) {
        if (l.italic >= 0) return l.italic == 1;
        if (cs && cs->italic) return true;
        return ps && ps->italic;
    });
}

namespace {
// Text with its character style, piece by piece (s011): how edits keep runs.
// s030: and its local formatting.
struct Span { std::string text, style; Local local = {}; };
std::vector<Span> spans_of(const Para& p, int from, int to) {
    std::vector<Span> out;
    int at = from;
    for (const auto& r : p.runs) {
        if (r.end <= from || r.start >= to) continue;
        const int a = std::max(r.start, from), b = std::min(r.end, to);
        if (a > at) out.push_back({p.text.substr(static_cast<std::size_t>(at), static_cast<std::size_t>(a - at)), {}});
        out.push_back({p.text.substr(static_cast<std::size_t>(a), static_cast<std::size_t>(b - a)), r.style, r.local});
        at = b;
    }
    if (to > at) out.push_back({p.text.substr(static_cast<std::size_t>(at), static_cast<std::size_t>(to - at)), {}});
    return out;
}
Para para_of(const std::string& style, const std::vector<Span>& spans) {
    Para p{style, {}, {}};
    for (const auto& sp : spans) {
        if (sp.text.empty()) continue;
        const int a = static_cast<int>(p.text.size());
        p.text += sp.text;
        const int b = static_cast<int>(p.text.size());
        if (sp.style.empty() && sp.local.empty()) continue;
        if (!p.runs.empty() && p.runs.back().end == a && p.runs.back().style == sp.style && p.runs.back().local == sp.local)
            p.runs.back().end = b;
        else p.runs.push_back({a, b, sp.style, sp.local});
    }
    return p;
}
}  // namespace

// ── images (s009) ───────────────────────────────────────────────────────────
std::string resolve_asset(const Asset& a, const std::string& dir) {
    namespace fs = std::filesystem;
    if (a.path.empty()) return {};
    const fs::path p(a.path);
    if (p.is_absolute()) return p.string();
    if (dir.empty()) return {};
    return (fs::path(dir) / p).string();
}

std::int64_t file_stamp(const std::string& path) {
    namespace fs = std::filesystem;
    if (path.empty()) return 0;
    std::error_code ec;
    const auto t = fs::last_write_time(path, ec);
    if (ec) return 0;
    const auto n = std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
    return n == 0 ? 1 : static_cast<std::int64_t>(n);
}

LinkStatus link_status(const Asset& a, const std::string& dir) {
    const std::string at = resolve_asset(a, dir);
    const std::int64_t now = file_stamp(at);
    if (a.embedded) {
        // Unsaved, an embedded file has no folder to be in yet: say embedded.
        if (dir.empty() && !std::filesystem::path(a.path).is_absolute()) return LinkStatus::Embedded;
        return now ? LinkStatus::Embedded : LinkStatus::EmbeddedMissing;
    }
    if (!now) return LinkStatus::Missing;
    if (a.stamp && now != a.stamp) return LinkStatus::Modified;
    return LinkStatus::OK;
}

std::string link_status_text(LinkStatus s) {
    switch (s) {
    case LinkStatus::OK:              return "OK";
    case LinkStatus::Modified:        return "Modified";
    case LinkStatus::Missing:         return "Missing";
    case LinkStatus::Embedded:        return "Embedded";
    case LinkStatus::EmbeddedMissing: return "Missing from project";
    }
    return {};
}

Rect fit_rect(const Rect& fr, double iw, double ih, ImageFit fit) {
    if (iw <= 0 || ih <= 0 || fr.w <= 0 || fr.h <= 0) return fr;
    const double sx = fr.w / iw, sy = fr.h / ih;
    const double s = fit == ImageFit::Fill ? std::max(sx, sy) : std::min(sx, sy);
    const double w = iw * s, h = ih * s;
    return {fr.x + (fr.w - w) / 2, fr.y + (fr.h - h) / 2, w, h};
}

Rect picture_rect(const Rect& fr, const Frame& f, double iw, double ih, double s) {
    if (f.fit != ImageFit::Custom || f.pic_w <= 0 || iw <= 0 || ih <= 0) return fit_rect(fr, iw, ih, f.fit == ImageFit::Fit ? ImageFit::Fit : ImageFit::Fill);
    return {fr.x + f.pic_x * s, fr.y + f.pic_y * s, f.pic_w * s, f.pic_w * ih / iw * s};
}

Size2 picture_natural(const Asset& a) {
    if (a.px_w <= 0 || a.px_h <= 0) return {2 * kPt, 2 * kPt};
    const double per = kPt / 300.0;
    return {a.px_w * per, a.px_h * per};
}

double picture_scale(const Frame& f, const Asset& a) {
    const double iw = a.px_w > 0 ? a.px_w : 1, ih = a.px_h > 0 ? a.px_h : 1;
    const Rect r = picture_rect({0, 0, f.w, f.h}, f, iw, ih);
    const auto nat = picture_natural(a);
    return nat.w > 0 ? r.w / nat.w * 100 : 0;
}

double effective_ppi(const Frame& f, const Asset& a) {
    if (a.px_w <= 0 || a.px_h <= 0) return 0;
    const Rect r = picture_rect({0, 0, f.w, f.h}, f, a.px_w, a.px_h);
    if (r.w <= 0) return 0;
    return a.px_w / (r.w / kPt);
}

Size2 natural_size(const ImageFile& f, double max_w, double max_h) {
    double w = 0, h = 0;
    if (f.px_w > 0 && f.px_h > 0) {
        const double per = f.vector ? 1.0 : kPt / 300.0;   // a vector's size is in points
        w = f.px_w * per;
        h = f.px_h * per;
    } else {
        w = h = 2 * kPt;
    }
    const double s = std::min({1.0, max_w > 0 ? max_w / w : 1.0, max_h > 0 ? max_h / h : 1.0});
    return {w * s, h * s};
}

namespace doc {

namespace {
std::string plural(std::size_t n, const char* one, const char* many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

// Only the ids that are pages, in page order, without repeats.
std::vector<Doc::Id> only_pages(const Doc& d, const std::vector<Doc::Id>& ids) {
    std::vector<Doc::Id> out;
    for (const auto& p : d.pages())
        if (std::find(ids.begin(), ids.end(), p) != ids.end()) out.push_back(p);
    return out;
}

std::vector<Doc::Id> only_sections(const Doc& d, const std::vector<Doc::Id>& ids) {
    std::vector<Doc::Id> out;
    for (const auto& id : ids)
        if (d.as<Section>(id) && std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
    return out;
}

// Apply `f` to each page record; a step only if something changes.
template <class F>
bool edit_pages(DocJournal& j, Doc& d, const std::vector<Doc::Id>& ids, const std::string& label, F f) {
    const auto pages = only_pages(d, ids);
    if (pages.empty()) return false;
    return j.run(label, pages, [&] {
        for (const auto& p : pages) {
            Page pg = *d.as<Page>(p);
            f(pg);
            d.put(p, pg);
        }
    });
}

template <class F>
bool edit_sections(DocJournal& j, Doc& d, const std::vector<Doc::Id>& ids, const std::string& label, F f) {
    const auto secs = only_sections(d, ids);
    if (secs.empty()) return false;
    return j.run(label, secs, [&] {
        for (const auto& s : secs) {
            Section sec = *d.as<Section>(s);
            f(sec);
            d.put(s, sec);
        }
    });
}
}  // namespace

std::vector<SizePreset> size_presets(const Settings& s) {
    const double tuck = 0.125 * kPt;   // a fold-in panel is narrower so it clears the spine
    return {
        {"Trim (" + size_text(s.trim_w, s.trim_h, 0) + ")", s.trim_w, s.trim_h, {}},
        {"2-panel foldout (" + size_text(2 * s.trim_w - tuck, s.trim_h, 1) + ")",
         2 * s.trim_w - tuck, s.trim_h, {s.trim_w}},
        {"3-panel foldout (" + size_text(3 * s.trim_w - 2 * tuck, s.trim_h, 2) + ")",
         3 * s.trim_w - 2 * tuck, s.trim_h, {s.trim_w, 2 * s.trim_w}},
    };
}

std::vector<Doc::Id> add_pages(DocJournal& j, Doc& d, const Doc::Id& after, int count,
                               const Doc::Id& master) {
    std::vector<Doc::Id> made;
    if (count <= 0) return made;
    j.run("Add " + plural(static_cast<std::size_t>(count), "page", "pages"), {}, [&] {
        const auto& s = d.settings();
        Doc::Id prev = after.empty() && !d.pages().empty() ? d.pages().back() : after;
        for (int i = 0; i < count; ++i) {
            const Doc::Id id = d.fresh_id("page");
            Page pg;
            pg.pos = d.pos_after<Page>(prev);
            pg.w = s.trim_w;
            pg.h = s.trim_h;
            pg.master = master;
            d.put(id, pg);
            made.push_back(id);
            prev = id;
        }
        return made;
    });
    return made;
}

bool remove_pages(DocJournal& j, Doc& d, const std::vector<Doc::Id>& ids) {
    const auto pages = only_pages(d, ids);
    if (pages.empty() || pages.size() >= d.pages().size()) return false;   // a document keeps one page
    // Everything the removal touches is in the step: the pages, their frames,
    // and any section starting on one of them (it moves to the next survivor,
    // or goes if its whole run goes).
    std::vector<Doc::Id> touched = pages;
    for (const auto& p : pages)
        for (const auto& f : d.frames_on(p)) touched.push_back(f);
    const auto secs = d.ids_of<Section>();
    for (const auto& s : secs) touched.push_back(s);
    return j.run("Delete " + plural(pages.size(), "page", "pages"), touched, [&] {
        const auto order = d.pages();
        const auto gone = [&](const Doc::Id& p) {
            return std::find(pages.begin(), pages.end(), p) != pages.end();
        };
        for (const auto& sid : secs) {
            Section s = *d.as<Section>(sid);
            if (!gone(s.start_page)) continue;
            auto it = std::find(order.begin(), order.end(), s.start_page);
            Doc::Id next;
            for (++it; it != order.end(); ++it) {
                if (gone(*it)) continue;
                // A survivor still in this section's run (before the next section starts)?
                bool other = false;
                for (const auto& o : secs)
                    if (o != sid && d.as<Section>(o)->start_page == *it) other = true;
                if (!other) next = *it;
                break;
            }
            if (next.empty()) d.put(sid, std::nullopt);
            else { s.start_page = next; d.put(sid, s); }
        }
        for (const auto& p : pages) {
            for (const auto& f : d.frames_on(p)) d.put(f, std::nullopt);
            d.put(p, std::nullopt);
        }
    });
}

bool move_pages(DocJournal& j, Doc& d, const std::vector<Doc::Id>& ids, const Doc::Id& after) {
    const auto pages = only_pages(d, ids);
    if (pages.empty() || std::find(pages.begin(), pages.end(), after) != pages.end()) return false;
    return j.run("Move " + plural(pages.size(), "page", "pages"), pages, [&] {
        // Lay them out, in order, into the gap after `after` (or at the front).
        const auto rest = [&] {
            std::vector<Doc::Id> r;
            for (const auto& p : d.pages())
                if (std::find(pages.begin(), pages.end(), p) == pages.end()) r.push_back(p);
            return r;
        }();
        double lo = 0, hi = 0;
        if (after.empty()) {
            hi = rest.empty() ? 1.0 : d.as<Page>(rest.front())->pos;
            lo = hi - 1.0;
        } else {
            const auto it = std::find(rest.begin(), rest.end(), after);
            lo = d.as<Page>(after)->pos;
            hi = (it + 1 == rest.end()) ? lo + 1.0 : d.as<Page>(*(it + 1))->pos;
        }
        const double step = (hi - lo) / static_cast<double>(pages.size() + 1);
        double at = lo;
        for (const auto& p : pages) {
            Page pg = *d.as<Page>(p);
            at += step;
            pg.pos = at;
            d.put(p, pg);
        }
    });
}

bool set_master(DocJournal& j, Doc& d, const std::vector<Doc::Id>& ids, const Doc::Id& master) {
    if (!master.empty() && !d.as<Master>(master)) return false;
    return edit_pages(j, d, ids, "Apply master " + master_label(d, master),
                      [&](Page& p) { p.master = master; });
}

bool set_page_size(DocJournal& j, Doc& d, const std::vector<Doc::Id>& ids, const SizePreset& sz) {
    return edit_pages(j, d, ids, "Page size " + size_text(sz.w, sz.h, sz.folds.size()), [&](Page& p) {
        p.w = sz.w;
        p.h = sz.h;
        p.folds = sz.folds;
    });
}

Doc::Id add_section(DocJournal& j, Doc& d, const Doc::Id& start_page, const std::string& name,
                    Numbering n, int start_at) {
    if (!d.as<Page>(start_page)) return {};
    for (const auto& s : d.ids_of<Section>())
        if (d.as<Section>(s)->start_page == start_page) return {};   // one section per start
    Doc::Id made;
    j.run("New section “" + name + "”", {}, [&] {
        made = d.fresh_id("section");
        Section s;
        s.name = name;
        s.start_page = start_page;
        s.numbering = n;
        s.start_at = start_at;
        s.master = d.as<Page>(start_page)->master;
        d.put(made, s);
        return std::vector<Doc::Id>{made};
    });
    return made;
}

bool set_numbering(DocJournal& j, Doc& d, const std::vector<Doc::Id>& ids, Numbering n) {
    return edit_sections(j, d, ids, "Numbering " + numbering_text(n), [&](Section& s) { s.numbering = n; });
}

bool set_starts(DocJournal& j, Doc& d, const std::vector<Doc::Id>& ids, Side side) {
    return edit_sections(j, d, ids, "Section starts " + side_text(side), [&](Section& s) { s.starts = side; });
}

bool remove_section(DocJournal& j, Doc& d, const Doc::Id& section) {
    const auto* s = d.as<Section>(section);
    if (!s) return false;
    return j.run("Delete section “" + s->name + "”", {section},
                 [&] { d.put(section, std::nullopt); });
}

bool rename_section(DocJournal& j, Doc& d, const Doc::Id& section, const std::string& name) {
    std::string n = name;
    while (!n.empty() && (n.back() == ' ' || n.back() == '\t')) n.pop_back();
    while (!n.empty() && (n.front() == ' ' || n.front() == '\t')) n.erase(n.begin());
    if (n.empty() || !d.as<Section>(section)) return false;
    return edit_sections(j, d, {section}, "Rename section \u201c" + n + "\u201d", [&](Section& s) { s.name = n; });
}

bool set_section_start_at(DocJournal& j, Doc& d, const std::vector<Doc::Id>& ids, int start_at) {
    if (start_at < 1) return false;
    return edit_sections(j, d, ids, "Start numbering at " + std::to_string(start_at), [&](Section& s) { s.start_at = start_at; });
}

bool set_section_master(DocJournal& j, Doc& d, const std::vector<Doc::Id>& ids, const Doc::Id& master) {
    if (!master.empty() && !d.as<Master>(master)) return false;
    return edit_sections(j, d, ids, "Section master", [&](Section& s) { s.master = master; });
}

Doc::Id neighbour_page(const Doc& d, const std::vector<Doc::Id>& gone) {
    const auto order = d.pages();
    const auto is_gone = [&](const Doc::Id& p) { return std::find(gone.begin(), gone.end(), p) != gone.end(); };
    int last = -1;
    for (int i = 0; i < static_cast<int>(order.size()); ++i)
        if (is_gone(order[static_cast<std::size_t>(i)])) last = i;
    if (last < 0) return {};
    for (int i = last + 1; i < static_cast<int>(order.size()); ++i)
        if (!is_gone(order[static_cast<std::size_t>(i)])) return order[static_cast<std::size_t>(i)];
    for (int i = last - 1; i >= 0; --i)
        if (!is_gone(order[static_cast<std::size_t>(i)])) return order[static_cast<std::size_t>(i)];
    return {};
}

bool move_frames(DocJournal& j, Doc& d, const std::vector<Doc::Id>& ids, double dx, double dy,
                 const std::string& merge_key) {
    std::vector<Doc::Id> frames;
    for (const auto& id : ids) if (d.as<Frame>(id)) frames.push_back(id);
    if (frames.empty() || (dx == 0 && dy == 0)) return false;
    return j.run("Move " + plural(frames.size(), "frame", "frames"), frames, [&] {
        for (const auto& id : frames) {
            Frame f = *d.as<Frame>(id);
            f.x += dx;
            f.y += dy;
            d.put(id, f);
        }
    }, merge_key);
}

void frames_begin(DocJournal& j, Doc& d, const std::vector<Doc::Id>& ids, const std::string& label) {
    std::vector<Doc::Id> frames;
    for (const auto& id : ids) if (d.as<Frame>(id)) frames.push_back(id);
    if (!frames.empty()) j.begin(label, frames);
}

void frames_set(DocJournal& j, Doc& d, const Doc::Id& id, const Rect& r) {
    const auto* f0 = d.as<Frame>(id);
    if (!f0) return;
    Frame f = *f0;
    if (f.x == r.x && f.y == r.y && f.w == r.w && f.h == r.h) return;
    // Through the door even mid-drag: inside the open gesture this just writes.
    j.run("Frame bounds", {id}, [&] {
        f.x = r.x; f.y = r.y; f.w = r.w; f.h = r.h;
        d.put(id, f);
    });
}

bool frames_end(DocJournal& j) { return j.commit(); }

int frames_rehome(DocJournal& j, Doc& d, const std::vector<Placed>& pl, const std::vector<Doc::Id>& ids) {
    int n = 0;
    for (const auto& id : ids) {
        const auto* f = d.as<Frame>(id);
        if (!f || !d.as<Page>(f->owner)) continue;
        const auto from = std::find_if(pl.begin(), pl.end(), [&](const Placed& p) { return p.id == f->owner; });
        if (from == pl.end()) continue;
        const double cx = from->r.x + f->x + f->w / 2, cy = from->r.y + f->y + f->h / 2;
        const auto to = std::find_if(pl.begin(), pl.end(), [&](const Placed& p) {
            return !p.master && d.as<Page>(p.id) && p.id != f->owner && p.r.contains(cx, cy);
        });
        if (to == pl.end()) continue;
        Frame g = *f;
        g.owner = to->id;
        g.x = from->r.x + f->x - to->r.x;
        g.y = from->r.y + f->y - to->r.y;
        g.overrides.clear();   // a page's override moved off its page is just a frame
        j.run("Move to page", {id}, [&] { d.put(id, g); });
        ++n;
    }
    return n;
}

Doc::Id add_frame(DocJournal& j, Doc& d, const Doc::Id& owner, FrameKind kind, const Rect& r,
                  int columns, double gutter) {
    if (!d.as<Page>(owner) && !d.as<Master>(owner)) return {};
    if (r.w < 1 || r.h < 1) return {};
    static const char* what[] = {"text frame", "image frame", "shape"};
    Doc::Id made;
    j.run(std::string("Draw ") + what[static_cast<int>(kind)], {}, [&] {
        std::vector<Doc::Id> ids;
        Frame f;
        f.owner = owner;
        f.kind = kind;
        f.x = r.x; f.y = r.y; f.w = r.w; f.h = r.h;
        if (kind == FrameKind::Text) { f.columns = std::max(1, columns); f.gutter = gutter; }
        if (kind == FrameKind::Shape)   // s016: a new shape is outlined in black, so it prints
            for (const auto& sw : d.ids_of<Swatch>())
                if (d.as<Swatch>(sw)->name == "Black") { f.stroke = sw; break; }
        double z = 0;   // on top of the owner's frames
        for (const auto& o : d.frames_on(owner)) z = std::max(z, d.as<Frame>(o)->z);
        f.z = z + 1;
        if (kind == FrameKind::Text) {
            Doc::Id style;
            for (const auto& s : d.ids_of<Style>())
                if (d.as<Style>(s)->kind == StyleKind::Paragraph) { style = s; break; }
            const Doc::Id story = d.fresh_id("story");
            d.put(story, Story{"Story " + story.substr(story.find('.') + 1), {{style, ""}}});
            ids.push_back(story);
            f.story = story;
        }
        made = d.fresh_id("frame");
        d.put(made, f);
        ids.push_back(made);
        return ids;
    });
    return made;
}

namespace {
// The frame whose `next` is `id` ("" if none).
Doc::Id prev_of(const Doc& d, const Doc::Id& id) {
    for (const auto& o : d.ids_of<Frame>())
        if (d.as<Frame>(o)->next == id) return o;
    return {};
}
}  // namespace

namespace {
// Is `rec` (a story or an asset) shown by any frame other than `but`?
bool used_by_other(const Doc& d, const Doc::Id& rec, const std::vector<Doc::Id>& but) {
    for (const auto& o : d.ids_of<Frame>()) {
        if (std::find(but.begin(), but.end(), o) != but.end()) continue;
        const auto* f = d.as<Frame>(o);
        if (f->story == rec || f->asset == rec) return true;
    }
    return false;
}
}  // namespace

bool delete_frames(DocJournal& j, Doc& d, const std::vector<Doc::Id>& ids) {
    std::vector<Doc::Id> frames;
    for (const auto& id : ids) if (d.as<Frame>(id)) frames.push_back(id);
    if (frames.empty()) return false;
    const auto add = [](std::vector<Doc::Id>& v, const Doc::Id& id) {
        if (!id.empty() && std::find(v.begin(), v.end(), id) == v.end()) v.push_back(id);
    };
    // In the step: the frames, every frame that points at one of them (its
    // `next` is re-spliced), and the stories / images left with no frame.
    std::vector<Doc::Id> touched = frames;
    std::vector<Doc::Id> orphans;
    for (const auto& id : frames) {
        add(touched, prev_of(d, id));
        const auto* f = d.as<Frame>(id);
        for (const auto& rec : {f->story, f->asset})
            if (!rec.empty() && !used_by_other(d, rec, frames)) { add(touched, rec); add(orphans, rec); }
    }
    return j.run("Delete " + plural(frames.size(), "frame", "frames"), touched, [&] {
        for (const auto& id : frames) {
            const auto* f = d.as<Frame>(id);
            if (!f) continue;
            const Doc::Id after = f->next;
            const Doc::Id before = prev_of(d, id);
            if (!before.empty()) {
                Frame b = *d.as<Frame>(before);
                b.next = after;
                d.put(before, b);
            }
            d.put(id, std::nullopt);
        }
        for (const auto& o : orphans) d.put(o, std::nullopt);
    });
}

bool set_export(DocJournal& j, Doc& d, const ExportPrefs& e) {
    if (d.settings().exp == e) return false;
    return j.run("Export settings", {"doc"}, [&] {
        Settings s = d.settings();
        s.exp = e;
        d.put("doc", s);
    });
}

Doc::Id override_master_item(DocJournal& j, Doc& d, const Doc::Id& page, const Doc::Id& mf) {
    const auto* f = d.as<Frame>(mf);
    if (!f || !d.as<Page>(page) || !d.as<Master>(f->owner)) return {};
    const auto chain = master_chain(d, page);
    if (std::find(chain.begin(), chain.end(), f->owner) == chain.end() || !master_item_on(d, mf, page)) return {};
    Doc::Id made;
    j.run("Override master item", {}, [&] {
        std::vector<Doc::Id> ids;   // what's made: the step's records
        Frame c = *d.as<Frame>(mf);
        c.owner = page;
        c.overrides = mf;
        c.side = Side::Any;
        c.next.clear();
        c.z = 0;
        for (const auto& o : d.frames_on(page)) c.z = std::max(c.z, d.as<Frame>(o)->z + 1);
        if (const auto* st = d.as<Story>(c.story)) {   // its own words from here on
            Story s = *st;
            s.name += " (page " + d.folio(page) + ")";
            c.story = d.fresh_id("story");
            d.put(c.story, s);
            ids.push_back(c.story);
        }
        made = d.fresh_id("frame");
        d.put(made, c);
        ids.push_back(made);
        return ids;
    });
    return made;
}

bool remove_overrides(DocJournal& j, Doc& d, const std::vector<Doc::Id>& ids) {
    std::vector<Doc::Id> frames, stories;
    for (const auto& id : ids)
        if (const auto* f = d.as<Frame>(id); f && !f->overrides.empty()) {
            frames.push_back(id);
            if (!f->story.empty() && !used_by_other(d, f->story, {id})) stories.push_back(f->story);
        }
    if (frames.empty()) return false;
    auto touched = frames;
    touched.insert(touched.end(), stories.begin(), stories.end());
    return j.run(frames.size() == 1 ? "Remove override" : "Remove overrides", touched, [&] {
        for (const auto& id : frames) d.put(id, std::nullopt);
        for (const auto& s : stories) d.put(s, std::nullopt);
    });
}

bool edit_masters(DocJournal& j, Doc& d, const std::vector<Doc::Id>& ids, const std::string& label,
                  const std::function<void(Master&)>& f) {
    std::vector<Doc::Id> ms;
    for (const auto& id : ids) {
        const auto* m = d.as<Master>(id);
        if (!m) continue;
        Master t = *m;
        f(t);
        if (!(t == *m)) ms.push_back(id);
    }
    if (ms.empty()) return false;
    return j.run(label, ms, [&] {
        for (const auto& id : ms) {
            Master t = *d.as<Master>(id);
            f(t);
            d.put(id, t);
        }
    });
}

bool edit_frames(DocJournal& j, Doc& d, const std::vector<Doc::Id>& ids, const std::string& label,
                 const std::function<void(Frame&)>& f) {
    std::vector<Doc::Id> frames;
    for (const auto& id : ids) {
        const auto* fr = d.as<Frame>(id);
        if (!fr) continue;
        Frame t = *fr;
        f(t);
        if (!(t == *fr)) frames.push_back(id);
    }
    if (frames.empty()) return false;
    return j.run(label, frames, [&] {
        for (const auto& id : frames) {
            Frame t = *d.as<Frame>(id);
            f(t);
            d.put(id, t);
        }
    });
}

// ── text (s010) ─────────────────────────────────────────────────────────────
TextPos edit_text(DocJournal& j, Doc& d, const Doc::Id& story, TextPos a, TextPos b, const std::string& text,
                  const std::string& label, const std::string& merge_key) {
    const auto* st = d.as<Story>(story);
    if (!st) return a;
    Story s = *st;
    if (s.paras.empty()) {   // a story with no paragraph yet: one, in the first paragraph style
        Doc::Id style;
        for (const auto& id : d.ids_of<Style>())
            if (d.as<Style>(id)->kind == StyleKind::Paragraph) { style = id; break; }
        s.paras.push_back({style, ""});
    }
    a = clamp_pos(s, a);
    b = clamp_pos(s, b);
    if (b < a) std::swap(a, b);

    // The inserted text as lines (\r\n and \r count as \n).
    std::vector<std::string> pieces(1);
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\r') { if (i + 1 < text.size() && text[i + 1] == '\n') ++i; pieces.emplace_back(); }
        else if (c == '\n') pieces.emplace_back();
        else pieces.back() += c;
    }
    const auto& pa = s.paras[static_cast<std::size_t>(a.para)];
    const auto& pb = s.paras[static_cast<std::size_t>(b.para)];
    // Runs ride along (s011): the text before / after keeps its character
    // styles; what is typed or pasted takes the style it lands in.
    const auto head = spans_of(pa, 0, a.byte);
    const auto tail = spans_of(pb, b.byte, static_cast<int>(pb.text.size()));
    // Replacing a selection: the new text takes the first replaced
    // character's style (as InDesign); typing at a caret, the one before it.
    const std::string cstyle = (b != a && a.byte < static_cast<int>(pa.text.size())) ? char_style_at(pa, a.byte + 1)
                                                                                     : char_style_at(pa, a.byte);
    const Local local = (b != a && a.byte < static_cast<int>(pa.text.size())) ? local_at(pa, a.byte + 1)
                                                                              : local_at(pa, a.byte);   // s030
    const std::string style = pa.style;
    const int restart = pa.restart;   // s047: the first paragraph keeps its restart
    const ParaStart brk = pa.brk;     // s055: and its break
    std::size_t head_len = 0;
    for (const auto& sp : head) head_len += sp.text.size();

    std::vector<Para> made;
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        std::vector<Span> sp;
        if (i == 0) sp = head;
        sp.push_back({pieces[i], cstyle, local});
        if (i + 1 == pieces.size()) sp.insert(sp.end(), tail.begin(), tail.end());
        // s037: Enter at a paragraph's end starts its Next style.
        const bool next = i > 0 && text == "\n" && tail.empty();
        made.push_back(para_of(next ? next_style_of(d, style) : style, sp));
        if (i == 0) { made.back().restart = restart; made.back().brk = brk; }
    }
    s.paras.erase(s.paras.begin() + a.para, s.paras.begin() + b.para + 1);
    s.paras.insert(s.paras.begin() + a.para, made.begin(), made.end());

    TextPos caret{a.para + static_cast<int>(pieces.size()) - 1,
                  static_cast<int>((pieces.size() == 1 ? head_len : 0) + pieces.back().size())};
    if (s == *st) return a;
    j.run(label, {story}, [&] { d.put(story, s); }, merge_key);
    return caret;
}

// ── styles (s011) ───────────────────────────────────────────────────────────
bool set_para_style(DocJournal& j, Doc& d, const Doc::Id& story, TextPos a, TextPos b, const Doc::Id& style) {
    const auto* st = d.as<Story>(story);
    const auto* sty = d.as<Style>(style);
    if (!st || !sty || sty->kind != StyleKind::Paragraph || st->paras.empty()) return false;
    a = clamp_pos(*st, a);
    b = clamp_pos(*st, b);
    if (b < a) std::swap(a, b);
    Story s = *st;
    for (int p = a.para; p <= b.para; ++p) s.paras[static_cast<std::size_t>(p)].style = style;
    if (s == *st) return false;
    return j.run("Paragraph style: " + sty->name, {story}, [&] { d.put(story, s); });
}

bool set_char_style(DocJournal& j, Doc& d, const Doc::Id& story, TextPos a, TextPos b, const Doc::Id& style) {
    const auto* st = d.as<Story>(story);
    const auto* sty = style.empty() ? nullptr : d.as<Style>(style);
    if (!st || st->paras.empty() || (!style.empty() && (!sty || sty->kind != StyleKind::Character))) return false;
    a = clamp_pos(*st, a);
    b = clamp_pos(*st, b);
    if (b < a) std::swap(a, b);
    if (a == b) return false;
    Story s = *st;
    for (int p = a.para; p <= b.para; ++p) {
        auto& para = s.paras[static_cast<std::size_t>(p)];
        const int n = static_cast<int>(para.text.size());
        const int from = p == a.para ? a.byte : 0, to = p == b.para ? b.byte : n;
        auto sp = spans_of(para, 0, from);
        for (auto x : spans_of(para, from, to)) { x.style = style; sp.push_back(x); }
        const auto rest = spans_of(para, to, n);
        sp.insert(sp.end(), rest.begin(), rest.end());
        const int restart = para.restart;   // s047
        const ParaStart brk = para.brk;     // s055
        para = para_of(para.style, sp);
        para.restart = restart;
        para.brk = brk;
    }
    if (s == *st) return false;
    return j.run(sty ? "Character style: " + sty->name : std::string("No character style"), {story},
                 [&] { d.put(story, s); });
}

bool set_local(DocJournal& j, Doc& d, const Doc::Id& story, TextPos a, TextPos b, const std::string& label,
               const std::function<void(Local&)>& f) {
    const auto* st = d.as<Story>(story);
    if (!st || st->paras.empty()) return false;
    a = clamp_pos(*st, a);
    b = clamp_pos(*st, b);
    if (b < a) std::swap(a, b);
    if (a == b) return false;
    Story s = *st;
    for (int p = a.para; p <= b.para; ++p) {
        auto& para = s.paras[static_cast<std::size_t>(p)];
        const int n = static_cast<int>(para.text.size());
        const int from = p == a.para ? a.byte : 0, to = p == b.para ? b.byte : n;
        auto sp = spans_of(para, 0, from);
        for (auto x : spans_of(para, from, to)) { f(x.local); sp.push_back(x); }
        const auto rest = spans_of(para, to, n);
        sp.insert(sp.end(), rest.begin(), rest.end());
        const int restart = para.restart;   // s047
        const ParaStart brk = para.brk;     // s055
        para = para_of(para.style, sp);
        para.restart = restart;
        para.brk = brk;
    }
    if (s == *st) return false;
    return j.run(label, {story}, [&] { d.put(story, s); });
}

// ── Based on / Next style (s037) ────────────────────────────────────────────
namespace {
struct StyleField {
    const char* key;
    const char* label;
    void (*copy)(Style&, const Style&);
    bool (*same)(const Style&, const Style&);
};
#define COMPOSIX_SF(k, label, m) \
    StyleField{k, label, [](Style& t, const Style& f) { t.m = f.m; }, [](const Style& a, const Style& b) { return a.m == b.m; }}
const std::vector<StyleField>& style_fields() {
    static const std::vector<StyleField> fs{
        COMPOSIX_SF("family", "Font", family),
        COMPOSIX_SF("size", "Size", size),
        COMPOSIX_SF("leading", "Leading", leading),
        COMPOSIX_SF("bold", "Weight", bold),
        COMPOSIX_SF("italic", "Slant", italic),
        COMPOSIX_SF("color", "Color", color),
        COMPOSIX_SF("color_tint", "Tint", color_tint),
        COMPOSIX_SF("align", "Align", align),
        COMPOSIX_SF("space_before", "Space before", space_before),
        COMPOSIX_SF("space_after", "Space after", space_after),
        COMPOSIX_SF("left_indent", "Left indent", left_indent),
        COMPOSIX_SF("right_indent", "Right indent", right_indent),
        COMPOSIX_SF("first_indent", "First indent", first_indent),
        COMPOSIX_SF("skip_after_heading", "Indent when", skip_after_heading),
        COMPOSIX_SF("tabs", "Tabs", tabs),
        COMPOSIX_SF("drop_lines", "Drop cap", drop_lines),
        COMPOSIX_SF("drop_chars", "Drop letters", drop_chars),
        COMPOSIX_SF("align_grid", "Baseline grid", align_grid),
        COMPOSIX_SF("hyphenate", "Hyphenate", hyphenate),
        COMPOSIX_SF("keep_next", "Keep with next", keep_next),
        COMPOSIX_SF("keep_lines", "Keep lines", keep_lines),
        COMPOSIX_SF("start", "Start", start),
        COMPOSIX_SF("rule_above", "Rule above", rule_above),
        COMPOSIX_SF("rule_below", "Rule below", rule_below),
        COMPOSIX_SF("list", "List", list),
    };
    return fs;
}
#undef COMPOSIX_SF

bool owns(const Style& s, const std::string& key) {
    return std::find(s.own.begin(), s.own.end(), key) != s.own.end();
}
void add_own(Style& s, const std::vector<std::string>& keys) {
    s.own.insert(s.own.end(), keys.begin(), keys.end());
    std::sort(s.own.begin(), s.own.end());
    s.own.erase(std::unique(s.own.begin(), s.own.end()), s.own.end());
}
// Each descendant of `roots` takes its base's settings again (bases first).
void write_through(Doc& d, const std::vector<Doc::Id>& roots) {
    for (const auto& r : roots)
        for (const auto& id : style_descendants(d, r)) {
            const auto* cur = d.as<Style>(id);
            const auto* base = cur ? d.as<Style>(cur->based_on) : nullptr;
            if (!base) continue;
            Style t = *cur;
            inherit_from(t, *base);
            if (!(t == *cur)) d.put(id, t);
        }
}
// `roots` and every style based on them, once each.
std::vector<Doc::Id> with_descendants(const Doc& d, const std::vector<Doc::Id>& roots) {
    std::vector<Doc::Id> out = roots;
    for (const auto& r : roots) {
        const auto ds = style_descendants(d, r);
        out.insert(out.end(), ds.begin(), ds.end());
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}
}  // namespace

const std::vector<std::string>& style_field_keys() {
    static const std::vector<std::string> keys = [] {
        std::vector<std::string> k;
        for (const auto& f : style_fields()) k.push_back(f.key);
        return k;
    }();
    return keys;
}

std::string style_field_label(const std::string& key) {
    for (const auto& f : style_fields()) if (key == f.key) return f.label;
    return key;
}

std::vector<std::string> style_differences(const Style& a, const Style& b) {
    std::vector<std::string> out;
    for (const auto& f : style_fields()) if (!f.same(a, b)) out.push_back(f.key);
    std::sort(out.begin(), out.end());
    return out;
}

void inherit_from(Style& child, const Style& base) {
    for (const auto& f : style_fields()) if (!owns(child, f.key)) f.copy(child, base);
}

std::vector<Doc::Id> style_descendants(const Doc& d, const Doc::Id& style) {
    std::vector<Doc::Id> out;
    std::vector<Doc::Id> frontier{style};
    std::set<Doc::Id> seen{style};
    const auto all = d.ids_of<Style>();
    while (!frontier.empty()) {
        std::vector<Doc::Id> next;
        for (const auto& p : frontier)
            for (const auto& id : all)
                if (d.as<Style>(id)->based_on == p && seen.insert(id).second) {
                    out.push_back(id);
                    next.push_back(id);
                }
        frontier = std::move(next);
    }
    return out;
}

bool can_base_on(const Doc& d, const Doc::Id& style, const Doc::Id& base) {
    const auto* s = d.as<Style>(style);
    if (!s) return false;
    if (base.empty()) return true;
    const auto* b = d.as<Style>(base);
    // s046: a character style on a character style, a paragraph style on a paragraph style.
    if (!b || b->kind != s->kind || base == style) return false;
    const auto ds = style_descendants(d, style);
    return std::find(ds.begin(), ds.end(), base) == ds.end();
}

bool set_style_base(DocJournal& j, Doc& d, const Doc::Id& style, const Doc::Id& base) {
    if (!can_base_on(d, style, base)) return false;
    const auto* s = d.as<Style>(style);
    if (s->based_on == base) return false;
    const auto* b = d.as<Style>(base);
    return j.run(b ? "Based on " + b->name : "Based on None", {style}, [&] {
        Style t = *d.as<Style>(style);
        t.based_on = base;
        t.own = b ? style_differences(t, *b) : std::vector<std::string>{};   // it looks the same as before
        d.put(style, t);
    });
}

bool reset_to_base(DocJournal& j, Doc& d, const std::vector<Doc::Id>& ids) {
    std::vector<Doc::Id> styles;
    for (const auto& id : ids)
        if (const auto* s = d.as<Style>(id); s && d.as<Style>(s->based_on) && !s->own.empty()) styles.push_back(id);
    if (styles.empty()) return false;
    return j.run("Reset to base", with_descendants(d, styles), [&] {
        for (const auto& id : styles) {
            Style t = *d.as<Style>(id);
            t.own.clear();
            inherit_from(t, *d.as<Style>(t.based_on));
            d.put(id, t);
        }
        write_through(d, styles);
    });
}

int list_number(const Doc& d, const Story& s, int para) {
    if (para < 0 || para >= static_cast<int>(s.paras.size())) return 0;
    const auto& style = s.paras[static_cast<std::size_t>(para)].style;
    const auto* me = d.as<Style>(style);
    const bool on = me && me->list.continues;
    const int level = me ? me->list.level : 1;
    int n = 0;
    for (int p = para; p >= 0; --p) {
        const auto& q = s.paras[static_cast<std::size_t>(p)];
        if (q.style == style) {
            ++n;
            if (q.restart > 0) return q.restart + n - 1;
            continue;
        }
        const auto* qs = d.as<Style>(q.style);
        if (qs && qs->list.type != ListType::None && qs->list.level < level) break;   // a parent item
        if (qs && qs->list.type != ListType::None && qs->list.level > level) continue;   // its own sub-items: counts on
        if (!on) break;
    }
    return n;
}

bool set_list_restart(DocJournal& j, Doc& d, const Doc::Id& story, TextPos a, TextPos b, int n) {
    const auto* st = d.as<Story>(story);
    if (!st || st->paras.empty()) return false;
    Story s = *st;
    a = clamp_pos(s, a);
    b = clamp_pos(s, b);
    if (b < a) std::swap(a, b);
    for (int p = a.para; p <= b.para; ++p) s.paras[static_cast<std::size_t>(p)].restart = std::max(0, n);
    if (s == *st) return false;
    return j.run(n > 0 ? "Restart numbering" : "Continue numbering", {story}, [&] { d.put(story, s); });
}

TextPos insert_break(DocJournal& j, Doc& d, const Doc::Id& story, TextPos at, ParaStart kind) {
    const auto* st = d.as<Story>(story);
    if (!st || st->paras.empty()) return at;
    at = clamp_pos(*st, at);
    static const char* labels[] = {"Remove break", "Column break", "Frame break", "Page break", "Right-hand page break"};
    TextPos caret = at;
    j.run(labels[static_cast<int>(kind)], {story}, [&] {
        if (at.byte > 0 && kind != ParaStart::Anywhere) {
            // Split here, the Enter way -- but the new paragraph keeps this style.
            Story s = *d.as<Story>(story);
            const std::string style = s.paras[static_cast<std::size_t>(at.para)].style;
            caret = edit_text(j, d, story, at, at, "\n");
            s = *d.as<Story>(story);
            s.paras[static_cast<std::size_t>(caret.para)].style = style;
            s.paras[static_cast<std::size_t>(caret.para)].brk = kind;
            d.put(story, s);
        } else {
            Story s = *d.as<Story>(story);
            s.paras[static_cast<std::size_t>(at.para)].brk = kind;
            caret = {at.para, 0};
            if (!(s == *d.as<Story>(story))) d.put(story, s);
        }
    });
    return caret;
}

ParaStart start_of(const Doc& d, const Para& p) {
    if (p.brk != ParaStart::Anywhere) return p.brk;
    const auto* s = d.as<Style>(p.style);
    return s ? s->start : ParaStart::Anywhere;
}

std::string list_marker(const Doc& d, const Story& s, int para) {
    if (para < 0 || para >= static_cast<int>(s.paras.size())) return {};
    const auto* st = d.as<Style>(s.paras[static_cast<std::size_t>(para)].style);
    if (!st || st->kind != StyleKind::Paragraph) return {};
    switch (st->list.type) {
    case ListType::None:   return {};
    case ListType::Bullet: return st->list.bullet;
    case ListType::Number: return format_folio(list_number(d, s, para), st->list.numbering) + st->list.after;
    }
    return {};
}

Doc::Id next_style_of(const Doc& d, const Doc::Id& style) {
    const auto* s = d.as<Style>(style);
    if (!s || s->next.empty()) return style;
    const auto* n = d.as<Style>(s->next);
    return n && n->kind == StyleKind::Paragraph ? s->next : style;
}

bool edit_styles(DocJournal& j, Doc& d, const std::vector<Doc::Id>& ids, const std::string& label,
                 const std::function<void(Style&)>& f) {
    std::vector<Doc::Id> styles;
    for (const auto& id : ids) if (d.as<Style>(id)) styles.push_back(id);
    if (styles.empty()) return false;
    return j.run(label, with_descendants(d, styles), [&] {
        for (const auto& id : styles) {
            const Style before = *d.as<Style>(id);
            Style st = before;
            f(st);
            // s037: a setting changed on a based-on style is its own now.
            if (d.as<Style>(st.based_on)) add_own(st, style_differences(before, st));
            d.put(id, st);
        }
        write_through(d, styles);
    });
}

Doc::Id duplicate_style(DocJournal& j, Doc& d, const Doc::Id& style) {
    const auto* st = d.as<Style>(style);
    if (!st) return {};
    Doc::Id made;
    j.run("Duplicate style " + st->name, {}, [&]() -> std::vector<Doc::Id> {
        Style copy = *d.as<Style>(style);
        copy.name += " copy";
        copy.pos = d.pos_after<Style>(style);
        made = d.fresh_id("style");
        d.put(made, copy);
        return {made};
    });
    return made;
}

// ── swatches (s058) ─────────────────────────────────────────────────────────
std::uint32_t swatch_preview(const Swatch& s) {
    const auto c = [](double v) { return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0, 255.0))); };
    double r = 0, g = 0, b = 0;
    if (s.model == ColorModel::RGB) { r = s.v[0]; g = s.v[1]; b = s.v[2]; }
    else if (s.model == ColorModel::Gray) { r = g = b = 255 * (1 - std::clamp(s.v[0], 0.0, 100.0) / 100); }
    else {   // CMYK, or a spot's CMYK alternate
        const double C = s.v[0] / 100, M = s.v[1] / 100, Y = s.v[2] / 100, K = s.v[3] / 100;
        r = 255 * (1 - std::min(1.0, C)) * (1 - std::min(1.0, K));
        g = 255 * (1 - std::min(1.0, M)) * (1 - std::min(1.0, K));
        b = 255 * (1 - std::min(1.0, Y)) * (1 - std::min(1.0, K));
    }
    return (c(r) << 16) | (c(g) << 8) | c(b);
}

void swatch_from_rgb(Swatch& s, std::uint32_t rgb) {
    const double r = ((rgb >> 16) & 0xff) / 255.0, g = ((rgb >> 8) & 0xff) / 255.0, b = (rgb & 0xff) / 255.0;
    s.v = {0, 0, 0, 0};
    if (s.model == ColorModel::RGB) {
        s.v = {r * 255, g * 255, b * 255, 0};
    } else if (s.model == ColorModel::Gray) {
        s.v[0] = std::round((1 - (0.299 * r + 0.587 * g + 0.114 * b)) * 100);
    } else {
        const double k = 1 - std::max({r, g, b});
        if (k < 1) {
            s.v[0] = std::round((1 - r - k) / (1 - k) * 100);
            s.v[1] = std::round((1 - g - k) / (1 - k) * 100);
            s.v[2] = std::round((1 - b - k) / (1 - k) * 100);
        }
        s.v[3] = std::round(k * 100);
    }
}

Doc::Id add_swatch(DocJournal& j, Doc& d, Swatch s) {
    Doc::Id made;
    s.preview = swatch_preview(s);
    double pos = 0;
    for (const auto& id : d.ids_of<Swatch>()) pos = std::max(pos, d.as<Swatch>(id)->pos);
    s.pos = pos + 1;
    if (s.name.empty()) s.name = "New Swatch";
    j.run("New swatch " + s.name, {}, [&]() -> std::vector<Doc::Id> {
        made = d.fresh_id("swatch");
        d.put(made, s);
        return {made};
    });
    return made;
}

bool edit_swatch(DocJournal& j, Doc& d, const Doc::Id& id, Swatch s) {
    const auto* cur = d.as<Swatch>(id);
    if (!cur) return false;
    s.pos = cur->pos;
    // The screen colour from the values -- unless they're as they were (a
    // swatch's own tuned preview, as the sample's, is kept).
    s.preview = (s.model == cur->model && s.v == cur->v) ? cur->preview : swatch_preview(s);
    if (s.name.empty()) s.name = cur->name;
    if (s == *cur) return false;
    return j.run("Edit swatch " + s.name, {id}, [&] { d.put(id, s); });
}

Doc::Id duplicate_swatch(DocJournal& j, Doc& d, const Doc::Id& id) {
    const auto* cur = d.as<Swatch>(id);
    if (!cur) return {};
    Swatch copy = *cur;
    copy.name += " copy";
    return add_swatch(j, d, copy);
}

bool delete_swatch(DocJournal& j, Doc& d, const Doc::Id& id) {
    const auto* cur = d.as<Swatch>(id);
    if (!cur || swatch_uses(d, id) > 0) return false;
    return j.run("Delete swatch " + cur->name, {id}, [&] { d.put(id, std::nullopt); });
}

int style_uses(const Doc& d, const Doc::Id& style) {
    int n = 0;
    for (const auto& sid : d.ids_of<Story>())
        for (const auto& p : d.as<Story>(sid)->paras) {
            n += p.style == style;
            for (const auto& r : p.runs) n += r.style == style;
        }
    for (const auto& lid : d.ids_of<GenList>())
        for (const auto& fs : d.as<GenList>(lid)->from_styles) n += fs == style;
    return n;
}

bool delete_styles(DocJournal& j, Doc& d, const std::vector<Doc::Id>& ids) {
    std::vector<Doc::Id> gone;
    for (const auto& id : ids) if (d.as<Style>(id) && style_uses(d, id) == 0) gone.push_back(id);
    if (gone.empty()) return false;
    const auto going = [&](const Doc::Id& id) { return std::find(gone.begin(), gone.end(), id) != gone.end(); };
    // s037: styles based on one going take its base instead (looking the
    // same), and a Next style that goes becomes "this one again".
    std::vector<Doc::Id> touched = gone;
    for (const auto& id : d.ids_of<Style>()) {
        const auto* s = d.as<Style>(id);
        if (!going(id) && (going(s->based_on) || going(s->next))) touched.push_back(id);
    }
    return j.run("Delete " + plural(gone.size(), "style", "styles"), touched, [&] {
        for (const auto& id : touched) {
            if (going(id)) continue;
            Style t = *d.as<Style>(id);
            if (going(t.next)) t.next.clear();
            if (going(t.based_on)) {
                Doc::Id base = t.based_on;
                for (int guard = 0; going(base) && guard < 64; ++guard) base = d.as<Style>(base)->based_on;
                const auto* b = going(base) ? nullptr : d.as<Style>(base);
                t.based_on = b ? base : Doc::Id{};
                if (b) add_own(t, style_differences(t, *b));
                else t.own.clear();
            }
            d.put(id, t);
        }
        for (const auto& id : gone) d.put(id, std::nullopt);
    });
}

// ── images (s009) ───────────────────────────────────────────────────────────
namespace {
// The linked asset for this file (the same path is one asset), or a new id.
Doc::Id asset_for(Doc& d, const ImageFile& f, bool& fresh) {
    for (const auto& a : d.ids_of<Asset>()) {
        const auto* r = d.as<Asset>(a);
        if (!r->embedded && r->path == f.path) { fresh = false; return a; }
    }
    fresh = true;
    return d.fresh_id("asset");
}
Asset asset_from(const ImageFile& f, double pos) {
    Asset a;
    a.name = f.name;
    a.pos = pos;
    a.path = f.path;
    a.color = f.color;
    a.px_w = f.vector ? 0 : f.px_w;
    a.px_h = f.vector ? 0 : f.px_h;
    a.stamp = f.stamp;
    return a;
}
Doc::Id existing_asset(const Doc& d, const ImageFile& f) {
    for (const auto& a : d.ids_of<Asset>()) {
        const auto* r = d.as<Asset>(a);
        if (!r->embedded && r->path == f.path) return a;
    }
    return {};
}
}  // namespace

bool place_image(DocJournal& j, Doc& d, const Doc::Id& frame, const ImageFile& file) {
    const auto* f = d.as<Frame>(frame);
    if (!f || f->kind != FrameKind::Image || file.path.empty()) return false;
    std::vector<Doc::Id> touched{frame};
    const Doc::Id old = f->asset;
    const Doc::Id same = existing_asset(d, file);
    if (!old.empty() && old == same) return false;   // already showing it
    if (!same.empty()) touched.push_back(same);
    const bool drop_old = !old.empty() && !used_by_other(d, old, {frame});
    if (drop_old) touched.push_back(old);
    return j.run("Place " + file.name, touched, [&]() -> std::vector<Doc::Id> {
        std::vector<Doc::Id> made;
        bool fresh = false;
        const Doc::Id a = asset_for(d, file, fresh);
        if (fresh) {
            d.put(a, asset_from(file, d.pos_after<Asset>({})));
            made.push_back(a);
        }
        Frame nf = *d.as<Frame>(frame);
        nf.asset = a;
        d.put(frame, nf);
        if (drop_old) d.put(old, std::nullopt);
        return made;
    });
}

Doc::Id place_image_new(DocJournal& j, Doc& d, const Doc::Id& owner, const Rect& r, const ImageFile& file) {
    if (!d.as<Page>(owner) && !d.as<Master>(owner)) return {};
    if (r.w < 1 || r.h < 1 || file.path.empty()) return {};
    std::vector<Doc::Id> touched;
    if (const auto same = existing_asset(d, file); !same.empty()) touched.push_back(same);
    Doc::Id made_frame;
    j.run("Place " + file.name, touched, [&]() -> std::vector<Doc::Id> {
        std::vector<Doc::Id> made;
        bool fresh = false;
        const Doc::Id a = asset_for(d, file, fresh);
        if (fresh) {
            d.put(a, asset_from(file, d.pos_after<Asset>({})));
            made.push_back(a);
        }
        Frame f;
        f.owner = owner;
        f.kind = FrameKind::Image;
        f.x = r.x; f.y = r.y; f.w = r.w; f.h = r.h;
        double z = 0;
        for (const auto& o : d.frames_on(owner)) z = std::max(z, d.as<Frame>(o)->z);
        f.z = z + 1;
        f.asset = a;
        made_frame = d.fresh_id("frame");
        d.put(made_frame, f);
        made.push_back(made_frame);
        return made;
    });
    return made_frame;
}

bool relink_assets(DocJournal& j, Doc& d, const std::vector<std::pair<Doc::Id, ImageFile>>& files) {
    std::vector<Doc::Id> ids;
    bool update = true;
    for (const auto& [id, f] : files) {
        const auto* a = d.as<Asset>(id);
        if (!a || f.path.empty() || std::find(ids.begin(), ids.end(), id) != ids.end()) continue;
        ids.push_back(id);
        if (a->embedded || a->path != f.path) update = false;
    }
    if (ids.empty()) return false;
    const std::string what = ids.size() == 1 ? d.as<Asset>(ids[0])->name : plural(ids.size(), "image", "images");
    return j.run((update ? "Update " : "Relink ") + what, ids, [&] {
        for (const auto& [id, f] : files) {
            const auto* a = d.as<Asset>(id);
            if (!a || f.path.empty()) continue;
            d.put(id, asset_from(f, a->pos));
        }
    });
}

bool embed_assets(DocJournal& j, Doc& d, const std::vector<std::pair<Doc::Id, std::string>>& rels) {
    std::vector<Doc::Id> ids;
    for (const auto& [id, rel] : rels)
        if (const auto* a = d.as<Asset>(id); a && !a->embedded && !rel.empty()) ids.push_back(id);
    if (ids.empty()) return false;
    const std::string what = ids.size() == 1 ? d.as<Asset>(ids[0])->name : plural(ids.size(), "image", "images");
    return j.run("Embed " + what, ids, [&] {
        for (const auto& [id, rel] : rels) {
            const auto* a = d.as<Asset>(id);
            if (!a || a->embedded || rel.empty()) continue;
            Asset na = *a;
            na.source = na.path;
            na.path = rel;
            na.embedded = true;
            d.put(id, na);
        }
    });
}

bool unembed_assets(DocJournal& j, Doc& d, const std::vector<Doc::Id>& assets) {
    std::vector<Doc::Id> ids;
    for (const auto& id : assets)
        if (const auto* a = d.as<Asset>(id); a && a->embedded && !a->source.empty()) ids.push_back(id);
    if (ids.empty()) return false;
    const std::string what = ids.size() == 1 ? d.as<Asset>(ids[0])->name : plural(ids.size(), "image", "images");
    return j.run("Unembed " + what, ids, [&] {
        for (const auto& id : ids) {
            Asset na = *d.as<Asset>(id);
            na.path = na.source;
            na.source.clear();
            na.embedded = false;
            na.stamp = file_stamp(na.path);
            d.put(id, na);
        }
    });
}

bool set_fit(DocJournal& j, Doc& d, const std::vector<Doc::Id>& ids, ImageFit fit) {
    std::vector<Doc::Id> frames;
    for (const auto& id : ids)
        if (const auto* f = d.as<Frame>(id); f && f->kind == FrameKind::Image && f->fit != fit) frames.push_back(id);
    if (frames.empty()) return false;
    return j.run(fit == ImageFit::Fill ? "Fill frame" : fit == ImageFit::Fit ? "Fit in frame" : "Place by hand", frames, [&] {
        for (const auto& id : frames) {
            Frame f = *d.as<Frame>(id);
            f.fit = fit;
            d.put(id, f);
        }
    });
}

bool set_picture_scale(DocJournal& j, Doc& d, const std::vector<Doc::Id>& ids, double percent) {
    std::vector<Doc::Id> frames;
    for (const auto& id : ids)
        if (const auto* f = d.as<Frame>(id); f && f->kind == FrameKind::Image && d.as<Asset>(f->asset)) frames.push_back(id);
    if (frames.empty() || percent <= 0) return false;
    return j.run("Picture scale", frames, [&] {
        for (const auto& id : frames) {
            Frame f = *d.as<Frame>(id);
            const auto& a = *d.as<Asset>(f.asset);
            const double iw = a.px_w > 0 ? a.px_w : 1, ih = a.px_h > 0 ? a.px_h : 1;
            const Rect now = picture_rect({0, 0, f.w, f.h}, f, iw, ih);
            const auto nat = picture_natural(a);
            const double w = nat.w * percent / 100, h = w * ih / iw;
            f.fit = ImageFit::Custom;
            f.pic_w = w;   // about the picture's centre
            f.pic_x = now.x + now.w / 2 - w / 2;
            f.pic_y = now.y + now.h / 2 - h / 2;
            d.put(id, f);
        }
    });
}

bool thread_frames(DocJournal& j, Doc& d, const Doc::Id& from, const Doc::Id& to) {
    const auto* a = d.as<Frame>(from);
    const auto* b = d.as<Frame>(to);
    if (!a || !b || from == to) return false;
    if (a->kind != FrameKind::Text || b->kind != FrameKind::Text) return false;
    if (!b->next.empty() || !prev_of(d, to).empty()) return false;   // `to` already in a thread
    if (!b->story.empty() && b->story == a->story) return false;
    std::vector<Doc::Id> touched{from, to};
    if (!a->story.empty()) touched.push_back(a->story);
    if (!b->story.empty() && b->story != a->story) touched.push_back(b->story);
    return j.run("Thread frames", touched, [&] {
        Frame fa = *d.as<Frame>(from);
        Frame fb = *d.as<Frame>(to);
        // `from` may have no story yet (an unthreaded tail): it takes `to`'s.
        if (fa.story.empty()) {
            fa.story = fb.story;
            fb.story.clear();
        }
        if (!fb.story.empty() && fb.story != fa.story) {
            // Append `to`'s text (skipping one empty paragraph) to `from`'s story.
            Story sa = *d.as<Story>(fa.story);
            const Story sb = *d.as<Story>(fb.story);
            for (const auto& p : sb.paras)
                if (!(sb.paras.size() == 1 && p.text.empty())) sa.paras.push_back(p);
            d.put(fa.story, sa);
            bool used = false;
            for (const auto& o : d.ids_of<Frame>())
                if (o != to && d.as<Frame>(o)->story == fb.story) used = true;
            if (!used) d.put(fb.story, std::nullopt);
        }
        fb.story = fa.story;
        fb.next = fa.next;    // slot in after `from`
        fa.next = to;
        d.put(from, fa);
        d.put(to, fb);
    });
}

bool unthread_after(DocJournal& j, Doc& d, const Doc::Id& from) {
    const auto* a = d.as<Frame>(from);
    if (!a || a->next.empty()) return false;
    std::vector<Doc::Id> tail;
    for (Doc::Id k = a->next; !k.empty() && d.as<Frame>(k) &&
                              std::find(tail.begin(), tail.end(), k) == tail.end();
         k = d.as<Frame>(k)->next)
        tail.push_back(k);
    std::vector<Doc::Id> touched{from};
    touched.insert(touched.end(), tail.begin(), tail.end());
    return j.run("Break thread", touched, [&] {
        Frame fa = *d.as<Frame>(from);
        fa.next.clear();
        d.put(from, fa);
        for (const auto& t : tail) {
            Frame ft = *d.as<Frame>(t);
            ft.story.clear();
            ft.next.clear();
            d.put(t, ft);
        }
    });
}

Doc::Id add_guide(DocJournal& j, Doc& d, const Doc::Id& owner, bool vertical, double pos) {
    if (!d.as<Page>(owner) && !d.as<Master>(owner)) return {};
    Doc::Id made;
    j.run(vertical ? "Add vertical guide" : "Add horizontal guide", {}, [&] {
        made = d.fresh_id("guide");
        d.put(made, Guide{owner, vertical, pos});
        return std::vector<Doc::Id>{made};
    });
    return made;
}

void guide_begin(DocJournal& j, Doc& d, const Doc::Id& id) {
    if (d.as<Guide>(id)) j.begin("Move guide", {id});
}

void guide_set(DocJournal& j, Doc& d, const Doc::Id& id, double pos) {
    const auto* g0 = d.as<Guide>(id);
    if (!g0 || g0->pos == pos) return;
    Guide g = *g0;
    j.run("Move guide", {id}, [&] { g.pos = pos; d.put(id, g); });
}

// Dragged off its page: the guide goes, inside the open gesture -- the
// gesture's one step records it as a removal ("Move guide" becomes one undo
// that brings it back where it started).
void guide_remove_in_gesture(DocJournal& j, Doc& d, const Doc::Id& id) {
    if (!d.as<Guide>(id)) return;
    j.relabel("Remove guide");
    j.run("Remove guide", {id}, [&] { d.put(id, std::nullopt); });
}

bool remove_guide(DocJournal& j, Doc& d, const Doc::Id& id) {
    if (!d.as<Guide>(id)) return false;
    return j.run("Remove guide", {id}, [&] { d.put(id, std::nullopt); });
}
void frames_cancel(DocJournal& j) { j.cancel(); }

const std::vector<Verb>& verbs() {
    // Each runs on the loaded sample. A verb the UI offers and this table
    // lacks is the gap the gate cannot see -- add it in the same change.
    static const std::vector<Verb> table = {
        {"add_pages", [](DocJournal& j, Doc& d) { add_pages(j, d, d.pages()[2], 2, "master.2"); }},
        {"remove_pages", [](DocJournal& j, Doc& d) {
             const auto p = d.pages();
             remove_pages(j, d, {p[0], p[4]});   // p[0] starts a section: it moves
         }},
        {"move_pages", [](DocJournal& j, Doc& d) {
             const auto p = d.pages();
             move_pages(j, d, {p[1], p[2]}, p[6]);
         }},
        {"set_master", [](DocJournal& j, Doc& d) {
             const auto p = d.pages();
             set_master(j, d, {p[1], p[3], p[5]}, "master.3");
         }},
        {"set_page_size", [](DocJournal& j, Doc& d) {
             const auto p = d.pages();
             set_page_size(j, d, {p[3]}, size_presets(d.settings())[1]);
         }},
        {"add_section", [](DocJournal& j, Doc& d) {
             add_section(j, d, d.pages()[9], "Appendix", Numbering::AlphaUpper, 1);
         }},
        {"set_numbering", [](DocJournal& j, Doc& d) {
             set_numbering(j, d, d.ids_of<Section>(), Numbering::RomanUpper);
         }},
        {"set_starts", [](DocJournal& j, Doc& d) {
             set_starts(j, d, d.ids_of<Section>(), Side::Left);
         }},
        {"remove_section", [](DocJournal& j, Doc& d) { remove_section(j, d, d.ids_of<Section>().back()); }},
        {"rename_section", [](DocJournal& j, Doc& d) { rename_section(j, d, d.ids_of<Section>().back(), "Part One"); }},   // s060
        {"section_start_at", [](DocJournal& j, Doc& d) { set_section_start_at(j, d, {d.ids_of<Section>().back()}, 5); }},
        {"section_master", [](DocJournal& j, Doc& d) {
             const auto sec = d.ids_of<Section>().back();
             for (const auto& m : d.ids_of<Master>())
                 if (m != d.as<Section>(sec)->master) { set_section_master(j, d, {sec}, m); break; }
         }},
        {"move_frames", [](DocJournal& j, Doc& d) { move_frames(j, d, {"frame.1", "frame.4"}, 9, -3); }},
        {"add_guide", [](DocJournal& j, Doc& d) { add_guide(j, d, "page.6", true, 100); }},
        {"add_text_frame", [](DocJournal& j, Doc& d) { add_frame(j, d, "page.8", FrameKind::Text, {54, 54, 200, 100}); }},
        {"add_image_frame", [](DocJournal& j, Doc& d) { add_frame(j, d, "page.8", FrameKind::Image, {54, 200, 200, 100}); }},
        {"add_shape", [](DocJournal& j, Doc& d) { add_frame(j, d, "page.8", FrameKind::Shape, {54, 400, 50, 50}); }},
        {"delete_frames", [](DocJournal& j, Doc& d) { delete_frames(j, d, {"frame.2", "frame.4"}); }},  // mid-thread
        {"override_master_item", [](DocJournal& j, Doc& d) { override_master_item(j, d, "page.7", "frame.13"); }},   // s025
        {"remove_overrides", [](DocJournal& j, Doc& d) { remove_overrides(j, d, {"frame.15"}); }},
        {"set_export", [](DocJournal& j, Doc& d) {   // s032
             auto e = d.settings().exp;
             e.marks = true;
             e.standard = 2;
             set_export(j, d, e);
         }},
        {"thread_frames", [](DocJournal& j, Doc& d) { thread_frames(j, d, "frame.3", "frame.7"); }},   // merges a story
        {"unthread_after", [](DocJournal& j, Doc& d) { unthread_after(j, d, "frame.1"); }},
        {"drag_guide", [](DocJournal& j, Doc& d) {
             guide_begin(j, d, "guide.1");
             guide_set(j, d, "guide.1", 60);
             guide_set(j, d, "guide.1", 72);
             frames_end(j);
         }},
        {"drag_guide_off", [](DocJournal& j, Doc& d) {   // dragged off its page: removed, one step
             guide_begin(j, d, "guide.1");
             guide_set(j, d, "guide.1", -40);
             guide_remove_in_gesture(j, d, "guide.1");
             frames_end(j);
         }},
        {"remove_guide", [](DocJournal& j, Doc& d) { remove_guide(j, d, "guide.2"); }},
        // s010: text. Typing, Enter (a split), a paste across paragraphs, a cut.
        {"type_text", [](DocJournal& j, Doc& d) { edit_text(j, d, "story.1", {1, 3}, {1, 3}, "abc"); }},
        {"enter", [](DocJournal& j, Doc& d) { edit_text(j, d, "story.1", {1, 5}, {1, 5}, "\n"); }},
        {"paste_lines", [](DocJournal& j, Doc& d) { edit_text(j, d, "story.1", {0, 2}, {2, 4}, "one\ntwo\nthree", "Paste"); }},
        {"cut", [](DocJournal& j, Doc& d) { edit_text(j, d, "story.1", {1, 0}, {3, 0}, "", "Cut"); }},
        // s014: master margins / columns.
        {"edit_masters", [](DocJournal& j, Doc& d) {
             edit_masters(j, d, {"master.2", "master.3"}, "Margins", [](Master& m) { m.margins.top = 72; m.columns = 2; });
         }},
        // s012: columns.
        {"edit_frames", [](DocJournal& j, Doc& d) {
             edit_frames(j, d, {"frame.1", "frame.10"}, "Columns", [](Frame& f) { f.columns = 2; f.gutter = 18; f.widths = {2, 1}; });
         }},
        // s011: styles.
        {"set_para_style", [](DocJournal& j, Doc& d) { set_para_style(j, d, "story.1", {1, 0}, {3, 2}, "style.3"); }},
        {"set_char_style", [](DocJournal& j, Doc& d) { set_char_style(j, d, "story.1", {1, 4}, {2, 6}, "style.5"); }},
        {"clear_char_style", [](DocJournal& j, Doc& d) { set_char_style(j, d, "story.4", {0, 5}, {0, 12}, ""); }},
        {"set_local", [](DocJournal& j, Doc& d) {   // s030: bold words, directly
             set_local(j, d, "story.1", {1, 4}, {1, 10}, "Bold", [](Local& l) { l.bold = 1; });
         }},
        {"change_hits", [](DocJournal& j, Doc& d) {   // s022: Change All
             change_hits(j, d, find_all(d, {"the"}), {true, "THE"}, "Change All");
         }},
        {"edit_styles", [](DocJournal& j, Doc& d) {
             edit_styles(j, d, {"style.1", "style.4"}, "Size", [](Style& s) { s.size = 12; s.leading = 15; });
         }},
        {"duplicate_style", [](DocJournal& j, Doc& d) { duplicate_style(j, d, "style.1"); }},
        // s058: swatches.
        {"add_swatch", [](DocJournal& j, Doc& d) { add_swatch(j, d, Swatch{"Teal", 0, ColorModel::CMYK, {80, 0, 30, 10}}); }},
        {"edit_swatch", [](DocJournal& j, Doc& d) {
             auto s = *d.as<Swatch>("swatch.4");
             s.v = {20, 120, 200, 0};
             edit_swatch(j, d, "swatch.4", s);
         }},
        {"duplicate_swatch", [](DocJournal& j, Doc& d) { duplicate_swatch(j, d, "swatch.3"); }},
        {"delete_unused_swatch", [](DocJournal& j, Doc& d) { delete_swatch(j, d, "swatch.4"); }},
        {"delete_unused_style", [](DocJournal& j, Doc& d) { delete_styles(j, d, {"style.6", "style.1"}); }},  // Body is used: stays
        // s037: Based on / Next style.
        {"set_style_base", [](DocJournal& j, Doc& d) { set_style_base(j, d, "style.6", "style.1"); }},
        {"reset_to_base", [](DocJournal& j, Doc& d) { reset_to_base(j, d, {"style.10"}); }},
        {"next_style", [](DocJournal& j, Doc& d) {
             edit_styles(j, d, {"style.1"}, "Next style", [](Style& s) { s.next = "style.3"; });
         }},
        {"list_restart", [](DocJournal& j, Doc& d) { set_list_restart(j, d, "story.4", {1, 0}, {1, 0}, 5); }},   // s047
        {"column_break", [](DocJournal& j, Doc& d) { insert_break(j, d, "story.1", {1, 20}, ParaStart::NextColumn); }},   // s055
        {"enter_next_style", [](DocJournal& j, Doc& d) {   // Enter at the end of "Chapter One"
             edit_text(j, d, "story.1", {0, 11}, {0, 11}, "\n");
         }},
        // s009: images. The files are made up -- the core never opens them.
        {"place_image", [](DocJournal& j, Doc& d) {   // replaces harbor.psd, which goes
             place_image(j, d, "frame.4", {"/pics/cove.png", "cove.png", 2400, 1600, false, "RGB", 7});
         }},
        {"place_image_new", [](DocJournal& j, Doc& d) {
             place_image_new(j, d, "page.8", {54, 54, 288, 192}, {"/pics/cove.png", "cove.png", 2400, 1600, false, "RGB", 7});
         }},
        {"place_same_file", [](DocJournal& j, Doc& d) {   // into a frame showing a shared asset
             place_image(j, d, "frame.8", {"/pics/cove.png", "cove.png", 2400, 1600, false, "RGB", 7});
         }},
        {"relink_asset", [](DocJournal& j, Doc& d) {
             relink_assets(j, d, {{"asset.2", {"/pics/harbor.png", "harbor.png", 3000, 2000, false, "RGB", 9}}});
         }},
        {"embed_assets", [](DocJournal& j, Doc& d) { embed_assets(j, d, {{"asset.2", "assets/harbor.psd"}}); }},
        {"unembed_assets", [](DocJournal& j, Doc& d) { unembed_assets(j, d, {"asset.1", "asset.3"}); }},
        {"set_fit", [](DocJournal& j, Doc& d) { set_fit(j, d, {"frame.4", "frame.5", "frame.1"}, ImageFit::Fit); }},
        {"set_picture_scale", [](DocJournal& j, Doc& d) { set_picture_scale(j, d, {"frame.4"}, 50); }},   // s035
        {"delete_last_frames", [](DocJournal& j, Doc& d) {   // the story and the image go too
             delete_frames(j, d, {"frame.4", "frame.6"});
         }},
        {"drag_frames", [](DocJournal& j, Doc& d) {   // press .. drag .. release
             frames_begin(j, d, {"frame.5", "frame.6"}, "Resize 2 frames");
             frames_set(j, d, "frame.5", {20, 40, 900, 400});
             frames_set(j, d, "frame.6", {20, 450, 400, 50});
             frames_set(j, d, "frame.5", {24, 44, 880, 390});
             frames_end(j);
         }},
    };
    return table;
}

}  // namespace doc
}  // namespace composix::core

namespace composix::core {

std::vector<std::vector<Doc::Id>> spreads(const Doc& d) {
    std::vector<std::vector<Doc::Id>> out;
    const auto pages = d.pages();
    for (std::size_t i = 0; i < pages.size(); ++i) {
        if (d.settings().facing && d.side_of(pages[i]) == Side::Left && i + 1 < pages.size() &&
            d.side_of(pages[i + 1]) == Side::Right) {
            out.push_back({pages[i], pages[i + 1]});
            ++i;
        } else {
            out.push_back({pages[i]});
        }
    }
    return out;
}

std::vector<Doc::Id> spread_of(const Doc& d, const Doc::Id& page) {
    for (const auto& s : spreads(d))
        if (std::find(s.begin(), s.end(), page) != s.end()) return s;
    return {page};
}

std::vector<Placed> layout_pages(const Doc& d, const std::vector<Doc::Id>& ids, double gap) {
    std::vector<Placed> out;
    double x = 0;
    // s027: a left page and the right page of its spread meet at the spine.
    const auto spine = [&](std::size_t i) {
        if (i + 1 >= ids.size() || !d.as<Page>(ids[i]) || !d.as<Page>(ids[i + 1])) return false;
        const auto sp = spread_of(d, ids[i]);
        return sp.size() == 2 && sp[0] == ids[i] && sp[1] == ids[i + 1];
    };
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const auto& id = ids[i];
        double w = 0, h = 0;
        bool master = false;
        if (const auto* p = d.as<Page>(id))        { w = p->w; h = p->h; }
        else if (const auto* m = d.as<Master>(id)) { w = m->w; h = m->h; master = true; }
        else continue;
        out.push_back({id, {x, 0, w, h}, master});
        x += w + (spine(i) ? 0 : gap);
    }
    return out;
}

std::vector<Doc::Id> pages_in_range(const Doc& d, const std::string& text, std::string* err) {
    const auto pages = d.pages();
    const auto trim = [](std::string t) {
        const auto a = t.find_first_not_of(" \t");
        if (a == std::string::npos) return std::string{};
        return t.substr(a, t.find_last_not_of(" \t") - a + 1);
    };
    std::string all = trim(text);
    std::string low = all;
    for (auto& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (all.empty() || low == "all") return pages;
    // An en dash typed or pasted reads as a hyphen.
    for (std::size_t at; (at = all.find("\u2013")) != std::string::npos;) all.replace(at, 3, "-");
    const auto index = [&](const std::string& folio) -> int {
        for (std::size_t i = 0; i < pages.size(); ++i) if (d.folio(pages[i]) == folio) return static_cast<int>(i);
        return -1;
    };
    std::vector<bool> take(pages.size(), false);
    std::size_t from = 0;
    while (from <= all.size()) {
        const auto comma = all.find(',', from);
        const std::string part = trim(all.substr(from, comma == std::string::npos ? std::string::npos : comma - from));
        from = comma == std::string::npos ? all.size() + 1 : comma + 1;
        if (part.empty()) continue;
        const auto dash = part.find('-', 1);
        const std::string a = trim(part.substr(0, dash)), b = dash == std::string::npos ? a : trim(part.substr(dash + 1));
        const int i = index(a), k = index(b);
        if (i < 0 || k < 0) {
            if (err) *err = "No page \u201c" + (i < 0 ? a : b) + "\u201d";
            return {};
        }
        if (k < i) {
            if (err) *err = "\u201c" + part + "\u201d runs backwards";
            return {};
        }
        for (int j = i; j <= k; ++j) take[static_cast<std::size_t>(j)] = true;
    }
    std::vector<Doc::Id> out;
    for (std::size_t i = 0; i < pages.size(); ++i) if (take[i]) out.push_back(pages[i]);
    if (out.empty() && err) *err = "No pages";
    return out;
}

std::vector<Doc::Id> master_chain(const Doc& d, const Doc::Id& id) {
    std::vector<Doc::Id> chain;
    Doc::Id m;
    if (const auto* p = d.as<Page>(id)) m = p->master;
    else if (const auto* ms = d.as<Master>(id)) m = ms->based_on;
    while (!m.empty() && d.as<Master>(m) &&
           std::find(chain.begin(), chain.end(), m) == chain.end()) {   // a cycle stops here
        chain.insert(chain.begin(), m);
        m = d.as<Master>(m)->based_on;
    }
    return chain;
}

double text_width(const Doc& d) {
    const auto& st = d.settings();
    const auto ms = d.ids_of<Master>();
    if (ms.empty()) return st.trim_w - 1.5 * kPt;
    const auto& m = *d.as<Master>(ms.front());
    return std::max(36.0, st.trim_w - m.margins.inside - m.margins.outside);
}

std::string tabs_text(const std::vector<TabStop>& tabs) {
    if (tabs.empty()) return "None";
    if (tabs.size() > 1) return std::to_string(tabs.size()) + " stops";
    static const char* kind[] = {"Left", "Centre", "Right", "Decimal"};
    const auto& t = tabs.front();
    std::string s = std::string(kind[static_cast<int>(t.align)]) + " " + inches(t.pos) + " in";
    if (t.leader == ".") s += ", dots";
    else if (!t.leader.empty()) s += ", " + t.leader;
    return s;
}

std::vector<std::pair<std::string, std::string>> tab_presets(const Doc& d) {
    const std::string w = inches(text_width(d)) + " in";
    return {{"none", "None"},
            {"right.", "Right at " + w + ", dots (contents)"},
            {"right", "Right at " + w},
            {"center", "Centre at " + inches(text_width(d) / 2) + " in"},
            {"every", "Left, every \u00bd in"}};
}

std::vector<TabStop> tab_preset(const Doc& d, const std::string& id) {
    const double w = text_width(d);
    if (id == "right.") return {{w, TabAlign::Right, "."}};
    if (id == "right") return {{w, TabAlign::Right, ""}};
    if (id == "center") return {{w / 2, TabAlign::Center, ""}};
    if (id == "every") {
        std::vector<TabStop> v;
        for (double x = 36; x < w - 1; x += 36) v.push_back({x, TabAlign::Left, ""});
        return v;
    }
    return {};
}

int marker_at(const std::string& t, std::size_t i) {
    if (i + 3 > t.size()) return -1;
    if (static_cast<unsigned char>(t[i]) != 0xEE || static_cast<unsigned char>(t[i + 1]) != 0x80) return -1;
    const int k = static_cast<unsigned char>(t[i + 2]) - 0x80;
    return k >= 0 && k <= 3 ? k : -1;
}

bool has_markers(const Story& s) {
    for (const auto& p : s.paras)
        if (p.text.find("\xEE\x80") != std::string::npos)
            for (std::size_t i = 0; i < p.text.size(); ++i)
                if (marker_at(p.text, i) >= 0) return true;
    return false;
}

std::uint32_t tint_rgb(std::uint32_t rgb, double t) {
    t = std::clamp(t, 0.0, 1.0);
    std::uint32_t out = 0;
    for (int sh : {16, 8, 0}) {
        const double c = (rgb >> sh) & 0xff;
        out |= static_cast<std::uint32_t>(std::lround(255 - t * (255 - c))) << sh;
    }
    return out;
}

const std::vector<int>& tint_steps() {
    static const std::vector<int> v{100, 90, 80, 70, 60, 50, 40, 30, 25, 20, 15, 10, 5};
    return v;
}

std::string tint_text(double t) { return std::to_string(static_cast<int>(std::lround(t * 100))) + "%"; }

std::vector<WrapRect> wrap_rects(const Doc& d, const Doc::Id& id) {
    std::vector<WrapRect> out;
    const auto* t = d.as<Frame>(id);
    if (!t) return out;
    auto owners = master_chain(d, t->owner);
    owners.push_back(t->owner);
    for (const auto& o : owners)
        for (const auto& fid : d.frames_on(o)) {
            const auto* f = d.as<Frame>(fid);
            if (fid == id || !f || f->wrap == Wrap::None) continue;
            if (o != t->owner && !master_item_on(d, fid, t->owner)) continue;   // s025: not shown on this page
            WrapRect r{f->x - f->wrap_off.left - t->x, f->y - f->wrap_off.top - t->y,
                       f->x + f->w + f->wrap_off.right - t->x, f->y + f->h + f->wrap_off.bottom - t->y,
                       f->wrap == Wrap::Jump};
            // Only what overlaps the text frame matters.
            if (r.x1 <= 0 || r.y1 <= 0 || r.x0 >= t->w || r.y0 >= t->h) continue;
            out.push_back(r);
        }
    return out;
}

Rect frame_in_layout(const Doc& d, const std::vector<Placed>& pl, const Doc::Id& id) {
    const auto* f = d.as<Frame>(id);
    if (!f) return {};
    for (const auto& p : pl)
        if (p.id == f->owner) return {p.r.x + f->x, p.r.y + f->y, f->w, f->h};
    return {};
}

Doc::Id hit_frame(const Doc& d, const std::vector<Placed>& pl, double x, double y, double tol) {
    // Every placed page's frames, even off the page: a frame may hang onto
    // the pasteboard.
    for (const auto& p : pl) {
        const auto own = d.frames_on(p.id);
        for (auto it = own.rbegin(); it != own.rend(); ++it) {   // topmost first
            const Rect r = frame_in_layout(d, pl, *it);
            if (Rect{r.x - tol, r.y - tol, r.w + 2 * tol, r.h + 2 * tol}.contains(x, y)) return *it;
        }
    }
    return {};
}

bool master_item_on(const Doc& d, const Doc::Id& fid, const Doc::Id& page) {
    const auto* f = d.as<Frame>(fid);
    if (!f || !d.as<Page>(page)) return true;
    if (f->side != Side::Any && d.side_of(page) != Side::Any && f->side != d.side_of(page)) return false;
    for (const auto& own : d.frames_on(page))
        if (d.as<Frame>(own)->overrides == fid) return false;
    return true;
}

Doc::Id hit_master_item(const Doc& d, const std::vector<Placed>& pl, double x, double y, double tol, Doc::Id* page) {
    for (const auto& p : pl) {
        if (!d.as<Page>(p.id)) continue;
        auto chain = master_chain(d, p.id);
        for (auto m = chain.rbegin(); m != chain.rend(); ++m) {   // nearest master first
            const auto items = d.frames_on(*m);
            for (auto it = items.rbegin(); it != items.rend(); ++it) {
                if (!master_item_on(d, *it, p.id)) continue;
                const auto& f = *d.as<Frame>(*it);
                const Rect r{p.r.x + f.x, p.r.y + f.y, f.w, f.h};
                if (Rect{r.x - tol, r.y - tol, r.w + 2 * tol, r.h + 2 * tol}.contains(x, y)) {
                    if (page) *page = p.id;
                    return *it;
                }
            }
        }
    }
    return {};
}

std::vector<Doc::Id> frames_in(const Doc& d, const std::vector<Placed>& pl, const Rect& r) {
    std::vector<Doc::Id> out;
    for (const auto& p : pl)
        for (const auto& f : d.frames_on(p.id))
            if (frame_in_layout(d, pl, f).intersects(r)) out.push_back(f);
    return out;
}

std::pair<int, int> thread_place(const Doc& d, const Doc::Id& id) {
    const auto* f = d.as<Frame>(id);
    if (!f || f->kind != FrameKind::Text || f->story.empty()) return {0, 0};
    // The head: the frame of this story nobody points to.
    std::vector<Doc::Id> mine;
    for (const auto& o : d.ids_of<Frame>())
        if (d.as<Frame>(o)->story == f->story) mine.push_back(o);
    Doc::Id head;
    for (const auto& o : mine) {
        bool pointed = false;
        for (const auto& q : mine) pointed = pointed || d.as<Frame>(q)->next == o;
        if (!pointed) { head = o; break; }
    }
    int at = 0, n = 0;
    for (Doc::Id k = head; !k.empty() && n <= static_cast<int>(mine.size()); k = d.as<Frame>(k)->next) {
        ++n;
        if (k == id) at = n;
        if (!d.as<Frame>(k)) break;
    }
    return {at, n};
}

}  // namespace composix::core

namespace composix::core {

std::vector<Doc::Id> guides_on(const Doc& d, const Doc::Id& owner) {
    std::vector<Doc::Id> out;
    for (const auto& id : d.ids_of<Guide>())
        if (d.as<Guide>(id)->owner == owner) out.push_back(id);
    return out;
}

Doc::Id hit_guide(const Doc& d, const std::vector<Placed>& pl, double x, double y, double tol) {
    for (const auto& p : pl) {
        if (!p.r.contains(x, y)) continue;
        for (const auto& g : guides_on(d, p.id)) {
            const auto& gd = *d.as<Guide>(g);
            const double at = gd.vertical ? p.r.x + gd.pos : p.r.y + gd.pos;
            if (std::abs((gd.vertical ? x : y) - at) <= tol) return g;
        }
    }
    return {};
}

Doc::Id page_at(const std::vector<Placed>& pl, double x, double y) {
    for (const auto& p : pl)
        if (p.r.contains(x, y)) return p.id;
    return {};
}

namespace {
const Master* master_of(const Doc& d, const Doc::Id& id) {
    if (const auto* m = d.as<Master>(id)) return m;
    if (const auto* p = d.as<Page>(id)) return d.as<Master>(p->master);
    return nullptr;
}
}  // namespace

Rect margin_rect(const Doc& d, const Doc::Id& id) {
    const auto* m = master_of(d, id);
    if (!m) return {};
    double w = m->w, h = m->h;
    if (const auto* p = d.as<Page>(id)) { w = p->w; h = p->h; }
    const auto& mg = m->margins;
    // A left-hand page: the spine is on its right.
    const bool left = d.as<Page>(id) && d.side_of(id) == Side::Left;
    const double l = left ? mg.outside : mg.inside, r = left ? mg.inside : mg.outside;
    if (w - l - r < 1 || h - mg.top - mg.bottom < 1) return {};
    return {l, mg.top, w - l - r, h - mg.top - mg.bottom};
}

BaselineGrid baseline_grid(const Doc& d, const Doc::Id& id) {
    const auto* m = master_of(d, id);
    if (!m || m->grid < 1) return {};
    return {m->margins.top, m->grid};
}

std::pair<int, double> page_column_setup(const Doc& d, const Doc::Id& id) {
    const auto* m = master_of(d, id);
    if (!m) return {1, 12};
    return {std::clamp(m->columns, 1, 24), std::max(0.0, m->gutter)};
}

std::vector<ColumnSpan> page_columns(const Doc& d, const Doc::Id& id) {
    const Rect r = margin_rect(d, id);
    if (r.w <= 0) return {};
    const auto [n, g] = page_column_setup(d, id);
    const double cw = (r.w - g * (n - 1)) / n;
    if (cw < 1) return {{r.x, r.r()}};
    std::vector<ColumnSpan> out;
    for (int c = 0; c < n; ++c) out.push_back({r.x + c * (cw + g), r.x + c * (cw + g) + cw});
    return out;
}

SnapTargets snap_targets(const Doc& d, const std::vector<Placed>& pl, const std::vector<Doc::Id>& exclude) {
    SnapTargets t;
    const auto skip = [&](const Doc::Id& id) { return std::find(exclude.begin(), exclude.end(), id) != exclude.end(); };
    for (const auto& p : pl) {
        t.xs.insert(t.xs.end(), {p.r.x, p.r.cx(), p.r.r()});
        t.ys.insert(t.ys.end(), {p.r.y, p.r.cy(), p.r.b()});
        const std::vector<double>* folds = nullptr;
        if (const auto* pg = d.as<Page>(p.id)) folds = &pg->folds;
        else if (const auto* m = d.as<Master>(p.id)) folds = &m->folds;
        if (folds) for (double f : *folds) t.xs.push_back(p.r.x + f);
        // s014: margins and the page's column edges.
        if (const Rect mr = margin_rect(d, p.id); mr.w > 0) {
            t.ys.insert(t.ys.end(), {p.r.y + mr.y, p.r.y + mr.b()});
            for (const auto& c : page_columns(d, p.id)) t.xs.insert(t.xs.end(), {p.r.x + c.x0, p.r.x + c.x1});
        }
        auto owners = p.master ? std::vector<Doc::Id>{} : master_chain(d, p.id);
        owners.push_back(p.id);
        for (const auto& o : owners) {
            for (const auto& g : guides_on(d, o)) {
                const auto& gd = *d.as<Guide>(g);
                (gd.vertical ? t.xs : t.ys).push_back((gd.vertical ? p.r.x : p.r.y) + gd.pos);
            }
            for (const auto& f : d.frames_on(o)) {
                if (skip(f)) continue;
                const auto& fr = *d.as<Frame>(f);
                const Rect r{p.r.x + fr.x, p.r.y + fr.y, fr.w, fr.h};
                t.xs.insert(t.xs.end(), {r.x, r.cx(), r.r()});
                t.ys.insert(t.ys.end(), {r.y, r.cy(), r.b()});
            }
        }
    }
    return t;
}

SnapHit snap_any(const std::vector<double>& vs, const std::vector<double>& targets, double tol) {
    SnapHit best;
    double gap = tol;
    for (double v : vs)
        for (double t : targets) {
            const double dd = t - v;
            if (std::abs(dd) <= gap) {
                gap = std::abs(dd);
                best.delta = dd;
                best.line = t;
            }
        }
    return best;
}

}  // namespace composix::core
