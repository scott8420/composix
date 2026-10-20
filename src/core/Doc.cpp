#include "core/Doc.hpp"
#include "core/Swatches.hpp"

#include <map>
#include <algorithm>
#include <memory>
#include <atomic>
#include <mutex>
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
    m_log.emplace_back(m_rev, id);   // s105
    if (m_log.size() > 4096) {
        m_log_floor = m_log.front().first;
        m_log.pop_front();
    }
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

bool Doc::changes_since(std::uint64_t rev, std::vector<Id>& ids) const {   // s105
    ids.clear();
    if (rev > m_rev || rev < m_log_floor) return false;
    if (rev == m_rev) return true;
    // The first write after `rev` must still be in the log.
    if (m_log.empty() || m_log.front().first > rev + 1) return false;
    for (auto it = m_log.rbegin(); it != m_log.rend() && it->first > rev; ++it) ids.push_back(it->second);
    std::reverse(ids.begin(), ids.end());
    return true;
}

void Doc::reset() {
    m_recs.clear();
    m_next.clear();
    ++m_rev;
    m_log.clear();
    m_log_floor = m_rev;
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
    // s105: kept until the revision moves.
    static const std::size_t k = Record(std::in_place_type<T>).index();
    if (m_ids_rev[k] == m_rev) return m_ids[k];
    std::vector<Id> out;
    for (const auto& [id, r] : m_recs)
        if (std::holds_alternative<T>(r)) out.push_back(id);
    if constexpr (std::is_same_v<T, Section>) {
        std::stable_sort(out.begin(), out.end(), [this](const Id& a, const Id& b) {
            return page_index(as<Section>(a)->start_page) < page_index(as<Section>(b)->start_page);
        });
    } else if constexpr (requires(const T& r) { r.pos; }) {
        std::stable_sort(out.begin(), out.end(), [this](const Id& a, const Id& b) {
            return pos_of(*as<T>(a)) < pos_of(*as<T>(b));
        });
    }
    // s101: no pos (frames, guides): id order as it stands -- the stable sort
    // on an all-equal key changed nothing and cost two lookups a comparison.
    m_ids[k] = out;
    m_ids_rev[k] = m_rev;
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
    if (m_page_at_rev != m_rev) {   // s101: (pos, id) order, as pages() gives it
        m_page_at.clear();
        int n = 0;
        for (const auto& id : pages()) m_page_at[id] = n++;
        m_page_at_rev = m_rev;
    }
    const auto it = m_page_at.find(page);
    return it == m_page_at.end() ? -1 : it->second;
}

Doc::Id Doc::section_of(const Id& page) const {
    if (m_section_at_rev != m_rev) {   // s105: every page's, in one pass
        m_section_at.clear();
        const auto ps = pages();
        std::vector<int> starts(ps.size(), -1);   // page index -> the section starting there (index in secs)
        const auto secs = ids_of<Section>();
        for (std::size_t i = 0; i < secs.size(); ++i) {
            const int s = page_index(as<Section>(secs[i])->start_page);
            if (s >= 0 && starts[static_cast<std::size_t>(s)] < 0) starts[static_cast<std::size_t>(s)] = static_cast<int>(i);
        }
        Id cur;
        for (std::size_t i = 0; i < ps.size(); ++i) {
            if (starts[i] >= 0) cur = secs[static_cast<std::size_t>(starts[i])];
            m_section_at[ps[i]] = cur;
        }
        m_section_at_rev = m_rev;
    }
    const auto it = m_section_at.find(page);
    return it == m_section_at.end() ? Id{} : it->second;
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
    // s147: whether it faces is its master's (no master: the document's).
    const auto* pg = as<Page>(page);
    const auto* m = pg ? as<Master>(pg->master) : nullptr;
    if (m ? !m->facing : !settings().facing) return Side::Any;
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
    if (m_frames_on_rev != m_rev) {   // s105: every owner's, in one pass
        m_frames_on.clear();
        for (const auto& [id, r] : m_recs)   // s101: straight from the store (ids_of's order)
            if (const auto* f = std::get_if<Frame>(&r)) m_frames_on[f->owner].push_back(id);
        for (auto& [o, ids] : m_frames_on)
            std::stable_sort(ids.begin(), ids.end(),
                             [this](const Id& a, const Id& b) { return as<Frame>(a)->z < as<Frame>(b)->z; });
        m_frames_on_rev = m_rev;
    }
    const auto it = m_frames_on.find(owner);
    return it == m_frames_on.end() ? std::vector<Id>{} : it->second;
}

// ─────────────────────────────────────────────────────────────────────────────
// Display helpers
// ─────────────────────────────────────────────────────────────────────────────
std::string note_label(int n, NoteStyle how) {   // s111
    if (n <= 0) return std::to_string(n);
    switch (how) {
    case NoteStyle::Arabic:     return std::to_string(n);
    case NoteStyle::RomanLower: return format_folio(n, Numbering::RomanLower);
    case NoteStyle::AlphaLower: return format_folio(n, Numbering::AlphaLower);
    case NoteStyle::Symbols: {
        static const char* sym[] = {"*", "\u2020", "\u2021", "\u00a7", "\u2016", "\u00b6"};
        std::string out;
        const int times = (n - 1) / 6 + 1;
        for (int i = 0; i < times; ++i) out += sym[(n - 1) % 6];
        return out;
    }
    }
    return std::to_string(n);
}

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

std::vector<ByteSpan> no_break_spans(const Doc& d, const Para& p) {   // s075
    std::vector<ByteSpan> out;
    for (const auto& r : p.runs) {
        const auto* st = d.as<Style>(r.style);   // (a based-on style's settings are written through)
        // s159: set on the words themselves -- over the character style's.
        const bool nb = r.local.no_break >= 0 ? r.local.no_break == 1 : (st && st->no_break);
        if (!nb || r.end <= r.start) continue;
        if (!out.empty() && r.start <= out.back().b) out.back().b = std::max(out.back().b, r.end);
        else out.push_back({r.start, r.end});
    }
    return out;
}

int keep_together(const std::string& text, const std::vector<ByteSpan>& spans, int pos, int took) {
    const int end = pos + took;
    for (const auto& s : spans) {
        if (!(s.a < end && end < s.b)) continue;   // the break isn't inside this one
        if (s.a <= pos) return took;                // it starts the line: too long for any -- as usual
        int at = s.a;
        while (at > pos && text[static_cast<std::size_t>(at - 1)] != ' ') --at;   // back to a word's start
        return at > pos ? at - pos : s.a - pos;
    }
    return took;
}

std::optional<double> parse_percent(std::string_view text) {   // s071
    std::string t;
    for (char c : text) if (!std::isspace(static_cast<unsigned char>(c))) t += c;
    if (!t.empty() && t.back() == '%') t.pop_back();
    if (t.empty()) return std::nullopt;
    std::size_t i = 0;
    if (t[i] == '+') ++i;
    bool digits = false, dot = false;
    for (std::size_t k = i; k < t.size(); ++k) {
        if (std::isdigit(static_cast<unsigned char>(t[k]))) digits = true;
        else if (t[k] == '.' && !dot) dot = true;
        else return std::nullopt;
    }
    if (!digits) return std::nullopt;
    return std::stod(t.substr(i));
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

double unit_pt(Units u) {   // s073
    switch (u) {
    case Units::Millimetres: return 72.0 / 25.4;
    case Units::Picas:       return 12;
    case Units::Points:      return 1;
    default:                 return kPt;
    }
}
std::string unit_name(Units u) {
    switch (u) {
    case Units::Millimetres: return "mm";
    case Units::Picas:       return "picas";
    case Units::Points:      return "pt";
    default:                 return "in";
    }
}
namespace {
std::string trimmed(double v, int places) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.*f", places, v);
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    return s;
}
}  // namespace
std::string length_in(double pt, Units u) {
    switch (u) {
    case Units::Millimetres: return trimmed(pt / unit_pt(u), 2);
    case Units::Points:      return trimmed(pt, 2);
    case Units::Picas: {   // "4p6": picas and points
        const bool neg = pt < 0;
        const double a = std::abs(pt);
        double p = std::floor(a / 12 + 1e-9);
        double rest = a - p * 12;
        if (rest > 12 - 0.005) { p += 1; rest = 0; }
        std::string s = (neg ? "-" : "") + trimmed(p, 0) + "p";
        if (rest > 0.005) s += trimmed(rest, 2);
        return s;
    }
    default:                 return inches(pt);
    }
}
std::string length_text(double pt, Units u) {
    const auto n = length_in(pt, u);
    return u == Units::Picas ? n : n + " " + unit_name(u);
}

std::string size_text(double w, double h, std::size_t folds, Units u) {
    std::string s = u == Units::Picas ? length_in(w, u) + " × " + length_in(h, u)
                                      : length_in(w, u) + " × " + length_in(h, u) + " " + unit_name(u);
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
    if ((a.px_w <= 0 || a.px_h <= 0) && a.vec_w > 0 && a.vec_h > 0) return {a.vec_w, a.vec_h};   // s188: a vector, its own size
    if (a.px_w <= 0 || a.px_h <= 0) return {2 * kPt, 2 * kPt};
    const double per = kPt / 300.0;
    return {a.px_w * per, a.px_h * per};
}

Aspect picture_aspect(const Asset& a) {   // s188
    if (a.px_w > 0 && a.px_h > 0) return {static_cast<double>(a.px_w), static_cast<double>(a.px_h)};
    if (a.vec_w > 0 && a.vec_h > 0) return {a.vec_w, a.vec_h};
    return {};
}

double picture_scale(const Frame& f, const Asset& a) {
    const auto asp = picture_aspect(a);   // s188
    const double iw = asp.w, ih = asp.h;
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
        const double per = f.vector ? 72.0 / 96.0 : kPt / 300.0;   // s188: a vector's size is CSS pixels, 96 to the inch (was taken as points)
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
        {"Trim (" + size_text(s.trim_w, s.trim_h, 0, s.units) + ")", s.trim_w, s.trim_h, {}},
        {"2-panel foldout (" + size_text(2 * s.trim_w - tuck, s.trim_h, 1, s.units) + ")",
         2 * s.trim_w - tuck, s.trim_h, {s.trim_w}},
        {"3-panel foldout (" + size_text(3 * s.trim_w - 2 * tuck, s.trim_h, 2, s.units) + ")",
         3 * s.trim_w - 2 * tuck, s.trim_h, {s.trim_w, 2 * s.trim_w}},
    };
}

std::vector<SizePreset> size_presets(const Doc& d, const Doc::Id& page) {   // s148
    Settings s = d.settings();
    if (const auto* p = d.as<Page>(page))
        if (const auto* m = d.as<Master>(p->master)) { s.trim_w = m->w; s.trim_h = m->h; }
    return size_presets(s);
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
            if (const auto* m = d.as<Master>(master)) { pg.w = m->w; pg.h = m->h; }   // s148: its master's size
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

bool edit_page_margins(DocJournal& j, Doc& d, const std::vector<Doc::Id>& ids, const std::string& label,
                       const std::function<void(Margins&)>& f) {
    return edit_pages(j, d, ids, label, [&](Page& p) {
        if (!p.margins) {   // from what it uses now
            const Master* m = nullptr;
            for (const auto& mid : d.ids_of<Master>()) if (mid == p.master) m = d.as<Master>(mid);
            p.margins = m ? m->margins : Margins{};
        }
        f(*p.margins);
    });
}

bool reset_page_margins(DocJournal& j, Doc& d, const std::vector<Doc::Id>& ids) {
    return edit_pages(j, d, ids, "Margins from the master", [](Page& p) { p.margins.reset(); });
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
                  int columns, double gutter, bool across, ShapeKind shape) {
    if (!d.as<Page>(owner) && !d.as<Master>(owner)) return {};
    if (r.w < 1 || r.h < 1) return {};
    static const char* what[] = {"text frame", "image frame", "shape"};
    const bool oval = kind == FrameKind::Shape && shape == ShapeKind::Oval;   // s197
    Doc::Id made;
    j.run(oval ? std::string("Draw oval") : std::string("Draw ") + what[static_cast<int>(kind)], {}, [&] {
        std::vector<Doc::Id> ids;
        Frame f;
        f.owner = owner;
        f.kind = kind;
        f.x = r.x; f.y = r.y; f.w = r.w; f.h = r.h;
        if (const auto* m = d.as<Master>(owner)) f.across = across && m->facing && m->folds.empty();   // s155
        if (kind == FrameKind::Text) { f.columns = std::max(1, columns); f.gutter = gutter; }
        if (kind == FrameKind::Shape)   // s016: a new shape is outlined in black, so it prints
            for (const auto& sw : d.ids_of<Swatch>())
                if (d.as<Swatch>(sw)->name == "Black") { f.stroke = sw; break; }
        if (oval) f.shape = ShapeKind::Oval;
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

// ── s197: shapes ────────────────────────────────────────────────────────────
namespace {
constexpr double kKappa = 0.5522847498307936;   // a quarter circle's handles, as a fraction of its radius
// A path point's coordinates in the box (and back).
PathPoint to_abs(const PathPoint& p, const Frame& f, double dx, double dy) {
    PathPoint q = p;
    const double X = f.x + dx, Y = f.y + dy;
    q.x = X + p.x * f.w;   q.y = Y + p.y * f.h;
    q.ix = X + p.ix * f.w; q.iy = Y + p.iy * f.h;
    q.ox = X + p.ox * f.w; q.oy = Y + p.oy * f.h;
    return q;
}
bool straight(const PathPoint& a, const PathPoint& b) {
    return a.ox == a.x && a.oy == a.y && b.ix == b.x && b.iy == b.y;
}
XY bez(XY p0, XY p1, XY p2, XY p3, double t) {
    const double u = 1 - t;
    const double a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, e = t * t * t;
    return {a * p0.x + b * p1.x + c * p2.x + e * p3.x, a * p0.y + b * p1.y + c * p2.y + e * p3.y};
}
// The ops for absolute points (a path or an oval's four).
std::vector<OutlineOp> ops_of(const std::vector<PathPoint>& P, bool open, const std::vector<int>& subs = {}) {
    std::vector<OutlineOp> out;
    if (P.empty()) return out;
    for (const auto r : sub_ranges(static_cast<int>(P.size()), subs)) {   // s211: each subpath
        const int n = r.b - r.a;
        if (n < 1) continue;
        out.push_back({'M', {P[static_cast<std::size_t>(r.a)].x, P[static_cast<std::size_t>(r.a)].y}});
        const int segs = open ? n - 1 : n;
        for (int i = 0; i < segs; ++i) {
            const auto& a = P[static_cast<std::size_t>(r.a + i)];
            const auto& b = P[static_cast<std::size_t>(r.a + (i + 1) % n)];
            if (straight(a, b)) out.push_back({'L', {b.x, b.y}});
            else                out.push_back({'C', {a.ox, a.oy, b.ix, b.iy, b.x, b.y}});
        }
        if (!open) out.push_back({'Z', {}});
    }
    return out;
}
std::vector<PathPoint> oval_points(const Frame& f, double dx, double dy) {
    const double cx = f.x + dx + f.w / 2, cy = f.y + dy + f.h / 2, rx = f.w / 2, ry = f.h / 2;
    const double kx = rx * kKappa, ky = ry * kKappa;
    // top, right, bottom, left -- clockwise on the page (y down)
    return {
        {cx, cy - ry, cx - kx, cy - ry, cx + kx, cy - ry, true},
        {cx + rx, cy, cx + rx, cy - ky, cx + rx, cy + ky, true},
        {cx, cy + ry, cx + kx, cy + ry, cx - kx, cy + ry, true},
        {cx - rx, cy, cx - rx, cy + ky, cx - rx, cy - ky, true},
    };
}
}  // namespace

bool has_outline(const Frame& f) {   // s209: any frame -- a picture cut to an oval, text flowing in a star
    return f.shape == ShapeKind::Oval || (f.shape == ShapeKind::Path && f.path.size() >= 2);
}

std::vector<PathPoint> path_points(const Frame& f) {
    std::vector<PathPoint> out;
    out.reserve(f.path.size());
    for (const auto& p : f.path) out.push_back(to_abs(p, f, 0, 0));
    return out;
}

std::vector<OutlineOp> outline_ops(const Frame& f, double dx, double dy) {
    if (has_outline(f)) {
        if (f.shape == ShapeKind::Oval) return ops_of(oval_points(f, dx, dy), false);
        std::vector<PathPoint> P;
        for (const auto& p : f.path) P.push_back(to_abs(p, f, dx, dy));
        return ops_of(P, f.open, f.subs);
    }
    const double x = f.x + dx, y = f.y + dy;
    if (const double r = std::min({f.radius, f.w / 2, f.h / 2}); r > 0) {   // s205: rounded corners
        const double k = r * (1 - kKappa), X = x + f.w, Y = y + f.h;
        return {{'M', {x + r, y}}, {'L', {X - r, y}}, {'C', {X - k, y, X, y + k, X, y + r}},
                {'L', {X, Y - r}}, {'C', {X, Y - k, X - k, Y, X - r, Y}},
                {'L', {x + r, Y}}, {'C', {x + k, Y, x, Y - k, x, Y - r}},
                {'L', {x, y + r}}, {'C', {x, y + k, x + k, y, x + r, y}}, {'Z', {}}};
    }
    return {{'M', {x, y}}, {'L', {x + f.w, y}}, {'L', {x + f.w, y + f.h}}, {'L', {x, y + f.h}}, {'Z', {}}};
}

std::vector<std::vector<XY>> outline_polys(const Frame& f, double tol) {
    std::vector<std::vector<XY>> out;
    XY cur{};
    for (const auto& o : outline_ops(f)) {
        switch (o.op) {
        case 'M': out.push_back({XY{o.v[0], o.v[1]}}); cur = {o.v[0], o.v[1]}; break;
        case 'L': if (!out.empty()) out.back().push_back({o.v[0], o.v[1]}); cur = {o.v[0], o.v[1]}; break;
        case 'C': {
            if (out.empty()) break;
            const XY p1{o.v[0], o.v[1]}, p2{o.v[2], o.v[3]}, p3{o.v[4], o.v[5]};
            // Steps from the control polygon's length: each chord within tol.
            const double len = std::hypot(p1.x - cur.x, p1.y - cur.y) + std::hypot(p2.x - p1.x, p2.y - p1.y) +
                               std::hypot(p3.x - p2.x, p3.y - p2.y);
            const int n = std::clamp(static_cast<int>(std::ceil(std::sqrt(len / std::max(0.01, tol)) * 1.5)), 2, 200);
            for (int k = 1; k <= n; ++k) out.back().push_back(bez(cur, p1, p2, p3, static_cast<double>(k) / n));
            cur = p3;
            break;
        }
        default: break;   // 'Z': a polygon closes itself
        }
    }
    return out;
}

Rect path_bounds(const std::vector<PathPoint>& abs, bool open) {
    if (abs.empty()) return {};
    Frame f;
    f.kind = FrameKind::Shape;
    f.shape = ShapeKind::Path;
    f.w = f.h = 1;   // the identity box: the points stay as they are
    f.path = abs;
    f.open = open;
    double x0 = abs[0].x, y0 = abs[0].y, x1 = x0, y1 = y0;
    for (const auto& poly : outline_polys(f, 0.05))
        for (const auto& q : poly) { x0 = std::min(x0, q.x); y0 = std::min(y0, q.y); x1 = std::max(x1, q.x); y1 = std::max(y1, q.y); }
    return Rect::from_corners(x0, y0, x1, y1);
}

void set_path(Frame& f, const std::vector<PathPoint>& abs, bool open) {   // s209: its kind kept (a shaped picture stays one)
    const Rect b = path_bounds(abs, open);
    f.shape = ShapeKind::Path;
    f.open = open;
    f.x = b.x; f.y = b.y; f.w = b.w; f.h = b.h;
    // A side with no size (a level line): the points sit at 0 across it.
    const auto nx = [&](double v) { return b.w > 1e-9 ? (v - b.x) / b.w : 0.0; };
    const auto ny = [&](double v) { return b.h > 1e-9 ? (v - b.y) / b.h : 0.0; };
    f.path.clear();
    for (const auto& p : abs)
        f.path.push_back({nx(p.x), ny(p.y), nx(p.ix), ny(p.iy), nx(p.ox), ny(p.oy), p.smooth});
}

PathPoint corner_point(double x, double y) { return {x, y, x, y, x, y, false}; }

Doc::Id add_path(DocJournal& j, Doc& d, const Doc::Id& owner, const std::vector<PathPoint>& abs, bool open, bool across,
                 const std::string& label) {
    if (!d.as<Page>(owner) && !d.as<Master>(owner)) return {};
    if (abs.size() < 2) return {};
    const Rect b = path_bounds(abs, open);
    if (b.w < 1 && b.h < 1) return {};
    bool line = open && abs.size() == 2;
    for (const auto& p : abs) if (!(p.ix == p.x && p.iy == p.y && p.ox == p.x && p.oy == p.y)) line = false;
    Doc::Id made;
    j.run(!label.empty() ? label : line ? "Draw line" : "Draw path", {}, [&] {
        Frame f;
        f.owner = owner;
        f.kind = FrameKind::Shape;
        set_path(f, abs, open);
        if (const auto* m = d.as<Master>(owner)) f.across = across && m->facing && m->folds.empty();
        for (const auto& sw : d.ids_of<Swatch>())
            if (d.as<Swatch>(sw)->name == "Black") { f.stroke = sw; break; }
        double z = 0;
        for (const auto& o : d.frames_on(owner)) z = std::max(z, d.as<Frame>(o)->z);
        f.z = z + 1;
        made = d.fresh_id("frame");
        d.put(made, f);
        return std::vector<Doc::Id>{made};
    });
    return made;
}

// ── s204: the stroke's style ────────────────────────────────────────────────
namespace {
// An open path's end k (0 its first point, 1 its last): the point, and the
// way the line leaves it (a unit vector pointing back along the path).
bool path_end(const Frame& f, int k, XY& at, XY& back, double dx, double dy) {
    if (f.shape != ShapeKind::Path || !f.open || f.path.size() < 2) return false;
    std::vector<PathPoint> P;
    for (const auto& p : f.path) P.push_back(to_abs(p, f, dx, dy));
    const auto& e = k == 0 ? P.front() : P.back();
    const auto& n = k == 0 ? P[1] : P[P.size() - 2];
    // The handle on this side if it has one, else the neighbour.
    XY toward = k == 0 ? XY{e.ox, e.oy} : XY{e.ix, e.iy};
    if (std::hypot(toward.x - e.x, toward.y - e.y) < 1e-9) toward = k == 0 ? XY{n.ix, n.iy} : XY{n.ox, n.oy};
    if (std::hypot(toward.x - e.x, toward.y - e.y) < 1e-9) toward = {n.x, n.y};
    const double len = std::hypot(toward.x - e.x, toward.y - e.y);
    if (len < 1e-9) return false;
    at = {e.x, e.y};
    back = {(toward.x - e.x) / len, (toward.y - e.y) / len};
    return true;
}
double end_len(const Frame& f) { return std::max(f.stroke_w, 0.25) * 4.5 + 2; }
double end_wid(const Frame& f) { return std::max(f.stroke_w, 0.25) * 4 + 2; }
}  // namespace

std::vector<OutlineOp> stroke_ops(const Frame& f, double dx, double dy) {
    auto ops = outline_ops(f, dx, dy);
    if (ops.size() < 2 || !(f.shape == ShapeKind::Path && f.open)) return ops;
    // Under an arrow: the line stops short of the tip (the arrow covers the rest).
    for (int k = 0; k < 2; ++k) {
        const int kind = k == 0 ? f.end_a : f.end_b;
        if (kind != 1) continue;
        XY at, back;
        if (!path_end(f, k, at, back, dx, dy)) continue;
        const double cut = end_len(f) * 0.6;
        const double mx = back.x * cut, my = back.y * cut;
        if (k == 0) {
            ops[0].v[0] += mx; ops[0].v[1] += my;
            if (ops[1].op == 'C') { ops[1].v[0] += mx; ops[1].v[1] += my; }
        } else {
            auto& o = ops.back();
            if (o.op == 'L') { o.v[0] += mx; o.v[1] += my; }
            else if (o.op == 'C') { o.v[2] += mx; o.v[3] += my; o.v[4] += mx; o.v[5] += my; }
        }
    }
    return ops;
}

std::vector<double> dash_pattern(const Frame& f) {
    const double w = std::max(f.stroke_w, 0.25);
    if (f.dash == 1) return {w * 3, w * 2};
    if (f.dash == 2) return {0, w * 2};   // dots: round ends on nothing
    return {};
}

std::vector<std::vector<XY>> end_shapes(const Frame& f, double dx, double dy) {
    std::vector<std::vector<XY>> out;
    for (int k = 0; k < 2; ++k) {
        const int kind = k == 0 ? f.end_a : f.end_b;
        XY at, back;
        if (kind <= 0 || !path_end(f, k, at, back, dx, dy)) continue;
        const XY side{-back.y, back.x};
        const double L = end_len(f), W = end_wid(f) / 2, w = std::max(f.stroke_w, 0.25);
        if (kind == 1) {   // an arrow, its tip on the end
            out.push_back({at, {at.x + back.x * L + side.x * W, at.y + back.y * L + side.y * W},
                           {at.x + back.x * L - side.x * W, at.y + back.y * L - side.y * W}});
        } else if (kind == 2) {   // a circle on the end
            std::vector<XY> c;
            const double r = W * 0.8;
            for (int i = 0; i < 32; ++i) {
                const double a = i * 2 * M_PI / 32;
                c.push_back({at.x + r * std::cos(a), at.y + r * std::sin(a)});
            }
            out.push_back(c);
        } else if (kind == 3) {   // a bar across the end
            const double h = w * 0.75;
            out.push_back({{at.x + side.x * W - back.x * h, at.y + side.y * W - back.y * h},
                           {at.x + side.x * W + back.x * h, at.y + side.y * W + back.y * h},
                           {at.x - side.x * W + back.x * h, at.y - side.y * W + back.y * h},
                           {at.x - side.x * W - back.x * h, at.y - side.y * W - back.y * h}});
        }
    }
    return out;
}

bool shape_hit(const Frame& f, double x, double y, double tol) {   // s203
    if (!Rect{f.x - tol - f.stroke_w, f.y - tol - f.stroke_w, f.w + 2 * (tol + f.stroke_w), f.h + 2 * (tol + f.stroke_w)}.contains(x, y))
        return false;
    if (!has_outline(f)) return true;
    const double near = std::max(tol, 1.0) + (f.stroke.empty() ? 0.0 : f.stroke_w / 2);
    const bool open = f.shape == ShapeKind::Path && f.open;
    bool inside = false;
    for (const auto& poly : outline_polys(f, 0.5)) {
        const std::size_t n = poly.size();
        for (std::size_t i = 0; i + 1 < n || (!open && i < n && n > 2); ++i) {
            const XY a = poly[i], b = poly[(i + 1) % n];
            // near this edge?
            const double dx = b.x - a.x, dy = b.y - a.y, len2 = dx * dx + dy * dy;
            const double t = len2 > 0 ? std::clamp(((x - a.x) * dx + (y - a.y) * dy) / len2, 0.0, 1.0) : 0.0;
            if (std::hypot(x - (a.x + dx * t), y - (a.y + dy * t)) <= near) return true;
            // crossing count (even-odd), for a closed shape's inside
            if (!open && ((a.y > y) != (b.y > y)) && x < a.x + (y - a.y) * dx / dy) inside = !inside;
            if (open && i + 2 > n) break;
        }
    }
    return inside;
}

std::vector<PathPoint> polygon_points(double cx, double cy, double r, int sides, double inner, double angle) {   // s206
    sides = std::clamp(sides, 3, 100);
    inner = std::clamp(inner, 0.01, 1.0);
    const bool star = inner < 0.9999;
    const int n = star ? sides * 2 : sides;
    std::vector<PathPoint> out;
    for (int i = 0; i < n; ++i) {
        const double a = angle + i * (star ? M_PI / sides : 2 * M_PI / sides);
        const double rr = star && (i % 2) ? r * inner : r;
        out.push_back(corner_point(cx + rr * std::cos(a), cy + rr * std::sin(a)));
    }
    return out;
}

double perfect_star(int sides) { return sides >= 5 ? std::cos(2 * M_PI / sides) / std::cos(M_PI / sides) : -1.0; }

Doc::Id add_polygon(DocJournal& j, Doc& d, const Doc::Id& owner, double cx, double cy, double r, int sides, double inner,
                    double angle) {
    if (r < 1) return {};
    return add_path(j, d, owner, polygon_points(cx, cy, r, sides, inner, angle), false, false,
                    inner < 0.9999 ? "Draw star" : "Draw polygon");
}

std::vector<SubRange> sub_ranges(int n, const std::vector<int>& subs) {   // s211
    std::vector<SubRange> out;
    int a = 0;
    for (const int k : subs)
        if (k > a && k < n) { out.push_back({a, k}); a = k; }
    if (n > a) out.push_back({a, n});
    return out;
}

Doc::Id make_compound(DocJournal& j, Doc& d, const std::vector<Doc::Id>& frames) {   // s211
    std::vector<Doc::Id> use;
    for (const auto& id : frames) {
        const auto* f = d.as<Frame>(id);
        if (!f || f->kind == FrameKind::Text || (f->shape == ShapeKind::Path && f->open) || f->rot != 0) continue;
        if (!use.empty() && f->owner != d.as<Frame>(use.front())->owner) continue;   // one page's
        use.push_back(id);
    }
    if (use.size() < 2) return {};
    std::vector<PathPoint> all;
    std::vector<int> subs;
    for (const auto& id : use) {
        const auto* f = d.as<Frame>(id);
        bool open = false;
        auto P = shape_points(*f, open);
        if (f->shape == ShapeKind::Path)   // its own subpaths come along
            for (const int k : f->subs) subs.push_back(static_cast<int>(all.size()) + k);
        if (!all.empty()) subs.push_back(static_cast<int>(all.size()));
        all.insert(all.end(), P.begin(), P.end());
    }
    std::sort(subs.begin(), subs.end());
    subs.erase(std::unique(subs.begin(), subs.end()), subs.end());
    const Doc::Id keep = use.front();
    j.run("Make compound path", use, [&] {
        Frame g = *d.as<Frame>(keep);
        g.radius = 0;
        set_path(g, all, false);
        g.subs = subs;
        d.put(keep, g);
        for (std::size_t i = 1; i < use.size(); ++i) d.put(use[i], std::nullopt);
    });
    return keep;
}

std::vector<Doc::Id> release_compound(DocJournal& j, Doc& d, const Doc::Id& id) {   // s211
    const auto* f = d.as<Frame>(id);
    if (!f || f->shape != ShapeKind::Path || f->subs.empty()) return {};
    const auto P = path_points(*f);
    const auto ranges = sub_ranges(static_cast<int>(P.size()), f->subs);
    std::vector<Doc::Id> out{id};
    j.run("Release compound path", {id}, [&]() -> std::vector<Doc::Id> {
        std::vector<Doc::Id> made;
        const Frame base = *d.as<Frame>(id);
        for (std::size_t r = 0; r < ranges.size(); ++r) {
            std::vector<PathPoint> part(P.begin() + ranges[r].a, P.begin() + ranges[r].b);
            Frame g = base;
            g.subs.clear();
            set_path(g, part, base.open);
            if (r == 0) { d.put(id, g); continue; }
            g.z = base.z + 0.001 * static_cast<double>(r);
            const Doc::Id nid = d.fresh_id("frame");
            d.put(nid, g);
            made.push_back(nid);
            out.push_back(nid);
        }
        return made;
    });
    return out;
}

namespace { bool used_by_other(const Doc& d, const Doc::Id& rec, const std::vector<Doc::Id>& but); }   // below

bool set_frame_content(DocJournal& j, Doc& d, const std::vector<Doc::Id>& frames, FrameKind k) {   // s210
    std::vector<Doc::Id> touch;
    for (const auto& id : frames) {
        const auto* f = d.as<Frame>(id);
        if (!f || f->kind == k) continue;
        if (f->kind == FrameKind::Text) {   // only an empty, unthreaded one
            const auto* st = d.as<Story>(f->story);
            bool empty = !st || std::all_of(st->paras.begin(), st->paras.end(), [](const Para& p) { return p.text.empty(); });
            for (const auto& o : d.ids_of<Frame>()) if (o != id && d.as<Frame>(o)->story == f->story && !f->story.empty()) empty = false;
            if (!empty) continue;
            if (!f->story.empty()) touch.push_back(f->story);
        }
        if (f->kind == FrameKind::Image && !f->asset.empty() && !used_by_other(d, f->asset, {id})) touch.push_back(f->asset);
        touch.push_back(id);
    }
    if (touch.empty()) return false;
    static const char* names[] = {"Text", "Picture", "None"};
    return j.run(std::string("Content: ") + names[static_cast<int>(k)], touch, [&]() -> std::vector<Doc::Id> {
        std::vector<Doc::Id> made;
        Doc::Id style;
        for (const auto& s : d.ids_of<Style>())
            if (d.as<Style>(s)->kind == StyleKind::Paragraph) { style = s; break; }
        for (const auto& id : frames) {
            const auto* cur = d.as<Frame>(id);
            if (!cur || cur->kind == k || std::find(touch.begin(), touch.end(), id) == touch.end()) continue;
            Frame f = *cur;
            if (f.kind == FrameKind::Text && !f.story.empty()) d.put(f.story, std::nullopt);
            if (f.kind == FrameKind::Image && !f.asset.empty() && !used_by_other(d, f.asset, {id})) d.put(f.asset, std::nullopt);
            f.story.clear();
            f.next.clear();
            f.asset.clear();
            f.kind = k;
            if (k == FrameKind::Text) {
                const Doc::Id story = d.fresh_id("story");
                d.put(story, Story{"Story " + story.substr(story.find('.') + 1), {{style, ""}}});
                made.push_back(story);
                f.story = story;
            }
            d.put(id, f);
        }
        return made;
    });
}

bool set_shape_kind(DocJournal& j, Doc& d, const std::vector<Doc::Id>& frames, ShapeKind k) {
    if (k == ShapeKind::Path) return false;   // a path is drawn, not chosen
    std::vector<std::pair<Doc::Id, Frame>> plan;
    for (const auto& id : frames) {
        const auto* f = d.as<Frame>(id);
        if (!f || f->shape == ShapeKind::Path || f->shape == k) continue;   // s210: any frame -- a text frame made an oval
        Frame g = *f;
        g.shape = k;
        plan.emplace_back(id, g);
    }
    if (plan.empty()) return false;
    std::vector<Doc::Id> ids;
    for (const auto& [id, g] : plan) ids.push_back(id);
    j.run(k == ShapeKind::Oval ? "Make oval" : "Make rectangle", ids, [&] {
        for (const auto& [id, g] : plan) d.put(id, g);
        return std::vector<Doc::Id>{};
    });
    return true;
}

// ── s200: Direct on a shape's points ───────────────────────────────────────
std::vector<PathPoint> shape_points(const Frame& f, bool& open) {
    open = false;
    // s210: any frame -- a text frame's box, a shaped picture's outline
    if (f.shape == ShapeKind::Path) { open = f.open; return path_points(f); }
    if (f.shape == ShapeKind::Oval) return oval_points(f, 0, 0);
    return {corner_point(f.x, f.y), corner_point(f.x + f.w, f.y), corner_point(f.x + f.w, f.y + f.h), corner_point(f.x, f.y + f.h)};
}

OutlineHit outline_nearest(const Frame& f, double x, double y) {
    OutlineHit best;
    bool open = false;
    const auto P = shape_points(f, open);
    if (P.size() < 2) return best;
    for (const auto r : sub_ranges(static_cast<int>(P.size()), f.shape == ShapeKind::Path ? f.subs : std::vector<int>{})) {   // s211
        const int n = r.b - r.a;
        for (int k = 0; k < (open ? n - 1 : n); ++k) {
            const auto& a = P[static_cast<std::size_t>(r.a + k)];
            const auto& b = P[static_cast<std::size_t>(r.a + (k + 1) % n)];
            constexpr int kSteps = 64;
            for (int i = 0; i <= kSteps; ++i) {
                const double t = static_cast<double>(i) / kSteps;
                const XY q = bez({a.x, a.y}, {a.ox, a.oy}, {b.ix, b.iy}, {b.x, b.y}, t);
                const double dd = std::hypot(q.x - x, q.y - y);
                if (dd < best.dist) best = {r.a + k, t, dd};
            }
        }
    }
    return best;
}

namespace {
// Write a shape's points back as a path (one step).
bool put_path(DocJournal& j, Doc& d, const Doc::Id& id, const std::vector<PathPoint>& P, bool open, const std::string& label,
              const std::vector<int>* subs = nullptr) {
    const auto* f = d.as<Frame>(id);
    if (!f) return false;
    Frame g = *f;
    set_path(g, P, open);
    if (subs) g.subs = *subs;   // s211: the subpaths as they now are
    else if (f->shape != ShapeKind::Path) g.subs.clear();
    if (g == *f) return false;
    return j.run(label, {id}, [&] { d.put(id, g); });
}
}  // namespace

bool path_add_point(DocJournal& j, Doc& d, const Doc::Id& id, int k, double t) {
    const auto* f = d.as<Frame>(id);
    if (!f || t <= 0 || t >= 1) return false;
    bool open = false;
    auto P = shape_points(*f, open);
    const int n = static_cast<int>(P.size());
    std::vector<int> subs = f->shape == ShapeKind::Path ? f->subs : std::vector<int>{};
    SubRange in{-1, -1};   // s211: the subpath k is in -- its segment ends within it
    for (const auto r : sub_ranges(n, subs)) if (k >= r.a && k < r.b) in = r;
    if (in.a < 0 || (open && k == in.b - 1)) return false;
    const int next = k + 1 < in.b ? k + 1 : in.a;
    auto& a = P[static_cast<std::size_t>(k)];
    auto& b = P[static_cast<std::size_t>(next)];
    const auto lerp = [](XY p, XY q, double u) { return XY{p.x + (q.x - p.x) * u, p.y + (q.y - p.y) * u}; };
    const XY p0{a.x, a.y}, p1{a.ox, a.oy}, p2{b.ix, b.iy}, p3{b.x, b.y};
    const XY q0 = lerp(p0, p1, t), q1 = lerp(p1, p2, t), q2 = lerp(p2, p3, t);
    const XY r0 = lerp(q0, q1, t), r1 = lerp(q1, q2, t);
    const XY m = lerp(r0, r1, t);
    const bool curved = !straight(a, b);
    PathPoint mid = corner_point(m.x, m.y);
    if (curved) {
        a.ox = q0.x; a.oy = q0.y;
        b.ix = q2.x; b.iy = q2.y;
        mid.ix = r0.x; mid.iy = r0.y;
        mid.ox = r1.x; mid.oy = r1.y;
        mid.smooth = true;
    }
    P.insert(P.begin() + k + 1, mid);
    for (auto& sidx : subs) if (sidx > k) ++sidx;
    return put_path(j, d, id, P, open, "Add point", &subs);
}

bool path_delete_point(DocJournal& j, Doc& d, const Doc::Id& id, int k) {
    return path_delete_points(j, d, id, {k});
}

bool set_path_points(DocJournal& j, Doc& d, const Doc::Id& id, const std::vector<PathPoint>& abs, bool open,
                     const std::string& label) {   // s208
    const auto* f = d.as<Frame>(id);
    if (!f || abs.size() < 2) return false;
    return put_path(j, d, id, abs, open, label);
}

std::vector<PathPoint> reversed_path(std::vector<PathPoint> P) {   // s208
    std::reverse(P.begin(), P.end());
    for (auto& p : P) { std::swap(p.ix, p.ox); std::swap(p.iy, p.oy); }
    return P;
}

bool path_delete_points(DocJournal& j, Doc& d, const Doc::Id& id, const std::set<int>& ks) {   // s207 (s211: by subpath)
    const auto* f = d.as<Frame>(id);
    if (!f || ks.empty()) return false;
    bool open = false;
    auto P = shape_points(*f, open);
    const int least = open ? 2 : 3;
    std::vector<PathPoint> keep;
    std::vector<int> subs;
    bool any = false;
    for (const auto r : sub_ranges(static_cast<int>(P.size()), f->shape == ShapeKind::Path ? f->subs : std::vector<int>{})) {
        std::vector<PathPoint> part;
        for (int i = r.a; i < r.b; ++i)
            if (!ks.count(i)) part.push_back(P[static_cast<std::size_t>(i)]);
            else any = true;
        if (static_cast<int>(part.size()) < least) continue;   // too few left: that subpath goes (a hole taken out)
        if (!keep.empty()) subs.push_back(static_cast<int>(keep.size()));
        keep.insert(keep.end(), part.begin(), part.end());
    }
    if (!any || static_cast<int>(keep.size()) < least) return false;
    const int gone = static_cast<int>(P.size() - keep.size());
    return put_path(j, d, id, keep, open, gone == 1 ? "Delete point" : "Delete points", &subs);
}

bool path_move_points(DocJournal& j, Doc& d, const Doc::Id& id, const std::set<int>& ks, double dx, double dy,
                      const std::string& merge_key, std::chrono::steady_clock::time_point now) {   // s207
    const auto* f = d.as<Frame>(id);
    if (!f || ks.empty() || (dx == 0 && dy == 0)) return false;
    bool open = false;
    auto P = shape_points(*f, open);
    bool any = false;
    for (const int k : ks)
        if (k >= 0 && k < static_cast<int>(P.size())) {
            auto& p = P[static_cast<std::size_t>(k)];
            p.x += dx; p.ix += dx; p.ox += dx;
            p.y += dy; p.iy += dy; p.oy += dy;
            any = true;
        }
    if (!any) return false;
    Frame g = *f;
    set_path(g, P, open);
    return j.run(ks.size() == 1 ? "Move point" : "Move points", {id}, [&] { d.put(id, g); }, merge_key, now);
}

bool path_toggle_point(DocJournal& j, Doc& d, const Doc::Id& id, int k) {
    const auto* f = d.as<Frame>(id);
    if (!f) return false;
    bool open = false;
    auto P = shape_points(*f, open);
    const int n = static_cast<int>(P.size());
    if (k < 0 || k >= n) return false;
    auto& p = P[static_cast<std::size_t>(k)];
    const bool has = !(p.ix == p.x && p.iy == p.y && p.ox == p.x && p.oy == p.y);
    if (has) {
        p.ix = p.ox = p.x;
        p.iy = p.oy = p.y;
        p.smooth = false;
    } else {
        // Neighbours (an open path's end: itself on that side).
        SubRange in{0, n};   // s211: its neighbours within its own subpath
        for (const auto r : sub_ranges(n, f->shape == ShapeKind::Path ? f->subs : std::vector<int>{})) if (k >= r.a && k < r.b) in = r;
        const int m = in.b - in.a, kk = k - in.a;
        const auto& prev = (kk > 0 || !open) ? P[static_cast<std::size_t>(in.a + (kk - 1 + m) % m)] : p;
        const auto& next = (kk < m - 1 || !open) ? P[static_cast<std::size_t>(in.a + (kk + 1) % m)] : p;
        double dx = next.x - prev.x, dy = next.y - prev.y;
        const double len = std::hypot(dx, dy);
        if (len < 1e-9) return false;
        dx /= len; dy /= len;
        const double din = std::hypot(p.x - prev.x, p.y - prev.y) / 3, dout = std::hypot(next.x - p.x, next.y - p.y) / 3;
        const double wi = din > 0 ? din : dout, wo = dout > 0 ? dout : din;
        p.ix = p.x - dx * wi; p.iy = p.y - dy * wi;
        p.ox = p.x + dx * wo; p.oy = p.y + dy * wo;
        p.smooth = true;
    }
    return put_path(j, d, id, P, open, has ? "Corner point" : "Smooth point");
}

bool set_shows_on(DocJournal& j, Doc& d, const std::vector<Doc::Id>& frames, int choice) {   // s155
    if (choice < 0 || choice > 3) return false;
    std::vector<std::pair<Doc::Id, Frame>> plan;
    for (const auto& id : frames) {
        const auto* f = d.as<Frame>(id);
        const auto* m = f ? d.as<Master>(f->owner) : nullptr;
        if (!m) continue;
        const bool spread = m->facing && m->folds.empty();
        Frame g = *f;
        const bool to_across = choice == 3 && spread;
        if (choice == 3 && !spread) continue;   // no spine to cross
        if (to_across && !g.across && g.side == Side::Right) g.x += m->w;
        if (!to_across && g.across && choice == 1) g.x -= m->w;
        g.across = to_across;
        g.side = to_across ? Side::Any : static_cast<Side>(choice);
        if (!(g == *f)) plan.emplace_back(id, g);
    }
    if (plan.empty()) return false;
    std::vector<Doc::Id> ids;
    for (const auto& pr : plan) ids.push_back(pr.first);
    return j.run("Shows on", ids, [&] {
        for (const auto& [id, g] : plan) d.put(id, g);
    });
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

bool set_title(DocJournal& j, Doc& d, const std::string& title) {
    std::string t = title;
    while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.pop_back();
    while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.erase(t.begin());
    if (t.empty() || t == d.settings().title) return false;
    return j.run("Title \u201c" + t + "\u201d", {"doc"}, [&] {
        Settings s = d.settings();
        s.title = t;
        d.put("doc", s);
    });
}

bool set_trim(DocJournal& j, Doc& d, double w, double h) {
    const Settings old = d.settings();
    if (w < 36 || h < 36 || w > 5000 || h > 5000) return false;
    if (std::abs(w - old.trim_w) < 1e-9 && std::abs(h - old.trim_h) < 1e-9) return false;
    std::vector<Doc::Id> ids{"doc"};
    const auto at_trim = [&](double ph) { return std::abs(ph - old.trim_h) < 0.01; };
    for (const auto& p : d.pages()) if (at_trim(d.as<Page>(p)->h)) ids.push_back(p);
    for (const auto& m : d.ids_of<Master>()) if (at_trim(d.as<Master>(m)->h)) ids.push_back(m);
    const double kx = w / old.trim_w;
    return j.run("Trim size " + inches(w) + " \u00d7 " + inches(h) + " in", ids, [&] {
        Settings s = old;
        s.trim_w = w;
        s.trim_h = h;
        d.put("doc", s);
        for (const auto& id : ids) {
            if (auto* p = d.as<Page>(id)) {
                Page pg = *p;
                pg.w *= kx;
                pg.h = h;
                for (auto& f : pg.folds) f *= kx;
                d.put(id, pg);
            } else if (auto* m = d.as<Master>(id)) {
                Master ms = *m;
                ms.w *= kx;
                ms.h = h;
                for (auto& f : ms.folds) f *= kx;
                d.put(id, ms);
            }
        }
    });
}

bool set_master_size(DocJournal& j, Doc& d, const Doc::Id& master, double w, double h) {   // s147
    const auto* m = d.as<Master>(master);
    if (!m || w < 36 || h < 36 || w > 5000 || h > 5000) return false;
    if (std::abs(w - m->w) < 1e-9 && std::abs(h - m->h) < 1e-9) return false;
    const Master old = *m;
    std::vector<Doc::Id> ids{master};
    for (const auto& p : d.pages())
        if (const auto* pg = d.as<Page>(p); pg->master == master && std::abs(pg->w - old.w) < 0.01 && std::abs(pg->h - old.h) < 0.01)
            ids.push_back(p);
    const double kx = w / old.w;
    return j.run(master_label(d, master) + ": size " + inches(w) + " \u00d7 " + inches(h) + " in", ids, [&] {
        Master ms = old;
        ms.w = w;
        ms.h = h;
        for (auto& f : ms.folds) f *= kx;
        d.put(master, ms);
        for (std::size_t i = 1; i < ids.size(); ++i) {
            Page pg = *d.as<Page>(ids[i]);
            pg.w = w;
            pg.h = h;
            for (auto& f : pg.folds) f *= kx;
            d.put(ids[i], pg);
        }
    });
}

bool set_master_bleed(DocJournal& j, Doc& d, const Doc::Id& master, double bleed) {   // s147
    const auto* m = d.as<Master>(master);
    if (!m || bleed < 0 || bleed > 72 || std::abs(bleed - m->bleed) < 1e-9) return false;
    return j.run(master_label(d, master) + ": bleed " + inches(bleed) + " in", {master}, [&] {
        Master ms = *d.as<Master>(master);
        ms.bleed = bleed;
        d.put(master, ms);
    });
}

bool set_master_facing(DocJournal& j, Doc& d, const Doc::Id& master, bool facing) {   // s147
    const auto* m = d.as<Master>(master);
    if (!m || m->facing == facing) return false;
    return j.run(master_label(d, master) + (facing ? ": facing pages" : ": single pages"), {master}, [&] {
        Master ms = *d.as<Master>(master);
        ms.facing = facing;
        d.put(master, ms);
    });
}

bool set_bleed(DocJournal& j, Doc& d, double bleed) {
    if (bleed < 0 || bleed > 72 || std::abs(bleed - d.settings().bleed) < 1e-9) return false;
    std::vector<Doc::Id> ids{"doc"};   // s147: the document's default, and every master's
    for (const auto& m : d.ids_of<Master>()) ids.push_back(m);
    return j.run("Bleed " + inches(bleed) + " in", ids, [&] {
        Settings s = d.settings();
        s.bleed = bleed;
        d.put("doc", s);
        for (std::size_t i = 1; i < ids.size(); ++i) {
            Master m = *d.as<Master>(ids[i]);
            m.bleed = bleed;
            d.put(ids[i], m);
        }
    });
}

bool set_facing(DocJournal& j, Doc& d, bool facing) {
    if (facing == d.settings().facing) return false;
    std::vector<Doc::Id> ids{"doc"};   // s147: the document's default, and every master's
    for (const auto& m : d.ids_of<Master>()) ids.push_back(m);
    return j.run(facing ? "Facing pages" : "Single pages", ids, [&] {
        Settings s = d.settings();
        s.facing = facing;
        d.put("doc", s);
        for (std::size_t i = 1; i < ids.size(); ++i) {
            Master m = *d.as<Master>(ids[i]);
            m.facing = facing;
            d.put(ids[i], m);
        }
    });
}

bool set_wrap_beneath(DocJournal& j, Doc& d, bool on) {   // s069
    if (on == d.settings().wrap_beneath) return false;
    return j.run("Text wrap affects", {"doc"}, [&] {
        Settings s = d.settings();
        s.wrap_beneath = on;
        d.put("doc", s);
    });
}

bool set_note_numbering(DocJournal& j, Doc& d, NoteRestart restart, NoteStyle style) {   // s111
    if (restart == d.settings().note_restart && style == d.settings().note_style) return false;
    return j.run("Footnote numbering", {"doc"}, [&] {
        Settings s = d.settings();
        s.note_restart = restart;
        s.note_style = style;
        d.put("doc", s);
    });
}

bool set_note_area(DocJournal& j, Doc& d, int percent) {   // s117
    percent = std::clamp(percent, 10, 100);
    if (percent == d.settings().note_area) return false;
    return j.run("Footnote area", {"doc"}, [&] {
        Settings s = d.settings();
        s.note_area = percent;
        d.put("doc", s);
    });
}

bool set_note_look(DocJournal& j, Doc& d, int rule, double rule_w, double space, const std::string& cont) {   // s118
    rule = std::clamp(rule, 0, 100);
    rule_w = std::clamp(rule_w, 0.1, 6.0);
    space = std::clamp(space, 0.0, 36.0);
    std::string c = cont;
    std::replace(c.begin(), c.end(), '\n', ' ');
    const auto& o = d.settings();
    if (rule == o.note_rule && rule_w == o.note_rule_w && space == o.note_space && c == o.note_cont) return false;
    return j.run("Footnote look", {"doc"}, [&] {
        Settings s = d.settings();
        s.note_rule = rule;
        s.note_rule_w = rule_w;
        s.note_space = space;
        s.note_cont = c;
        d.put("doc", s);
    });
}

bool set_note_place(DocJournal& j, Doc& d, NoteAt at, const std::string& title) {   // s120
    std::string t = title;
    std::replace(t.begin(), t.end(), '\n', ' ');
    if (at == d.settings().note_at && t == d.settings().note_title) return false;
    return j.run(at == NoteAt::StoryEnd ? "Endnotes" : "Footnotes", {"doc"}, [&] {
        Settings s = d.settings();
        s.note_at = at;
        s.note_title = t;
        d.put("doc", s);
    });
}

bool set_note_num(DocJournal& j, Doc& d, Settings::NoteNum how) {   // s126
    if (how == d.settings().note_num) return false;
    return j.run("Footnote numbers", {"doc"}, [&] {
        Settings s = d.settings();
        s.note_num = how;
        d.put("doc", s);
    });
}

bool save_query(DocJournal& j, Doc& d, SavedQuery q) {   // s078
    const auto a = q.name.find_first_not_of(" \t"), b = q.name.find_last_not_of(" \t");
    if (a == std::string::npos) return false;
    q.name = q.name.substr(a, b - a + 1);
    Settings s = d.settings();
    auto it = std::find_if(s.queries.begin(), s.queries.end(), [&](const SavedQuery& x) { return x.name == q.name; });
    if (it != s.queries.end()) {
        if (*it == q) return false;
        *it = q;
    } else {
        s.queries.push_back(q);
        std::sort(s.queries.begin(), s.queries.end(), [](const SavedQuery& x, const SavedQuery& y) { return x.name < y.name; });
    }
    return j.run("Save query", {"doc"}, [&] { d.put("doc", s); });
}

bool delete_query(DocJournal& j, Doc& d, const std::string& name) {   // s078
    Settings s = d.settings();
    const auto n = s.queries.size();
    std::erase_if(s.queries, [&](const SavedQuery& x) { return x.name == name; });
    if (s.queries.size() == n) return false;
    return j.run("Delete query", {"doc"}, [&] { d.put("doc", s); });
}

bool set_units(DocJournal& j, Doc& d, Units u) {   // s073
    if (u == d.settings().units) return false;
    return j.run("Units", {"doc"}, [&] {
        Settings s = d.settings();
        s.units = u;
        d.put("doc", s);
    });
}

bool set_ruler_origin(DocJournal& j, Doc& d, RulerOrigin o) {   // s154
    const auto& st = d.settings();
    if (st.ruler_origin == o && st.zero_x == 0 && st.zero_y == 0) return false;
    return j.run("Ruler origin", {"doc"}, [&] {
        Settings s = d.settings();
        s.ruler_origin = o;
        s.zero_x = s.zero_y = 0;   // the zero point back to the origin
        d.put("doc", s);
    });
}

bool set_ruler_zero(DocJournal& j, Doc& d, double x, double y) {   // s154
    const auto& st = d.settings();
    if (st.ruler_origin == RulerOrigin::Spine) return false;
    if (std::abs(st.zero_x - x) < 1e-9 && std::abs(st.zero_y - y) < 1e-9) return false;
    return j.run("Zero point", {"doc"}, [&] {
        Settings s = d.settings();
        s.zero_x = x;
        s.zero_y = y;
        d.put("doc", s);
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

bool set_list_styles(DocJournal& j, Doc& d, const Doc::Id& list, const std::vector<Doc::Id>& styles) {   // s088
    const auto* l = d.as<GenList>(list);
    if (!l) return false;
    std::vector<Doc::Id> v;
    for (const auto& s : styles)
        if (const auto* st = d.as<Style>(s); st && st->kind == StyleKind::Paragraph && std::find(v.begin(), v.end(), s) == v.end())
            v.push_back(s);
    if (v == l->from_styles) return false;
    return j.run("List styles", {list}, [&] {
        GenList nl = *d.as<GenList>(list);
        nl.from_styles = v;
        d.put(list, nl);
    });
}

int list_title_paras(const Story& st) {   // s090
    if (st.paras.empty()) return 0;
    const auto& p0 = st.paras.front();
    if (p0.text.empty() || p0.text.find('\t') != std::string::npos) return 0;
    if (st.paras.size() == 1) return 1;
    for (std::size_t k = 1; k < st.paras.size(); ++k)
        if (st.paras[k].style != p0.style) return 1;
    return 0;
}

bool set_list_entry_style(DocJournal& j, Doc& d, const Doc::Id& list, int level, const Doc::Id& style) {   // s090
    const auto* l = d.as<GenList>(list);
    if (!l || level < 1 || level > 9) return false;
    if (!style.empty()) {
        const auto* s = d.as<Style>(style);
        if (!s || s->kind != StyleKind::Paragraph) return false;
    }
    const auto at = static_cast<std::size_t>(level - 1);
    const std::string now = at < l->entry_styles.size() ? l->entry_styles[at] : std::string{};
    if (now == style) return false;
    return j.run("Entry style", {list}, [&] {
        GenList nl = *d.as<GenList>(list);
        if (nl.entry_styles.size() <= at) nl.entry_styles.resize(at + 1);
        nl.entry_styles[at] = style;
        while (!nl.entry_styles.empty() && nl.entry_styles.back().empty()) nl.entry_styles.pop_back();
        d.put(list, nl);
    });
}

bool set_list_numbers(DocJournal& j, Doc& d, const Doc::Id& list, bool on) {   // s090
    const auto* l = d.as<GenList>(list);
    if (!l || l->numbers == on) return false;
    return j.run(on ? "Page numbers" : "No page numbers", {list}, [&] {
        GenList nl = *d.as<GenList>(list);
        nl.numbers = on;
        d.put(list, nl);
    });
}

Doc::Id add_list(DocJournal& j, Doc& d, ListKind kind, const std::string& name, const Doc::Id& on) {   // s135
    if (!on.empty() && !d.as<Page>(on)) return {};
    double pos = 0;
    for (const auto& id : d.ids_of<GenList>()) pos = std::max(pos, d.as<GenList>(id)->pos);
    const Doc::Id id = d.fresh_id("list");
    GenList l;
    l.name = name;
    l.kind = kind;
    l.pos = pos + 1;
    l.placed_on = on;
    l.built_entries = -1;
    j.run("New " + name, {}, [&]() -> std::vector<Doc::Id> { d.put(id, l); return {id}; });
    return id;
}

namespace {
bool letter_head(const std::string& w) {   // s135: one character -- an index's letter heading
    if (w.empty()) return false;
    const auto c = static_cast<unsigned char>(w[0]);
    const std::size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
    return w.size() == n;
}
}  // namespace

int list_title_paras(const Story& st, ListKind kind) {   // s135
    const int t = list_title_paras(st);
    if (kind != ListKind::Index || t == 0) return t;
    return letter_head(st.paras.front().text) ? 0 : t;
}

Doc::Id list_story(const Doc& d, const Doc::Id& list) {   // s141 (s140's adoption, shared)
    const auto* l = d.as<GenList>(list);
    if (!l) return {};
    if (d.as<Story>(l->story)) return l->story;
    if (l->kind != ListKind::Bibliography || !d.as<Page>(l->placed_on)) return {};
    for (const auto& fid : d.frames_on(l->placed_on)) {
        const auto* f = d.as<Frame>(fid);
        const auto* fs = f && !f->story.empty() ? d.as<Story>(f->story) : nullptr;
        if (!fs || fs->paras.empty()) continue;
        if (std::all_of(fs->paras.begin(), fs->paras.end(), [&](const Para& p) {
                return std::find(l->from_styles.begin(), l->from_styles.end(), p.style) != l->from_styles.end();
            }))
            return f->story;
    }
    return {};
}

bool set_list_cite_order(DocJournal& j, Doc& d, const Doc::Id& list, bool on) {   // s142
    const auto* l = d.as<GenList>(list);
    if (!l || l->kind != ListKind::Bibliography || l->cite_order == on) return false;
    return j.run(on ? "Order: First Cited" : "Order: Alphabetical", {list}, [&] {
        GenList nl = *d.as<GenList>(list);
        nl.cite_order = on;
        d.put(list, nl);
    });
}

bool build_list(DocJournal& j, Doc& d, const Doc::Id& list, const std::vector<ListEntry>& entries) {   // s087
    const auto* l = d.as<GenList>(list);
    if (!l) return false;
    const auto* st = l->story.empty() ? nullptr : d.as<Story>(l->story);
    // s140: a bibliography with no story yet, its page already holding a story all of
    // sources (typed there by hand): that one is it -- sorted where it is, no frame on top.
    Doc::Id adopt;
    if (!st && l->kind == ListKind::Bibliography) {
        adopt = list_story(d, list);
        st = adopt.empty() ? nullptr : d.as<Story>(adopt);
    }
    const auto* pg = l->placed_on.empty() ? nullptr : d.as<Page>(l->placed_on);
    if (!st && !pg) return false;
    // Level 1's style, and the title.
    Doc::Id entry_style;
    std::vector<Para> paras;
    if (st) {   // s090: the title by style (entries may have no tab)
        const auto t = static_cast<std::size_t>(list_title_paras(*st, l->kind));   // s135: an index's letter isn't a title
        if (t < st->paras.size()) {
            entry_style = st->paras[t].style;
        } else if (!st->paras.empty()) {
            entry_style = st->paras.front().style;
        }
        // s135: an index's level 1 -- the first entry that isn't a letter heading.
        // (s140: braces -- the else above had bound to this loop's if since s135.)
        if (l->kind == ListKind::Index)
            for (auto k = t; k < st->paras.size(); ++k)
                if (!letter_head(st->paras[k].text)) {
                    entry_style = st->paras[k].style;
                    break;
                }
        if (t == 1) paras.push_back(st->paras.front());   // the title
    } else {
        for (const auto& s : d.ids_of<Style>())
            if (d.as<Style>(s)->kind == StyleKind::Paragraph) { entry_style = s; break; }
    }
    const auto set_style = [&](int lv) -> std::string {   // the list's own pick for a level, if it's a paragraph style
        if (lv < 1 || lv > static_cast<int>(l->entry_styles.size())) return {};
        const auto& id = l->entry_styles[static_cast<std::size_t>(lv - 1)];
        const auto* s = d.as<Style>(id);
        return s && s->kind == StyleKind::Paragraph ? id : std::string{};
    };
    // s140: a bibliography's entries, unless picked, in the style it gathers -- its own story is
    // one of its sources (built again, the same), so they keep it.
    if (l->kind == ListKind::Bibliography && !l->from_styles.empty())
        if (const auto* g = d.as<Style>(l->from_styles.front()); g && g->kind == StyleKind::Paragraph) entry_style = l->from_styles.front();
    if (const auto own = set_style(1); !own.empty()) entry_style = own;
    // s088: each deeper level's style -- the list's, else by name, else to be made.
    int deepest = 1;
    for (const auto& e : entries) deepest = std::max(deepest, e.level);
    const auto* base = d.as<Style>(entry_style);
    std::vector<Doc::Id> lv_style(static_cast<std::size_t>(deepest + 1));
    std::vector<int> to_make;
    lv_style[1] = entry_style;
    for (int lv = 2; lv <= deepest; ++lv) {
        auto id = set_style(lv);
        if (id.empty() && base) {
            const std::string want = base->name + " " + std::to_string(lv);
            for (const auto& s : d.ids_of<Style>())
                if (const auto* x = d.as<Style>(s); x->kind == StyleKind::Paragraph && x->name == want) { id = s; break; }
        }
        if (id.empty() && base) to_make.push_back(lv);
        if (id.empty()) id = entry_style;   // no base to make one from: level 1's (made ones replace it below)
        lv_style[static_cast<std::size_t>(lv)] = id;
    }
    // s135: an index's letter headings -- their style found by name, else made below.
    Doc::Id head_style;
    bool make_head = false;
    if (std::any_of(entries.begin(), entries.end(), [](const ListEntry& e) { return e.level == 0; })) {
        if (base) {
            const std::string want = base->name + " Letter";
            for (const auto& s : d.ids_of<Style>())
                if (const auto* x = d.as<Style>(s); x->kind == StyleKind::Paragraph && x->name == want) { head_style = s; break; }
            make_head = head_style.empty();
        }
        if (head_style.empty()) head_style = entry_style;
    }
    const std::string sep = l->kind == ListKind::Index ? ", " : "\t";   // s135: an index: "harbor, 1, 3"
    const std::size_t title = paras.size();
    for (const auto& e : entries) {
        if (e.level == 0) { paras.push_back({head_style, e.text}); continue; }
        if (e.para) {   // s140: the source itself, in the entry style
            Para p = *e.para;
            p.style = lv_style[static_cast<std::size_t>(std::clamp(e.level, 1, deepest))];
            paras.push_back(std::move(p));
            continue;
        }
        paras.push_back({lv_style[static_cast<std::size_t>(std::clamp(e.level, 1, deepest))],
                         l->numbers && !e.folio.empty() ? e.text + sep + e.folio
                                                        : e.text});   // s090: or the words alone; s140: none to write, the words alone
    }
    if (paras.empty()) paras.push_back({entry_style, ""});
    const int n = static_cast<int>(entries.size());
    if (st && adopt.empty() && to_make.empty() && !make_head && st->paras == paras && l->built_entries == n) return false;   // s142: an adoption is a change
    std::vector<Doc::Id> ids{list};
    if (st) ids.push_back(adopt.empty() ? l->story : adopt);
    return j.run("Build " + l->name, ids, [&]() -> std::vector<Doc::Id> {
        std::vector<Doc::Id> made;
        GenList nl = *d.as<GenList>(list);
        if (!adopt.empty()) nl.story = adopt;   // s140
        if (make_head) {   // s135: the letter headings' style
            double pos = 0;
            for (const auto& s : d.ids_of<Style>()) pos = std::max(pos, d.as<Style>(s)->pos);
            Style ns = *d.as<Style>(entry_style);
            ns.name = ns.name + " Letter";
            ns.pos = pos + 1;
            ns.based_on = entry_style;
            ns.next.clear();
            ns.bold = true;
            ns.space_before += 6;
            ns.own = {"bold", "space_before"};
            const Doc::Id sid = d.fresh_id("style");
            d.put(sid, ns);
            made.push_back(sid);
            for (std::size_t k = 0; k < entries.size(); ++k)
                if (entries[k].level == 0) paras[title + k].style = sid;
        }
        // s088: the deeper levels' styles, made on first need.
        if (!to_make.empty()) {
            double pos = 0;
            for (const auto& s : d.ids_of<Style>()) pos = std::max(pos, d.as<Style>(s)->pos);
            for (int lv : to_make) {
                Style ns = *d.as<Style>(entry_style);
                ns.name = ns.name + " " + std::to_string(lv);
                ns.pos = ++pos;
                ns.based_on = entry_style;
                ns.next.clear();
                ns.left_indent += 12.0 * (lv - 1);
                for (auto& t : ns.tabs) t.pos = std::max(0.0, t.pos - 12.0 * (lv - 1));   // stops count from the indent: same place on the page
                ns.own = {"left_indent", "tabs"};
                const Doc::Id sid = d.fresh_id("style");
                d.put(sid, ns);
                made.push_back(sid);
                for (std::size_t k = 0; k < entries.size(); ++k)
                    if (entries[k].level > 0 && std::clamp(entries[k].level, 1, deepest) == lv) paras[title + k].style = sid;
                if (nl.entry_styles.size() < static_cast<std::size_t>(lv)) nl.entry_styles.resize(static_cast<std::size_t>(lv));
                nl.entry_styles[static_cast<std::size_t>(lv - 1)] = sid;
            }
        }
        if (d.as<Story>(nl.story)) {
            Story s = *d.as<Story>(nl.story);
            s.paras = paras;
            d.put(nl.story, s);
        } else {
            const Doc::Id sid = d.fresh_id("story");
            d.put(sid, Story{nl.name, paras});
            made.push_back(sid);
            Rect r = margin_rect(d, nl.placed_on);
            if (r.w < 1 || r.h < 1) { const auto* p = d.as<Page>(nl.placed_on); r = {36, 36, p->w - 72, p->h - 72}; }
            Frame f;
            f.owner = nl.placed_on;
            f.kind = FrameKind::Text;
            f.x = r.x; f.y = r.y; f.w = r.w; f.h = r.h;
            double z = 0;
            for (const auto& o : d.frames_on(nl.placed_on)) z = std::max(z, d.as<Frame>(o)->z);
            f.z = z + 1;
            f.story = sid;
            const Doc::Id fid = d.fresh_id("frame");
            d.put(fid, f);
            made.push_back(fid);
            nl.story = sid;
        }
        nl.built_entries = n;
        d.put(list, nl);
        return made;
    });
}

namespace {
// s025 / s072: the page's own copy of master item `mf`, made inside a step;
// its records (the frame, a text frame's story) go on `ids`.
// s182 (W2): an item across the spine, overridden on one page, keeps only
// the part on that page -- cut at the spine, so it no longer hangs over
// the facing page (where the master's other part still shows). A picture
// stays exactly where it was (placed by hand from its old place); a shape
// keeps its fill (a stroke gains an edge at the spine). Kept whole, as
// before: a text frame (cutting it would reflow its words -- Scott's
// call), a turned one, and a vector picture set to Fill / Fit (its aspect
// isn't known here). `c` is already shifted for the page (across_dx).
void cut_at_spine(const Doc& d, Frame& c, Side side) {
    if (!c.across || (side != Side::Left && side != Side::Right) || c.rot != 0) return;
    if (c.kind == FrameKind::Text) return;
    const auto* m = d.as<Master>(c.owner);
    if (!m) return;
    const double x0 = side == Side::Right ? std::max(c.x, 0.0) : c.x;
    const double x1 = side == Side::Left ? std::min(c.x + c.w, m->w) : c.x + c.w;
    if (x1 - x0 <= 0 || (x0 == c.x && x1 == c.x + c.w)) return;   // nothing over the spine
    if (c.kind == FrameKind::Image && !c.asset.empty()) {
        const auto* a = d.as<Asset>(c.asset);
        const bool known = a && a->px_w > 0 && a->px_h > 0;
        if (c.fit != ImageFit::Custom && !known) return;
        const Rect pr = picture_rect({0, 0, c.w, c.h}, c, known ? a->px_w : 1, known ? a->px_h : 1);
        c.fit = ImageFit::Custom;
        c.pic_x = pr.x - (x0 - c.x);
        c.pic_y = pr.y;
        c.pic_w = pr.w;
    }
    c.x = x0;
    c.w = x1 - x0;
}

Doc::Id copy_master_item(Doc& d, const Doc::Id& page, const Doc::Id& mf, std::vector<Doc::Id>& ids) {
    Frame c = *d.as<Frame>(mf);
    const Side side = d.side_of(page);
    c.x += across_dx(d, c, side);   // s155: across the spine -- where it sits on this page
    cut_at_spine(d, c, side);       // s182 (W2): and only the part on this page
    c.across = false;
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
    const auto made = d.fresh_id("frame");
    d.put(made, c);
    ids.push_back(made);
    return made;
}
}  // namespace

std::vector<Doc::Id> overridable_items(const Doc& d, const Doc::Id& page) {
    std::vector<Doc::Id> out;
    if (!d.as<Page>(page)) return out;
    for (const auto& m : master_chain(d, page))
        for (const auto& fid : d.frames_on(m))
            if (master_item_on(d, fid, page)) out.push_back(fid);
    return out;
}

Doc::Id override_master_item(DocJournal& j, Doc& d, const Doc::Id& page, const Doc::Id& mf) {
    const auto* f = d.as<Frame>(mf);
    if (!f || !d.as<Page>(page) || !d.as<Master>(f->owner)) return {};
    const auto chain = master_chain(d, page);
    if (std::find(chain.begin(), chain.end(), f->owner) == chain.end() || !master_item_on(d, mf, page)) return {};
    Doc::Id made;
    j.run("Override master item", {}, [&] {
        std::vector<Doc::Id> ids;   // what's made: the step's records
        made = copy_master_item(d, page, mf, ids);
        return ids;
    });
    return made;
}

std::vector<Doc::Id> override_all(DocJournal& j, Doc& d, const std::vector<Doc::Id>& pages) {
    std::vector<std::pair<Doc::Id, Doc::Id>> plan;   // page, master item
    for (const auto& p : pages)
        for (const auto& mf : overridable_items(d, p)) plan.push_back({p, mf});
    std::vector<Doc::Id> made;
    if (plan.empty()) return made;
    j.run(plan.size() == 1 ? "Override master item" : "Override all master items", {}, [&] {
        std::vector<Doc::Id> ids;
        for (const auto& [p, mf] : plan) made.push_back(copy_master_item(d, p, mf, ids));
        return ids;
    });
    return made;
}

std::vector<Doc::Id> override_master_items(DocJournal& j, Doc& d, const std::vector<MasterItemOn>& items) {   // s169
    std::vector<MasterItemOn> plan;
    for (const auto& it : items) {
        const auto* f = d.as<Frame>(it.item);
        if (!f || !d.as<Page>(it.page) || !d.as<Master>(f->owner)) continue;
        const auto chain = master_chain(d, it.page);
        if (std::find(chain.begin(), chain.end(), f->owner) == chain.end() || !master_item_on(d, it.item, it.page)) continue;
        if (std::any_of(plan.begin(), plan.end(), [&](const MasterItemOn& p) { return p.page == it.page && p.item == it.item; })) continue;
        plan.push_back(it);
    }
    std::vector<Doc::Id> made;
    if (plan.empty()) return made;
    j.run(plan.size() == 1 ? "Override master item" : "Override " + std::to_string(plan.size()) + " master items", {}, [&] {
        std::vector<Doc::Id> ids;
        for (const auto& p : plan) made.push_back(copy_master_item(d, p.page, p.item, ids));
        return ids;
    });
    return made;
}

bool remove_all_overrides(DocJournal& j, Doc& d, const std::vector<Doc::Id>& pages) {
    std::vector<Doc::Id> copies;
    for (const auto& p : pages)
        if (d.as<Page>(p))
            for (const auto& f : d.frames_on(p))
                if (!d.as<Frame>(f)->overrides.empty()) copies.push_back(f);
    return remove_overrides(j, d, copies);
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

bool set_wrap_shape(DocJournal& j, Doc& d, const std::vector<Doc::Id>& ids,
                    const std::map<Doc::Id, std::vector<float>>& traced) {
    std::vector<Doc::Id> frames, assets;
    for (const auto& id : ids) {
        const auto* fr = d.as<Frame>(id);
        if (!fr || fr->kind != FrameKind::Image) continue;
        if (fr->wrap != Wrap::Shape) frames.push_back(id);
        const auto t = traced.find(fr->asset);
        if (const auto* a = d.as<Asset>(fr->asset); a && t != traced.end() && a->shape != t->second &&
                                                   std::find(assets.begin(), assets.end(), fr->asset) == assets.end())
            assets.push_back(fr->asset);
    }
    if (frames.empty() && assets.empty()) return false;
    std::vector<Doc::Id> touched = frames;
    touched.insert(touched.end(), assets.begin(), assets.end());
    return j.run("Text wrap", touched, [&] {
        for (const auto& id : frames) {
            Frame t = *d.as<Frame>(id);
            if (t.wrap == Wrap::None && t.wrap_off == Sides{}) t.wrap_off = {9, 9, 9, 9};
            t.wrap = Wrap::Shape;
            d.put(id, t);
        }
        for (const auto& id : assets) {
            Asset a = *d.as<Asset>(id);
            a.shape = traced.at(id);
            d.put(id, a);
        }
    });
}

// ── stacking order (s070) ───────────────────────────────────────────────────
namespace {
// The owner's frames bottom up, rearranged.
std::vector<Doc::Id> arranged(std::vector<Doc::Id> order, const std::set<Doc::Id>& picked, Arrange how) {
    const auto is = [&](const Doc::Id& id) { return picked.count(id) > 0; };
    if (how == Arrange::Front || how == Arrange::Back) {
        std::vector<Doc::Id> in, out;
        for (const auto& id : order) (is(id) ? in : out).push_back(id);
        if (how == Arrange::Front) { out.insert(out.end(), in.begin(), in.end()); return out; }
        in.insert(in.end(), out.begin(), out.end());
        return in;
    }
    // Forward: from the top down, a picked frame swaps with the unpicked one above it.
    const int n = static_cast<int>(order.size());
    if (how == Arrange::Forward) {
        for (int i = n - 2; i >= 0; --i)
            if (is(order[static_cast<std::size_t>(i)]) && !is(order[static_cast<std::size_t>(i + 1)]))
                std::swap(order[static_cast<std::size_t>(i)], order[static_cast<std::size_t>(i + 1)]);
    } else {
        for (int i = 1; i < n; ++i)
            if (is(order[static_cast<std::size_t>(i)]) && !is(order[static_cast<std::size_t>(i - 1)]))
                std::swap(order[static_cast<std::size_t>(i)], order[static_cast<std::size_t>(i - 1)]);
    }
    return order;
}
}  // namespace

bool arrange(DocJournal& j, Doc& d, const std::vector<Doc::Id>& frames, Arrange how) {
    std::map<Doc::Id, std::vector<Doc::Id>> plan;   // owner -> new order, bottom up
    std::set<Doc::Id> picked;
    for (const auto& id : frames) if (d.as<Frame>(id)) picked.insert(id);
    std::vector<Doc::Id> touched;
    for (const auto& id : picked) {
        const auto& owner = d.as<Frame>(id)->owner;
        if (plan.count(owner)) continue;
        const auto now = d.frames_on(owner);
        auto next = arranged(now, picked, how);
        bool changed = next != now;
        // Renumbered z must differ somewhere too (ties read as an order already).
        for (std::size_t k = 0; !changed && k < next.size(); ++k) changed = d.as<Frame>(next[k])->z != static_cast<double>(k);
        if (!changed) continue;
        for (const auto& f : next) touched.push_back(f);
        plan[owner] = std::move(next);
    }
    if (plan.empty()) return false;
    // Only a real reorder counts as a change (a renumber alone is not the user's).
    bool moved = false;
    for (const auto& [owner, next] : plan) moved = moved || next != d.frames_on(owner);
    if (!moved) return false;
    static const char* labels[] = {"Bring to Front", "Bring Forward", "Send Backward", "Send to Back"};
    return j.run(labels[static_cast<int>(how)], touched, [&] {
        for (const auto& [owner, next] : plan)
            for (std::size_t k = 0; k < next.size(); ++k) {
                Frame f = *d.as<Frame>(next[k]);
                f.z = static_cast<double>(k);
                d.put(next[k], f);
            }
    });
}

// ── per-side values (s067) ──────────────────────────────────────────────────
bool set_side(DocJournal& j, Doc& d, const std::vector<Doc::Id>& frames, SideOf which, int side, double pts) {
    if (side < -1 || side > 3) return false;
    const double v = std::clamp(pts, 0.0, 500.0);
    return edit_frames(j, d, frames, which == SideOf::Inset ? "Text inset" : "Wrap offset", [&](Frame& f) {
        if (which == SideOf::Inset && f.kind != FrameKind::Text) return;
        Sides& s = which == SideOf::Inset ? f.inset : f.wrap_off;
        if (side == -1 || side == 0) s.top = v;
        if (side == -1 || side == 1) s.right = v;
        if (side == -1 || side == 2) s.bottom = v;
        if (side == -1 || side == 3) s.left = v;
    });
}
bool set_column_width(DocJournal& j, Doc& d, const std::vector<Doc::Id>& frames, int col, double pts) {
    return edit_frames(j, d, frames, "Column width", [&](Frame& f) {
        const int n = column_count(f);
        if (f.kind != FrameKind::Text || n < 2 || col < 0 || col >= n) return;
        std::vector<double> w(static_cast<std::size_t>(n));
        double room = 0;
        for (int c = 0; c < n; ++c) room += (w[static_cast<std::size_t>(c)] = column_width(f, c));
        const double v = std::clamp(pts, 1.0, std::max(1.0, room - (n - 1)));
        double others = room - w[static_cast<std::size_t>(col)];
        const double left = room - v;
        for (int c = 0; c < n; ++c) {
            auto& x = w[static_cast<std::size_t>(c)];
            if (c == col) x = v;
            else x = std::max(1.0, others > 0 ? x * left / others : left / (n - 1));
        }
        // Kept as weights, rounded to a hundredth of a point (stable in a file).
        for (auto& x : w) x = std::round(x * 100) / 100;
        f.widths = w;
    });
}

// ── text (s010) ─────────────────────────────────────────────────────────────
Carried carried_between(const Story& s, TextPos a, TextPos b) {   // s132
    Carried c;
    if (s.paras.empty()) return c;
    a = clamp_pos(s, a);
    b = clamp_pos(s, b);
    if (b < a) std::swap(a, b);
    for (int p = a.para; p <= b.para; ++p) {
        Para para = s.paras[static_cast<std::size_t>(p)];
        fit_notes(para);
        fit_marks(para);
        const auto from = static_cast<std::size_t>(p == a.para ? a.byte : 0);
        const auto to = p == b.para ? static_cast<std::size_t>(b.byte) : para.text.size();
        const auto take = [&](const char* mark, const auto& all, auto& out) {
            const int h = marks_in(para.text, mark, 0, from), n = marks_in(para.text, mark, from, to);
            out.insert(out.end(), all.begin() + h, all.begin() + h + n);
        };
        take(kMarkNote, para.notes, c.notes);
        take(kMarkRef, para.refs, c.refs);
        take(kMarkAnchor, para.anchors, c.anchors);
        take(kMarkIndex, para.index, c.index);   // s134
    }
    return c;
}

TextPos edit_text(DocJournal& j, Doc& d, const Doc::Id& story, TextPos a, TextPos b, const std::string& text,
                  const std::string& label, const std::string& merge_key, const Carried* with) {
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
    const ParaLocal over = pa.over;   // s093: and its own settings
    std::size_t head_len = 0;
    for (const auto& sp : head) head_len += sp.text.size();
    // s109: the notes ride along with their marks -- those before a and after
    // b kept, in order; a mark typed or pasted gets an empty note.
    std::vector<Note> notes;
    {
        Para fa = pa, fb = pb;
        fit_notes(fa);
        fit_notes(fb);
        const int nh = notes_in(fa.text, 0, static_cast<std::size_t>(a.byte));
        notes.assign(fa.notes.begin(), fa.notes.begin() + nh);
        for (int i = 0; i < notes_in(text); ++i)   // s132: a paste's, their words
            notes.push_back(with && i < static_cast<int>(with->notes.size()) ? with->notes[static_cast<std::size_t>(i)] : Note{});
        const int nt = notes_in(fb.text, static_cast<std::size_t>(b.byte));
        notes.insert(notes.end(), fb.notes.end() - nt, fb.notes.end());
    }

    // s128: refs and anchors ride with their marks, as notes do; a pasted
    // reference keeps its target, a pasted anchor gets no id (one place, one id).
    std::vector<XRef> refs;
    std::vector<std::string> anchors;
    std::vector<IndexMark> index;   // s134
    std::size_t pasted_from = 0, pasted_to = 0;   // s132: anchors[from, to) came with the paste
    {
        Para fa = pa, fb = pb;
        fit_marks(fa);
        fit_marks(fb);
        const auto carry = [&](const auto& ha, const auto& tb, const char* mark, auto& out, auto blank, const auto* given) {
            const int nh = marks_in(fa.text, mark, 0, static_cast<std::size_t>(a.byte));
            out.assign(ha.begin(), ha.begin() + nh);
            for (int i = 0; i < marks_in(text, mark); ++i)
                out.push_back(given && i < static_cast<int>(given->size()) ? (*given)[static_cast<std::size_t>(i)] : blank);
            const int nt = marks_in(fb.text, mark, static_cast<std::size_t>(b.byte));
            out.insert(out.end(), tb.end() - nt, tb.end());
        };
        carry(fa.refs, fb.refs, kMarkRef, refs, XRef{}, with ? &with->refs : nullptr);
        pasted_from = static_cast<std::size_t>(marks_in(fa.text, kMarkAnchor, 0, static_cast<std::size_t>(a.byte)));
        pasted_to = pasted_from + static_cast<std::size_t>(marks_in(text, kMarkAnchor));
        carry(fa.anchors, fb.anchors, kMarkAnchor, anchors, std::string{}, with ? &with->anchors : nullptr);
        carry(fa.index, fb.index, kMarkIndex, index, IndexMark{}, with ? &with->index : nullptr);   // s134
    }

    std::vector<Para> made;
    std::string prev = style;
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        std::vector<Span> sp;
        if (i == 0) sp = head;
        sp.push_back({pieces[i], cstyle, local});
        const bool last = i + 1 == pieces.size();
        if (last) sp.insert(sp.end(), tail.begin(), tail.end());
        // s037: Enter at a paragraph's end starts its Next style. s081: so
        // does every new paragraph of a paste -- each the Next style of the
        // one before (a heading, its body, ...) -- except a last one that
        // carries the rest of the paragraph pasted into (it keeps its style).
        const bool next = i > 0 && (!last || tail.empty());
        const std::string ps = next ? next_style_of(d, prev) : style;
        made.push_back(para_of(ps, sp));
        prev = ps;
        if (i == 0) { made.back().restart = restart; made.back().brk = brk; }
        if (i == 0 || (last && !tail.empty())) made.back().over = over;   // s093: the paragraph's own pieces keep its settings
    }
    {   // s109: each new paragraph its notes, in mark order
        std::size_t at = 0;
        for (auto& m : made) {
            const auto n = static_cast<std::size_t>(notes_in(m.text));
            for (std::size_t k = 0; k < n; ++k) m.notes.push_back(at < notes.size() ? notes[at++] : Note{});
        }
    }
    std::vector<std::pair<int, int>> pasted;   // s132: (paragraph in made, anchor index) of each pasted anchor
    {   // s128: and its refs and anchors
        std::size_t ar = 0, aa = 0, ai = 0;
        for (int mi = 0; mi < static_cast<int>(made.size()); ++mi) {
            auto& m = made[static_cast<std::size_t>(mi)];
            for (int k = 0; k < marks_in(m.text, kMarkRef); ++k) m.refs.push_back(ar < refs.size() ? refs[ar++] : XRef{});
            for (int k = 0; k < marks_in(m.text, kMarkIndex); ++k) m.index.push_back(ai < index.size() ? index[ai++] : IndexMark{});   // s134
            for (int k = 0; k < marks_in(m.text, kMarkAnchor); ++k) {
                if (aa >= pasted_from && aa < pasted_to) pasted.emplace_back(mi, k);
                m.anchors.push_back(aa < anchors.size() ? anchors[aa++] : std::string{});
            }
        }
    }
    s.paras.erase(s.paras.begin() + a.para, s.paras.begin() + b.para + 1);
    s.paras.insert(s.paras.begin() + a.para, made.begin(), made.end());
    // s132: a pasted anchor whose id is had elsewhere (this story, another): a fresh one.
    if (with && !pasted.empty()) {
        const auto uses = [&](const std::string& id) {
            int n = 0;
            for (const auto& para : s.paras) n += static_cast<int>(std::count(para.anchors.begin(), para.anchors.end(), id));
            for (const auto& sid : d.ids_of<Story>())
                if (sid != story)
                    for (const auto& para : d.as<Story>(sid)->paras) n += static_cast<int>(std::count(para.anchors.begin(), para.anchors.end(), id));
            return n;
        };
        int fresh = 1;
        for (const auto& [mi, k] : pasted) {
            auto& id = s.paras[static_cast<std::size_t>(a.para + mi)].anchors[static_cast<std::size_t>(k)];
            if (id.empty() || uses(id) <= 1) continue;
            while (uses("anchor." + std::to_string(fresh)) > 0) ++fresh;
            id = "anchor." + std::to_string(fresh);
        }
    }

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
        Para was = std::move(para);
        para = para_of(was.style, sp);
        para.restart = was.restart;            // s047
        para.brk = was.brk;                    // s055
        para.over = was.over;                  // s113: its own settings (were dropped)
        para.notes = std::move(was.notes);     // s113: and its footnotes (were dropped)
        para.refs = std::move(was.refs);       // s128
        para.anchors = std::move(was.anchors);
        para.index = std::move(was.index);     // s134
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
        Para was = std::move(para);
        para = para_of(was.style, sp);
        para.restart = was.restart;            // s047
        para.brk = was.brk;                    // s055
        para.over = was.over;                  // s113: its own settings (were dropped)
        para.notes = std::move(was.notes);     // s113: and its footnotes (were dropped)
        para.refs = std::move(was.refs);       // s128
        para.anchors = std::move(was.anchors);
        para.index = std::move(was.index);     // s134
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
        COMPOSIX_SF("no_break", "No break", no_break),   // s075
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
        // s183: span / split columns ride together (one choice sets them).
        StyleField{"span", "Span columns",
                   [](Style& t, const Style& f) { t.span = f.span; t.split = f.split; t.split_gutter = f.split_gutter; },
                   [](const Style& a, const Style& b) { return a.span == b.span && a.split == b.split && a.split_gutter == b.split_gutter; }},
        COMPOSIX_SF("hyphenate", "Hyphenate", hyphenate),
        COMPOSIX_SF("lang", "Language", lang),   // s190
        COMPOSIX_SF("hyph_limit", "Hyphens in a row", hyph_limit),         // s065
        COMPOSIX_SF("hyph_shortest", "Shortest word", hyph_shortest),
        COMPOSIX_SF("hyph_last_word", "Hyphenate last word", hyph_last_word),
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

bool reset_style_fields(DocJournal& j, Doc& d, const std::vector<Doc::Id>& ids, const std::vector<std::string>& keys) {   // s092
    std::vector<Doc::Id> styles;
    for (const auto& id : ids)
        if (const auto* s = d.as<Style>(id); s && d.as<Style>(s->based_on))
            for (const auto& k : keys)
                if (owns(*s, k)) { styles.push_back(id); break; }
    if (styles.empty()) return false;
    std::string label = "Reset";
    for (const auto& k : keys) label += " " + style_field_label(k);
    return j.run(label, with_descendants(d, styles), [&] {
        for (const auto& id : styles) {
            Style t = *d.as<Style>(id);
            const Style& base = *d.as<Style>(t.based_on);
            for (const auto& k : keys) {
                std::erase(t.own, k);
                for (const auto& sf : style_fields())
                    if (k == sf.key) sf.copy(t, base);
            }
            d.put(id, t);
        }
        write_through(d, styles);
    });
}

Style para_style(const Doc& d, const Para& p) {   // s093
    Style s;
    if (const auto* st = d.as<Style>(p.style)) s = *st;
    const auto& o = p.over;
    if (o.align) s.align = *o.align;
    if (o.left_indent) s.left_indent = *o.left_indent;
    if (o.right_indent) s.right_indent = *o.right_indent;
    if (o.first_indent) s.first_indent = *o.first_indent;
    if (o.space_before) s.space_before = *o.space_before;
    if (o.space_after) s.space_after = *o.space_after;
    if (o.keep_next) s.keep_next = *o.keep_next;     // s094
    if (o.keep_lines) s.keep_lines = *o.keep_lines;
    if (o.hyphenate) s.hyphenate = *o.hyphenate;
    if (o.tabs) s.tabs = *o.tabs;   // s157
    return s;
}

bool set_para_local(DocJournal& j, Doc& d, const Doc::Id& story, TextPos a, TextPos b, const std::string& label,
                    const std::function<void(ParaLocal&)>& f) {   // s093
    const auto* st = d.as<Story>(story);
    if (!st || st->paras.empty()) return false;
    a = clamp_pos(*st, a);
    b = clamp_pos(*st, b);
    if (b < a) std::swap(a, b);
    Story s = *st;
    for (int p = a.para; p <= b.para; ++p) f(s.paras[static_cast<std::size_t>(p)].over);
    if (s == *st) return false;
    return j.run(label, {story}, [&] { d.put(story, s); });
}

bool clear_para_local(DocJournal& j, Doc& d, const Doc::Id& story, TextPos a, TextPos b, const std::vector<std::string>& keys) {   // s093
    return set_para_local(j, d, story, a, b, "Paragraph's own settings off", [&](ParaLocal& o) {
        for (const auto& k : keys) {
            if (k == "align") o.align.reset();
            else if (k == "left_indent") o.left_indent.reset();
            else if (k == "right_indent") o.right_indent.reset();
            else if (k == "first_indent") o.first_indent.reset();
            else if (k == "space_before") o.space_before.reset();
            else if (k == "space_after") o.space_after.reset();
            else if (k == "keep_next") o.keep_next.reset();     // s094
            else if (k == "keep_lines") o.keep_lines.reset();
            else if (k == "hyphenate") o.hyphenate.reset();
            else if (k == "tabs") o.tabs.reset();   // s157
        }
    });
}

std::string style_edit_label(const std::string& key) {   // s164
    static const std::map<std::string, std::string> labels{
        {"font", "Font"}, {"ssize", "Size"}, {"leading", "Leading"}, {"bold", "Weight"}, {"italic", "Slant"},
        {"align", "Align"}, {"before", "Space before"}, {"after", "Space after"}, {"lindent", "Left indent"},
        {"rindent", "Right indent"}, {"findent", "First indent"}, {"indent_after", "Indent when"}, {"tabs", "Tabs"},
        {"drop_lines", "Drop cap"}, {"drop_chars", "Drop letters"}, {"color", "Color"}, {"color_tint", "Tint"},
        {"nobreak", "No break"}, {"next_style", "Next style"}, {"list_cont", "Numbering"}, {"list_level", "Level"},
        {"list_type", "List"}, {"list_bullet", "Bullet"}, {"list_num", "Numbers"}, {"list_hang", "Hang"},
        {"pstart", "Start"}, {"keep_next", "Keep with next"}, {"keep_lines", "Keep lines"},
        {"hyph_limit", "Hyphens in a row"}, {"hyph_shortest", "Shortest word"}, {"hyph_last_word", "Hyphenate last word"},
        {"hyphenate", "Hyphenate"}, {"align_grid", "Align to grid"}, {"span", "Span columns"}, {"lang", "Language"},
        {"split_gutter", "Split gutter"}};
    if (const auto it = labels.find(key); it != labels.end()) return it->second;
    if (key.rfind("rule_a_", 0) == 0 || key.rfind("rule_b_", 0) == 0) {
        static const std::map<char, std::string> rl{{'w', "Rule"}, {'c', "Rule colour"}, {'t', "Rule tint"}, {'o', "Rule offset"},
                                                    {'x', "Rule width"}};
        if (const auto it = rl.find(key.back()); it != rl.end() && key.size() == 8) return it->second;
    }
    return {};
}

void style_edit(const Doc& d, Style& st, const std::string& key, const std::string& choice) {   // s164 (from the shell's handler)
    const bool para = st.kind == StyleKind::Paragraph;
    if (key == "font") { st.family = choice; return; }
    if (key == "ssize" || key == "leading" || key == "bold" || key == "italic" || key == "align" || key == "before" || key == "after") {
        const double v = std::stod(choice);
        if (key == "ssize") {
            // Leading follows the size when it was the usual ratio (or auto).
            if (st.size > 0 && st.leading > 0 && std::abs(st.leading - st.size * 1.2) < 1.5) st.leading = std::round(v * 1.2 * 2) / 2;
            st.size = v;
        }
        else if (key == "leading") st.leading = v;
        else if (key == "bold") st.bold = v != 0;
        else if (key == "italic") st.italic = v != 0;
        else if (key == "align") st.align = static_cast<Align>(static_cast<int>(v));
        else if (key == "before") st.space_before = v;
        else st.space_after = v;
        return;
    }
    if (key == "color") { st.color = choice; return; }
    if (key == "color_tint") { st.color_tint = std::stod(choice) / 100; return; }
    if (key == "nobreak") { if (!para) st.no_break = choice == "1"; return; }
    if (!para) return;   // the rest: paragraph styles'
    if (key == "lindent") st.left_indent = std::stod(choice);
    else if (key == "rindent") st.right_indent = std::stod(choice);
    else if (key == "findent") st.first_indent = std::stod(choice);
    else if (key == "indent_after") st.skip_after_heading = choice == "1";
    else if (key == "tabs") st.tabs = tab_preset(d, choice);
    else if (key == "drop_lines") st.drop_lines = std::stoi(choice);
    else if (key == "drop_chars") st.drop_chars = std::stoi(choice);
    else if (key == "next_style") st.next = choice;
    else if (key == "list_cont") st.list.continues = choice == "1";
    else if (key == "list_level") st.list.level = std::clamp(std::stoi(choice), 1, 2);
    else if (key == "list_type") {
        st.list.type = static_cast<ListType>(std::clamp(std::stoi(choice), 0, 2));
        // Room for the marker: the text in from the column by the hang.
        if (st.list.type != ListType::None && st.left_indent < st.list.hang) st.left_indent = st.list.hang;
    }
    else if (key == "list_bullet") st.list.bullet = choice;
    else if (key == "list_num") st.list.numbering = static_cast<Numbering>(std::clamp(std::stoi(choice), 0, 4));
    else if (key == "list_hang") st.list.hang = std::stod(choice);
    else if (key == "pstart") st.start = static_cast<ParaStart>(std::clamp(std::stoi(choice), 0, 4));
    else if (key == "keep_next") st.keep_next = std::stoi(choice) != 0;
    else if (key == "keep_lines") st.keep_lines = std::stoi(choice);
    else if (key == "hyph_limit") st.hyph_limit = std::max(0, std::stoi(choice));
    else if (key == "hyph_shortest") st.hyph_shortest = std::clamp(std::stoi(choice), 4, 20);
    else if (key == "hyph_last_word") st.hyph_last_word = std::stoi(choice) != 0;
    else if (key == "hyphenate") st.hyphenate = choice == "1";
    else if (key == "lang") st.lang = choice == "en-US" ? std::string{} : choice;   // s190
    else if (key == "align_grid") st.align_grid = choice == "1";
    else if (key == "span") {   // s166; s183: "s2".."s4" -- split into that many
        st.span = choice == "1";
        st.split = choice.size() == 2 && choice[0] == 's' ? std::clamp(choice[1] - '0', 2, 4) : 0;
    }
    else if (key == "split_gutter") st.split_gutter = std::clamp(std::stod(choice), 0.0, 500.0);   // s183
    else if (key.rfind("rule_a_", 0) == 0 || key.rfind("rule_b_", 0) == 0) {   // s044: paragraph rules
        const bool above = key[5] == 'a';
        const char what = key.back();
        auto& r = above ? st.rule_above : st.rule_below;
        if (what == 'w') {
            const double w = std::stod(choice);
            // Turned on: sit clear of the text (above: a line's height up; below: a little down).
            if (r.weight <= 0 && w > 0 && r.offset == 0) r.offset = above ? std::round(st.size > 0 ? st.size : 11) : 4;
            r.weight = w;
        } else if (what == 'c') r.color = choice;
        else if (what == 't') r.tint = std::stod(choice) / 100;
        else if (what == 'o') r.offset = std::stod(choice);
        else r.text_width = choice == "1";
    }
}

bool styles_as_one(DocJournal& j, Doc& d, const std::string& label, const std::function<void()>& body) {   // s162
    if (j.in_gesture()) { body(); return false; }
    j.begin(label, d.ids_of<Style>());
    body();
    return j.commit();
}

bool set_para_tabs(DocJournal& j, Doc& d, const Doc::Id& story, TextPos a, TextPos b, const std::string& label,
                   std::vector<TabStop> stops) {   // s157
    std::sort(stops.begin(), stops.end(), [](const TabStop& x, const TabStop& y) { return x.pos < y.pos; });
    const auto* st = d.as<Story>(story);
    if (!st || st->paras.empty()) return false;
    a = clamp_pos(*st, a);
    b = clamp_pos(*st, b);
    if (b < a) std::swap(a, b);
    Story s = *st;
    for (int p = a.para; p <= b.para; ++p) {
        auto& para = s.paras[static_cast<std::size_t>(p)];
        const auto* style = d.as<Style>(para.style);
        if (style && style->tabs == stops) para.over.tabs.reset();   // the style's: follow it
        else para.over.tabs = stops;
    }
    if (s == *st) return false;
    return j.run(label, {story}, [&] { d.put(story, s); });
}

std::string style_key_of_edit(const std::string& e) {   // s092
    static const std::map<std::string, std::string> m{
        {"font", "family"}, {"ssize", "size"}, {"leading", "leading"}, {"bold", "bold"}, {"italic", "italic"},
        {"nobreak", "no_break"}, {"color", "color"}, {"color_tint", "color_tint"}, {"align", "align"},
        {"before", "space_before"}, {"after", "space_after"}, {"lindent", "left_indent"}, {"rindent", "right_indent"},
        {"findent", "first_indent"}, {"indent_after", "skip_after_heading"}, {"tabs", "tabs"}, {"drop_lines", "drop_lines"},
        {"drop_chars", "drop_chars"}, {"span", "span"}, {"split_gutter", "span"}, {"align_grid", "align_grid"}, {"hyphenate", "hyphenate"}, {"lang", "lang"}, {"hyph_limit", "hyph_limit"},
        {"hyph_shortest", "hyph_shortest"}, {"hyph_last_word", "hyph_last_word"}, {"keep_next", "keep_next"},
        {"keep_lines", "keep_lines"}, {"pstart", "start"}};
    if (const auto it = m.find(e); it != m.end()) return it->second;
    if (e.rfind("rule_a", 0) == 0) return "rule_above";
    if (e.rfind("rule_b", 0) == 0) return "rule_below";
    if (e.rfind("list_", 0) == 0) return "list";
    return {};
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

std::string note_breaks(const std::string& text) {   // s116
    std::string t;
    t.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') { t += '\n'; if (i + 1 < text.size() && text[i + 1] == '\n') ++i; }
        else t += text[i];
    }
    return t;
}

// ── footnotes (s109) ────────────────────────────────────────────────────────
TextPos insert_note(DocJournal& j, Doc& d, const Doc::Id& story, TextPos at, const std::string& text) {
    static int serial = 0;
    const std::string key = "insert-note." + std::to_string(++serial);
    const TextPos after = edit_text(j, d, story, at, at, kMarkNote, "Insert Footnote", key);
    // The mark is just before `after`: its note takes the words (the same step).
    const auto* st = d.as<Story>(story);
    if (!st || after.byte < 3) return after;
    const int k = note_index_at(*st, {after.para, after.byte - 3});
    if (k >= 0 && !text.empty()) set_note_text(j, d, story, after.para, k, text, key);
    return after;
}

bool set_note_text(DocJournal& j, Doc& d, const Doc::Id& story, int p, int k, const std::string& text,
                   const std::string& merge_key) {
    const auto* st = d.as<Story>(story);
    if (!st || p < 0 || p >= static_cast<int>(st->paras.size())) return false;
    Story s = *st;
    auto& para = s.paras[static_cast<std::size_t>(p)];
    fit_notes(para);
    if (k < 0 || k >= static_cast<int>(para.notes.size())) return false;
    auto& n = para.notes[static_cast<std::size_t>(k)];
    const std::string t = note_breaks(text);   // s116: paragraphs kept ('\n')
    if (n.text == t) return false;
    n.text = t;
    const int len = static_cast<int>(t.size());
    std::erase_if(n.runs, [&](const Run& r) { return r.start >= len; });
    for (auto& r : n.runs) r.end = std::min(r.end, len);
    j.run(merge_key.empty() ? "Footnote" : "Insert Footnote", {story}, [&] { d.put(story, s); }, merge_key);
    return true;
}

int note_index_at(const Story& s, TextPos at) {
    if (at.para < 0 || at.para >= static_cast<int>(s.paras.size())) return -1;
    const auto& t = s.paras[static_cast<std::size_t>(at.para)].text;
    if (!note_at(t, static_cast<std::size_t>(at.byte))) return -1;
    return notes_in(t, 0, static_cast<std::size_t>(at.byte));
}

// ── cross-references (s128) ────────────────────────────────────────────────
AnchorAt find_anchor(const Doc& d, const std::string& id) {
    if (id.empty()) return {};
    for (const auto& sid : d.ids_of<Story>()) {
        const auto& st = *d.as<Story>(sid);
        for (int p = 0; p < static_cast<int>(st.paras.size()); ++p) {
            const auto& para = st.paras[static_cast<std::size_t>(p)];
            int k = 0;
            for (std::size_t i = para.text.find(kMarkAnchor); i != std::string::npos; i = para.text.find(kMarkAnchor, i + 3), ++k)
                if (k < static_cast<int>(para.anchors.size()) && para.anchors[static_cast<std::size_t>(k)] == id)
                    return {sid, {p, static_cast<int>(i)}};
        }
    }
    return {};
}

namespace {
std::string fresh_anchor_id(const Doc& d) {
    for (int n = 1;; ++n) {
        const std::string id = "anchor." + std::to_string(n);
        if (find_anchor(d, id).story.empty()) return id;
    }
}
// The anchor id and the reference written into a copy of the story.
void put_mark(Story& s, TextPos at, const char* mark) {
    auto& para = s.paras[static_cast<std::size_t>(at.para)];
    para.text.insert(static_cast<std::size_t>(at.byte), mark);
    // Runs past the place move on.
    for (auto& r : para.runs) {
        if (r.start >= at.byte) r.start += 3;
        if (r.end > at.byte) r.end += 3;
    }
}
}  // namespace

std::string insert_anchor(DocJournal& j, Doc& d, const Doc::Id& story, TextPos at) {
    const auto* st = d.as<Story>(story);
    if (!st || st->paras.empty()) return {};
    Story s = *st;
    at = clamp_pos(s, at);
    const std::string id = fresh_anchor_id(d);
    auto& para = s.paras[static_cast<std::size_t>(at.para)];
    fit_marks(para);
    const int k = marks_in(para.text, kMarkAnchor, 0, static_cast<std::size_t>(at.byte));
    put_mark(s, at, kMarkAnchor);
    para.anchors.insert(para.anchors.begin() + k, id);
    j.run("Insert Anchor", {story}, [&] { d.put(story, s); });
    return id;
}

TextPos insert_ref(DocJournal& j, Doc& d, const Doc::Id& story, TextPos at, const std::string& target, RefShow show,
                   const std::string& pattern) {
    const auto* st = d.as<Story>(story);
    if (!st || st->paras.empty()) return at;
    Story s = *st;
    at = clamp_pos(s, at);
    auto& para = s.paras[static_cast<std::size_t>(at.para)];
    fit_marks(para);
    const int k = marks_in(para.text, kMarkRef, 0, static_cast<std::size_t>(at.byte));
    put_mark(s, at, kMarkRef);
    para.refs.insert(para.refs.begin() + k, XRef{target, show, keeps_pattern(show) ? pattern : std::string{}});
    j.run("Insert Cross-Reference", {story}, [&] { d.put(story, s); });
    return {at.para, at.byte + 3};
}

bool insert_ref_to(DocJournal& j, Doc& d, const Doc::Id& story, TextPos at, const Doc::Id& to_story, int to_para, RefShow show,
                   const std::string& pattern) {
    const auto* ts = d.as<Story>(to_story);
    if (!ts || to_para < 0 || to_para >= static_cast<int>(ts->paras.size()) || !d.as<Story>(story)) return false;
    // The paragraph's anchor: the first in it, else one made at its start.
    std::string id;
    {
        const auto& para = ts->paras[static_cast<std::size_t>(to_para)];
        if (const auto i = para.text.find(kMarkAnchor); i != std::string::npos && !para.anchors.empty() && !para.anchors.front().empty())
            id = para.anchors.front();
    }
    bool did = false;
    j.run("Insert Cross-Reference", {story, to_story}, [&] {
        if (id.empty()) {
            id = insert_anchor(j, d, to_story, {to_para, 0});
            if (story == to_story && at.para == to_para) at.byte += 3;   // the anchor went in before it
        }
        insert_ref(j, d, story, at, id, show, pattern);
        did = true;
    });
    return did;
}

bool insert_ref_to_note(DocJournal& j, Doc& d, const Doc::Id& story, TextPos at, const Doc::Id& to_story, int to_para, int k,
                        RefShow show, const std::string& pattern) {   // s129
    const auto* ts = d.as<Story>(to_story);
    if (!ts || !d.as<Story>(story) || to_para < 0 || to_para >= static_cast<int>(ts->paras.size())) return false;
    const int m = note_mark_byte(*ts, to_para, k);
    if (m < 0) return false;
    // An anchor right before the mark already: that one.
    std::string id;
    {
        const auto& para = ts->paras[static_cast<std::size_t>(to_para)];
        if (m >= 3 && para.text.compare(static_cast<std::size_t>(m - 3), 3, kMarkAnchor) == 0) {
            const int ai = marks_in(para.text, kMarkAnchor, 0, static_cast<std::size_t>(m - 3));
            if (ai < static_cast<int>(para.anchors.size())) id = para.anchors[static_cast<std::size_t>(ai)];
        }
    }
    bool did = false;
    j.run("Insert Cross-Reference", {story, to_story}, [&] {
        if (id.empty()) {
            id = insert_anchor(j, d, to_story, {to_para, m});
            if (story == to_story && at.para == to_para && at.byte >= m) at.byte += 3;
        }
        insert_ref(j, d, story, at, id, show, pattern);
        did = true;
    });
    return did;
}

RefAt ref_near(const Story& s, TextPos at) {   // s130
    if (at.para < 0 || at.para >= static_cast<int>(s.paras.size())) return {};
    const auto& para = s.paras[static_cast<std::size_t>(at.para)];
    for (const int b : {at.byte, at.byte - 3}) {
        if (b < 0 || static_cast<std::size_t>(b) + 3 > para.text.size()) continue;
        if (para.text.compare(static_cast<std::size_t>(b), 3, kMarkRef) != 0) continue;
        const int k = marks_in(para.text, kMarkRef, 0, static_cast<std::size_t>(b));
        if (k < static_cast<int>(para.refs.size())) return {at.para, k, b};
    }
    return {};
}

bool set_ref(DocJournal& j, Doc& d, const Doc::Id& story, int para, int k, RefShow show, const std::string& pattern) {   // s130
    const auto* st = d.as<Story>(story);
    if (!st || para < 0 || para >= static_cast<int>(st->paras.size()) || k < 0) return false;
    if (k >= static_cast<int>(st->paras[static_cast<std::size_t>(para)].refs.size())) return false;
    Story s = *st;
    auto& r = s.paras[static_cast<std::size_t>(para)].refs[static_cast<std::size_t>(k)];
    const XRef was = r;
    r.show = show;
    r.pattern = keeps_pattern(show) ? pattern : std::string{};
    if (show != RefShow::Cite) r.more.clear();   // s143: only a citation has others
    if (r == was) return false;
    j.run("Edit Cross-Reference", {story}, [&] { d.put(story, s); });
    return true;
}

namespace {
// The anchor a reference to paragraph p (note k, or -1) would point at; "" none yet.
std::string anchor_for(const Story& ts, int p, int note_k) {
    const auto& para = ts.paras[static_cast<std::size_t>(p)];
    if (note_k < 0) {
        if (para.text.find(kMarkAnchor) != std::string::npos && !para.anchors.empty()) return para.anchors.front();
        return {};
    }
    const int m = note_mark_byte(ts, p, note_k);
    if (m >= 3 && para.text.compare(static_cast<std::size_t>(m - 3), 3, kMarkAnchor) == 0) {
        const int ai = marks_in(para.text, kMarkAnchor, 0, static_cast<std::size_t>(m - 3));
        if (ai < static_cast<int>(para.anchors.size())) return para.anchors[static_cast<std::size_t>(ai)];
    }
    return {};
}
}  // namespace

bool retarget_ref(DocJournal& j, Doc& d, const Doc::Id& story, int para, int k, const Doc::Id& to_story, int to_para,
                  int note_k, RefShow show, const std::string& pattern) {   // s130
    const auto* st = d.as<Story>(story);
    const auto* ts = d.as<Story>(to_story);
    if (!st || !ts || to_para < 0 || to_para >= static_cast<int>(ts->paras.size())) return false;
    if (para < 0 || para >= static_cast<int>(st->paras.size()) || k < 0 ||
        k >= static_cast<int>(st->paras[static_cast<std::size_t>(para)].refs.size()))
        return false;
    int m = 0;
    if (note_k >= 0 && (m = note_mark_byte(*ts, to_para, note_k)) < 0) return false;
    std::string id = anchor_for(*ts, to_para, note_k);
    {   // Already pointing there, showing that: nothing.
        const XRef want{id, show, keeps_pattern(show) ? pattern : std::string{}};
        if (!id.empty() && st->paras[static_cast<std::size_t>(para)].refs[static_cast<std::size_t>(k)] == want) return false;
    }
    j.run("Edit Cross-Reference", {story, to_story}, [&] {
        if (id.empty()) id = insert_anchor(j, d, to_story, {to_para, m});   // (refs are counted, not placed: k holds)
        Story s = *d.as<Story>(story);
        s.paras[static_cast<std::size_t>(para)].refs[static_cast<std::size_t>(k)] =
            XRef{id, show, keeps_pattern(show) ? pattern : std::string{}};
        d.put(story, s);
    });
    return true;
}

namespace {
std::string trim_ws(std::string t) {
    const auto a = t.find_first_not_of(" \t\n\r");
    if (a == std::string::npos) return {};
    return t.substr(a, t.find_last_not_of(" \t\n\r") - a + 1);
}
}  // namespace

namespace {
IndexMark tidy(IndexMark m) {   // s136: each part trimmed
    m.term = trim_ws(m.term);
    m.sub = trim_ws(m.sub);
    m.see = trim_ws(m.see);
    if (m.see.empty()) m.also = false;
    if (!m.see.empty() && !m.also) m.span = IndexSpan::Point;   // s139: a See has no pages to cover
    m.count = m.span == IndexSpan::Paragraphs ? std::clamp(m.count, 1, 9999) : 1;
    return m;
}
}  // namespace

bool heading_para(const Doc& d, const Para& p) {   // s139
    for (const auto& lid : d.ids_of<GenList>()) {
        const auto& l = *d.as<GenList>(lid);
        if (l.kind == ListKind::Contents && std::find(l.from_styles.begin(), l.from_styles.end(), p.style) != l.from_styles.end())
            return true;
    }
    const auto* s = d.as<Style>(p.style);
    return s && s->name.rfind("Heading", 0) == 0;
}

int index_span_end(const Doc& d, const Story& s, int p, const IndexMark& m) {   // s139
    const int last = static_cast<int>(s.paras.size()) - 1;
    switch (m.span) {
    case IndexSpan::Point:
    case IndexSpan::Paragraph: return p;
    case IndexSpan::Paragraphs: return std::min(last, p + std::max(1, m.count) - 1);
    case IndexSpan::StoryEnd: return last;
    case IndexSpan::NextHeading:
        for (int q = p + 1; q <= last; ++q)
            if (heading_para(d, s.paras[static_cast<std::size_t>(q)])) return q - 1;
        return last;
    }
    return p;
}

TextPos insert_index_mark(DocJournal& j, Doc& d, const Doc::Id& story, TextPos at, const std::string& term,
                          const std::string& sub) {   // s134
    return insert_index_mark(j, d, story, at, IndexMark{term, sub});
}

TextPos insert_index_mark(DocJournal& j, Doc& d, const Doc::Id& story, TextPos at, const IndexMark& mark) {   // s136
    const auto* st = d.as<Story>(story);
    const auto m = tidy(mark);
    if (!st || st->paras.empty() || m.term.empty()) return at;
    Story s = *st;
    at = clamp_pos(s, at);
    auto& para = s.paras[static_cast<std::size_t>(at.para)];
    fit_marks(para);
    const int k = marks_in(para.text, kMarkIndex, 0, static_cast<std::size_t>(at.byte));
    put_mark(s, at, kMarkIndex);
    para.index.insert(para.index.begin() + k, m);
    j.run("Index Entry", {story}, [&] { d.put(story, s); });
    return {at.para, at.byte + 3};
}

IndexAt index_near(const Story& s, TextPos at) {   // s134
    if (at.para < 0 || at.para >= static_cast<int>(s.paras.size())) return {};
    const auto& para = s.paras[static_cast<std::size_t>(at.para)];
    for (const int b : {at.byte, at.byte - 3}) {
        if (b < 0 || static_cast<std::size_t>(b) + 3 > para.text.size()) continue;
        if (para.text.compare(static_cast<std::size_t>(b), 3, kMarkIndex) != 0) continue;
        const int k = marks_in(para.text, kMarkIndex, 0, static_cast<std::size_t>(b));
        if (k < static_cast<int>(para.index.size())) return {at.para, k, b};
    }
    return {};
}

bool set_index_mark(DocJournal& j, Doc& d, const Doc::Id& story, int para, int k, const std::string& term,
                    const std::string& sub) {   // s134
    const auto* st = d.as<Story>(story);
    if (!st || para < 0 || para >= static_cast<int>(st->paras.size()) || k < 0 ||
        k >= static_cast<int>(st->paras[static_cast<std::size_t>(para)].index.size()))
        return false;
    IndexMark m = st->paras[static_cast<std::size_t>(para)].index[static_cast<std::size_t>(k)];   // its see kept
    m.term = term;
    m.sub = sub;
    return set_index_mark(j, d, story, para, k, m);
}

bool set_index_mark(DocJournal& j, Doc& d, const Doc::Id& story, int para, int k, const IndexMark& mark) {   // s136
    const auto* st = d.as<Story>(story);
    const auto want = tidy(mark);
    if (!st || want.term.empty() || para < 0 || para >= static_cast<int>(st->paras.size()) || k < 0 ||
        k >= static_cast<int>(st->paras[static_cast<std::size_t>(para)].index.size()))
        return false;
    Story s = *st;
    auto& m = s.paras[static_cast<std::size_t>(para)].index[static_cast<std::size_t>(k)];
    if (m == want) return false;
    m = want;
    j.run("Index Entry", {story}, [&] { d.put(story, s); });
    return true;
}

bool insert_citation(DocJournal& j, Doc& d, const Doc::Id& story, TextPos at, const Doc::Id& to_story, int to_para) {   // s141
    return insert_citation(j, d, story, at, std::vector<SourceAt>{{to_story, to_para}});
}

bool insert_citation(DocJournal& j, Doc& d, const Doc::Id& story, TextPos at, const std::vector<SourceAt>& sources,
                     const std::string& locator) {   // s143
    std::vector<SourceAt> ok;
    for (const auto& s : sources)
        if (const auto* ts = d.as<Story>(s.story); ts && source_number(d, *ts, s.para) > 0 &&
                                                   std::none_of(ok.begin(), ok.end(), [&](const SourceAt& o) { return o.story == s.story && o.para == s.para; }))
            ok.push_back(s);
    if (ok.empty() || !d.as<Story>(story)) return false;
    std::vector<Doc::Id> ids{story};
    for (const auto& s : ok) if (std::find(ids.begin(), ids.end(), s.story) == ids.end()) ids.push_back(s.story);
    const auto loc = trim_ws(locator);
    j.run("Insert Citation", ids, [&] {
        // Each source's anchor (made at its start if none); the reference to the first, the others with it.
        std::vector<std::string> anchors;
        for (const auto& s : ok) {
            std::string id = anchor_for(*d.as<Story>(s.story), s.para, -1);
            if (id.empty()) {
                id = insert_anchor(j, d, s.story, {s.para, 0});
                if (s.story == story && at.para == s.para) at.byte += 3;   // it went in before the place
            }
            anchors.push_back(id);
        }
        insert_ref(j, d, story, at, anchors.front(), RefShow::Cite, loc);
        if (anchors.size() > 1) {
            Story st = *d.as<Story>(story);
            auto& para = st.paras[static_cast<std::size_t>(at.para)];
            const int k = marks_in(para.text, kMarkRef, 0, static_cast<std::size_t>(at.byte));
            para.refs[static_cast<std::size_t>(k)].more.assign(anchors.begin() + 1, anchors.end());
            d.put(story, st);
        }
    });
    return true;
}

bool add_cite_source(DocJournal& j, Doc& d, const Doc::Id& story, int para, int k, const Doc::Id& to_story, int to_para) {   // s143
    const auto* st = d.as<Story>(story);
    const auto* ts = d.as<Story>(to_story);
    if (!st || !ts || para < 0 || para >= static_cast<int>(st->paras.size()) || k < 0 ||
        k >= static_cast<int>(st->paras[static_cast<std::size_t>(para)].refs.size()) || source_number(d, *ts, to_para) == 0)
        return false;
    const auto& r = st->paras[static_cast<std::size_t>(para)].refs[static_cast<std::size_t>(k)];
    if (r.show != RefShow::Cite) return false;
    std::string id = anchor_for(*ts, to_para, -1);
    if (!id.empty() && (id == r.target || std::find(r.more.begin(), r.more.end(), id) != r.more.end())) return false;
    j.run("Add Source to Citation", {story, to_story}, [&] {
        if (id.empty()) id = insert_anchor(j, d, to_story, {to_para, 0});   // (refs are counted: k holds)
        Story s = *d.as<Story>(story);
        s.paras[static_cast<std::size_t>(para)].refs[static_cast<std::size_t>(k)].more.push_back(id);
        d.put(story, s);
    });
    return true;
}

int note_mark_byte(const Story& s, int p, int k) {   // s113
    if (p < 0 || p >= static_cast<int>(s.paras.size()) || k < 0) return -1;
    const auto& t = s.paras[static_cast<std::size_t>(p)].text;
    int i = 0;
    for (std::size_t at = t.find(kMarkNote); at != std::string::npos; at = t.find(kMarkNote, at + 3), ++i)
        if (i == k) return static_cast<int>(at);
    return -1;
}

int change_note_words(DocJournal& j, Doc& d, const Doc::Id& story, int p, int k, int a, int b, bool set_text,
                      const std::string& text, const Doc::Id& cstyle, const std::string& label,
                      const std::function<void(Local&)>& fold) {   // s113
    const auto* st = d.as<Story>(story);
    if (!st || p < 0 || p >= static_cast<int>(st->paras.size())) return -1;
    const auto* sty = cstyle.empty() ? nullptr : d.as<Style>(cstyle);
    if (!cstyle.empty() && (!sty || sty->kind != StyleKind::Character)) return -1;
    Story s = *st;
    auto& para = s.paras[static_cast<std::size_t>(p)];
    fit_notes(para);
    if (k < 0 || k >= static_cast<int>(para.notes.size())) return -1;
    auto& note = para.notes[static_cast<std::size_t>(k)];
    const Para np{{}, note.text, note.runs};
    const int n = static_cast<int>(np.text.size());
    a = std::clamp(a, 0, n);
    b = std::clamp(b, 0, n);
    if (b < a) std::swap(a, b);
    std::vector<Span> sp = spans_of(np, 0, a);
    int end = b;
    if (set_text) {
        const std::string t = note_breaks(text);   // s116: paragraphs kept
        const int at = (b != a && a < n) ? a + 1 : a;  // the first replaced character's style, else the one before
        sp.push_back({t, char_style_at(np, at), local_at(np, at)});
        end = a + static_cast<int>(t.size());
    } else {
        const auto mid = spans_of(np, a, b);
        sp.insert(sp.end(), mid.begin(), mid.end());
    }
    if (!cstyle.empty() && end > a) {   // the stretch as it now is, in the style
        std::vector<Span> head;
        int at = 0;
        for (auto x : sp) {
            const int x0 = at, x1 = at + static_cast<int>(x.text.size());
            at = x1;
            if (x1 <= a || x0 >= end) { head.push_back(x); continue; }
            // Split at a and end.
            const auto piece = [&](int u, int v, bool styled) {
                if (v <= u) return;
                Span y = x;
                y.text = x.text.substr(static_cast<std::size_t>(u - x0), static_cast<std::size_t>(v - u));
                if (styled) {
                    y.style = cstyle;
                    if (fold) fold(y.local);
                }
                head.push_back(y);
            };
            piece(x0, std::max(x0, a), false);
            piece(std::max(x0, a), std::min(x1, end), true);
            piece(std::min(x1, end), x1, false);
        }
        sp = std::move(head);
    }
    const auto rest = spans_of(np, b, n);
    sp.insert(sp.end(), rest.begin(), rest.end());
    const Para made = para_of({}, sp);
    if (made.text == note.text && made.runs == note.runs) return -1;
    note.text = made.text;
    note.runs = made.runs;
    j.run(label, {story}, [&] { d.put(story, s); });
    return end;
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

namespace {
struct ScreenHook {
    std::mutex                        mx;
    std::shared_ptr<const ScreenCmyk> f;
    std::atomic<std::uint64_t>        gen{1};
};
ScreenHook& screen_hook() {
    static ScreenHook h;
    return h;
}
}  // namespace

void set_screen_cmyk(ScreenCmyk f) {   // s107
    auto& h = screen_hook();
    std::lock_guard lk(h.mx);
    h.f = f ? std::make_shared<const ScreenCmyk>(std::move(f)) : nullptr;
    ++h.gen;
}

std::uint64_t screen_gen() { return screen_hook().gen.load(); }

std::uint32_t screen_rgb(const Swatch& s) {   // s107
    if (s.model != ColorModel::CMYK && s.model != ColorModel::Spot) return s.preview;
    std::shared_ptr<const ScreenCmyk> f;
    {
        auto& h = screen_hook();
        std::lock_guard lk(h.mx);
        f = h.f;
    }
    if (!f) return s.preview;
    const double v[4] = {s.v[0], s.v[1], s.v[2], s.v[3]};
    return (*f)(v);
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

Doc::Id take_colour_onto(DocJournal& j, Doc& d, const std::vector<Doc::Id>& frames, std::uint32_t rgb, bool stroke) {
    rgb &= 0xFFFFFFu;
    const auto ch = [](std::uint32_t c, int k) { return static_cast<int>((c >> k) & 0xFF); };
    Doc::Id sw;
    int best = 4;   // at most 1 a channel (3 in all) counts as the same colour
    for (const auto& id : d.ids_of<Swatch>()) {
        const auto c = doc::screen_rgb(*d.as<Swatch>(id));
        const int diff = std::abs(ch(c, 16) - ch(rgb, 16)) + std::abs(ch(c, 8) - ch(rgb, 8)) + std::abs(ch(c, 0) - ch(rgb, 0));
        if (std::max({std::abs(ch(c, 16) - ch(rgb, 16)), std::abs(ch(c, 8) - ch(rgb, 8)), std::abs(ch(c, 0) - ch(rgb, 0))}) <= 1 &&
            diff < best) { best = diff; sw = id; }
    }
    std::vector<Doc::Id> touch;
    for (const auto& id : frames) {
        const auto* f = d.as<Frame>(id);
        if (f && (sw.empty() || (stroke ? f->stroke : f->fill) != sw)) touch.push_back(id);
    }
    if (!sw.empty() && touch.empty()) return sw;
    char name[16];
    std::snprintf(name, sizeof name, "#%06X", rgb);
    const bool fresh = sw.empty();
    j.run(touch.empty() ? std::string("New swatch ") + name : stroke ? "Stroke from page" : "Fill from page", touch,
          [&]() -> std::vector<Doc::Id> {
              std::vector<Doc::Id> made;
              if (fresh) {
                  Swatch s{name, 0, ColorModel::RGB, {double(ch(rgb, 16)), double(ch(rgb, 8)), double(ch(rgb, 0)), 0}};
                  s.preview = swatch_preview(s);
                  double pos = 0;
                  for (const auto& id : d.ids_of<Swatch>()) pos = std::max(pos, d.as<Swatch>(id)->pos);
                  s.pos = pos + 1;
                  sw = d.fresh_id("swatch");
                  d.put(sw, s);
                  made.push_back(sw);
              }
              for (const auto& id : touch) {
                  Frame f = *d.as<Frame>(id);
                  (stroke ? f.stroke : f.fill) = sw;
                  if (stroke && f.stroke_w <= 0) f.stroke_w = 1;
                  d.put(id, f);
              }
              return made;
          });
    return sw;
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

std::vector<Doc::Id> unused_swatches(const Doc& d) {
    std::vector<std::pair<double, Doc::Id>> v;
    for (const auto& id : d.ids_of<Swatch>()) {
        const auto* s = d.as<Swatch>(id);
        if (s->name == "Black" || s->name == "Paper" || swatch_uses(d, id) > 0) continue;
        v.push_back({s->pos, id});
    }
    std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<Doc::Id> out;
    for (auto& [p, id] : v) out.push_back(id);
    return out;
}

int delete_unused_swatches(DocJournal& j, Doc& d) {
    const auto gone = unused_swatches(d);
    if (gone.empty()) return 0;
    const std::string label = gone.size() == 1 ? "Delete swatch " + d.as<Swatch>(gone.front())->name
                                               : "Delete " + std::to_string(gone.size()) + " unused swatches";
    j.run(label, gone, [&] { for (const auto& id : gone) d.put(id, std::nullopt); });
    return static_cast<int>(gone.size());
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
    if (f.vector) {   // s188: its own size (CSS pixels, 96 to the inch) as points
        a.vec_w = f.px_w * 72.0 / 96.0;
        a.vec_h = f.px_h * 72.0 / 96.0;
    }
    a.stamp = f.stamp;
    a.shape = f.shape;   // s173
    a.clip = f.clip;     // s189
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
    if (!f || (f->kind != FrameKind::Image && f->kind != FrameKind::Shape) || file.path.empty()) return false;   // s210: a shape takes a picture too
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
        } else if (!file.shape.empty() && d.as<Asset>(a)->shape.empty()) {   // s175: an outline it didn't have
            Asset t = *d.as<Asset>(a);
            t.shape = file.shape;
            d.put(a, t);
        }
        Frame nf = *d.as<Frame>(frame);
        nf.asset = a;
        nf.kind = FrameKind::Image;   // s210: a shape placed into is a picture frame now
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

}  // namespace doc

// ── s189 (G3): a Photoshop clipping path ─────────────────────────────────────
namespace {
unsigned cp_u8(const std::string& s, std::size_t i) { return i < s.size() ? static_cast<unsigned char>(s[i]) : 0u; }
std::uint32_t cp_be16(const std::string& s, std::size_t i) { return (cp_u8(s, i) << 8) | cp_u8(s, i + 1); }
std::uint32_t cp_be32(const std::string& s, std::size_t i) { return (cp_be16(s, i) << 16) | cp_be16(s, i + 2); }
// The image resource blocks: id -> (name, data).
struct Res { unsigned id; std::string name, data; };
std::vector<Res> resource_blocks(const std::string& b) {
    std::vector<Res> out;
    std::size_t at = 0;
    while (at + 12 <= b.size() && b.compare(at, 4, "8BIM") == 0) {
        Res r;
        r.id = cp_be16(b, at + 4);
        const std::size_t nl = cp_u8(b, at + 6);
        r.name = b.substr(at + 7, nl);
        std::size_t p = at + 6 + ((nl + 2) & ~std::size_t{1});   // the pascal name, padded to even
        if (p + 4 > b.size()) break;
        const std::size_t len = cp_be32(b, p);
        p += 4;
        if (p + len > b.size()) break;
        r.data = b.substr(p, len);
        out.push_back(std::move(r));
        at = p + ((len + 1) & ~std::size_t{1});
    }
    return out;
}
// The resource section of a PSD / JPEG / TIFF, "" when none.
std::string photoshop_resources(const ReadAt& read) {
    const std::string h = read(0, 32);
    if (h.size() >= 26 && h.compare(0, 4, "8BPS") == 0) {
        const std::uint64_t cm = cp_be32(read(26, 4), 0);
        const std::uint64_t at = 30 + cm;
        const std::uint32_t len = cp_be32(read(at, 4), 0);
        return len > (1u << 28) ? std::string{} : read(at + 4, len);
    }
    if (h.size() >= 4 && cp_u8(h, 0) == 0xFF && cp_u8(h, 1) == 0xD8) {   // JPEG: every APP13 Photoshop segment, joined
        std::string all;
        std::uint64_t at = 2;
        for (int guard = 0; guard < 4096; ++guard) {
            const std::string m = read(at, 4);
            if (m.size() < 4 || cp_u8(m, 0) != 0xFF) break;
            const unsigned code = cp_u8(m, 1);
            if (code == 0xFF) { ++at; continue; }
            if (code == 0xD8 || code == 0x01 || (code >= 0xD0 && code <= 0xD7)) { at += 2; continue; }
            if (code == 0xD9 || code == 0xDA) break;
            const unsigned len = cp_be16(m, 2);
            if (len < 2) break;
            if (code == 0xED) {
                const std::string seg = read(at + 4, len - 2);
                static const std::string sig("Photoshop 3.0\0", 14);
                if (seg.compare(0, sig.size(), sig) == 0) all += seg.substr(sig.size());
            }
            at += 2 + len;
        }
        return all;
    }
    if (h.size() >= 8 && ((h[0] == 'I' && h[1] == 'I') || (h[0] == 'M' && h[1] == 'M'))) {   // TIFF tag 34377
        const bool le = h[0] == 'I';
        const auto num = [&](const std::string& s, std::size_t i, int n) {
            std::uint64_t v = 0;
            for (int k = 0; k < n; ++k) v |= static_cast<std::uint64_t>(cp_u8(s, i + static_cast<std::size_t>(k))) << (8 * (le ? k : n - 1 - k));
            return v;
        };
        if (num(h, 2, 2) != 42) return {};
        const std::uint64_t ifd = num(h, 4, 4);
        const std::size_t n = static_cast<std::size_t>(num(read(ifd, 2), 0, 2));
        const std::string e = read(ifd + 2, n * 12);
        for (std::size_t i = 0; i + 12 <= e.size(); i += 12)
            if (num(e, i, 2) == 34377) {
                const std::uint64_t count = num(e, i + 4, 4);
                if (count > (1u << 28)) return {};
                return count <= 4 ? e.substr(i + 8, static_cast<std::size_t>(count)) : read(num(e, i + 8, 4), static_cast<std::size_t>(count));
            }
    }
    return {};
}
}  // namespace

ClipPath clipping_path(const ReadAt& read) {
    ClipPath out;
    const auto blocks = resource_blocks(photoshop_resources(read));
    std::string want;
    for (const auto& r : blocks)
        if (r.id == 2999 && !r.data.empty()) want = r.data.substr(1, cp_u8(r.data, 0));   // its name, a pascal string
    if (want.empty()) return out;
    const Res* path = nullptr;
    for (const auto& r : blocks)
        if (r.id >= 2000 && r.id <= 2997 && r.name == want) { path = &r; break; }
    if (!path) return out;
    out.name = want;
    const auto fixed = [&](std::size_t i) { return static_cast<double>(static_cast<std::int32_t>(cp_be32(path->data, i))) / 16777216.0; };
    struct Knot { double px, py, ax, ay, lx, ly; };   // preceding control, anchor, leaving control
    std::vector<Knot> knots;
    int left = 0;
    bool closed = false;
    const auto finish = [&] {
        if (closed && knots.size() >= 2) {
            std::vector<std::array<double, 2>> poly;
            for (std::size_t i = 0; i < knots.size(); ++i) {
                const Knot& a = knots[i];
                const Knot& b = knots[(i + 1) % knots.size()];
                for (int s = 0; s < 16; ++s) {   // a cubic from a's anchor, by a's leaving and b's preceding controls
                    const double t = s / 16.0, u = 1 - t;
                    poly.push_back({u * u * u * a.ax + 3 * u * u * t * a.lx + 3 * u * t * t * b.px + t * t * t * b.ax,
                                    u * u * u * a.ay + 3 * u * u * t * a.ly + 3 * u * t * t * b.py + t * t * t * b.ay});
                }
            }
            out.polys.push_back(std::move(poly));
        }
        knots.clear();
    };
    for (std::size_t at = 0; at + 26 <= path->data.size(); at += 26) {
        const unsigned sel = cp_be16(path->data, at);
        if (sel == 0 || sel == 3) {   // a subpath's length record
            finish();
            closed = sel == 0;
            left = static_cast<int>(cp_be16(path->data, at + 2));
        } else if ((sel == 1 || sel == 2 || sel == 4 || sel == 5) && left > 0) {
            knots.push_back({fixed(at + 6), fixed(at + 2), fixed(at + 14), fixed(at + 10), fixed(at + 22), fixed(at + 18)});   // (vert, horiz) pairs
            --left;
        }
    }
    finish();
    return out;
}

std::vector<float> clip_shape(const ClipPath& p, double w, double h, int bands) {
    if (p.polys.empty() || w <= 0 || h <= 0) return {};
    const int W = 400, H = std::clamp(static_cast<int>(std::lround(W * h / w)), 1, 4000);
    std::vector<std::uint32_t> px(static_cast<std::size_t>(W) * static_cast<std::size_t>(H), 0u);
    std::vector<double> xs;
    for (int y = 0; y < H; ++y) {
        const double fy = (y + 0.5) / H;
        xs.clear();
        for (const auto& poly : p.polys)
            for (std::size_t i = 0; i < poly.size(); ++i) {
                const auto& a = poly[i];
                const auto& b = poly[(i + 1) % poly.size()];
                if ((a[1] <= fy) == (b[1] <= fy)) continue;
                xs.push_back(a[0] + (fy - a[1]) / (b[1] - a[1]) * (b[0] - a[0]));
            }
        std::sort(xs.begin(), xs.end());
        for (std::size_t k = 0; k + 1 < xs.size(); k += 2) {   // even-odd
            const int x0 = std::clamp(static_cast<int>(std::lround(xs[k] * W)), 0, W);
            const int x1 = std::clamp(static_cast<int>(std::lround(xs[k + 1] * W)), 0, W);
            for (int x = x0; x < x1; ++x) px[static_cast<std::size_t>(y) * W + static_cast<std::size_t>(x)] = 0xFF000000u;
        }
    }
    auto s = trace_shape(px.data(), W, H, W, bands);
    if (!s.empty()) s[0] = static_cast<float>(w / h);   // the picture's own aspect, not the mask's rounding
    return s;
}

std::vector<float> pack_clip(const ClipPath& p) {   // s189
    std::vector<float> out;
    for (const auto& poly : p.polys) {
        if (poly.size() < 3) continue;
        out.push_back(static_cast<float>(poly.size()));
        for (const auto& q : poly) { out.push_back(static_cast<float>(q[0])); out.push_back(static_cast<float>(q[1])); }
    }
    return out;
}

std::vector<std::vector<std::array<double, 2>>> clip_polys(const Asset& a, const Rect& pr) {   // s189
    std::vector<std::vector<std::array<double, 2>>> out;
    std::size_t i = 0;
    while (i < a.clip.size()) {
        const auto n = static_cast<std::size_t>(a.clip[i++]);
        if (n == 0 || i + 2 * n > a.clip.size()) break;
        std::vector<std::array<double, 2>> poly;
        for (std::size_t k = 0; k < n; ++k, i += 2) poly.push_back({pr.x + a.clip[i] * pr.w, pr.y + a.clip[i + 1] * pr.h});
        out.push_back(std::move(poly));
    }
    return out;
}

// ── s184 (G5): a picture's colour space from its header ──────────────────────
std::string picture_color(const ReadAt& read) {
    const std::string h = read(0, 32);
    const auto u8 = [](const std::string& s, std::size_t i) { return i < s.size() ? static_cast<unsigned>(static_cast<unsigned char>(s[i])) : 0u; };
    if (h.size() >= 3 && u8(h, 0) == 0xFF && u8(h, 1) == 0xD8) {   // JPEG: marker to marker, to the frame header
        std::uint64_t at = 2;
        for (int guard = 0; guard < 4096; ++guard) {
            const std::string m = read(at, 4);
            if (m.size() < 4 || u8(m, 0) != 0xFF) return "";
            const unsigned code = u8(m, 1);
            if (code == 0xFF) { ++at; continue; }   // fill byte
            if (code == 0xD8 || code == 0x01 || (code >= 0xD0 && code <= 0xD7)) { at += 2; continue; }   // no length
            if (code == 0xD9 || code == 0xDA) return "";   // end / scan before a frame: not a picture we know
            const unsigned len = (u8(m, 2) << 8) | u8(m, 3);
            if (len < 2) return "";
            if (code >= 0xC0 && code <= 0xCF && code != 0xC4 && code != 0xC8 && code != 0xCC) {   // SOFn
                const std::string f = read(at + 4, 6);
                const unsigned n = u8(f, 5);   // precision, height(2), width(2), components
                return n == 1 ? "Gray" : n == 4 ? "CMYK" : n == 3 ? "RGB" : "";
            }
            at += 2 + len;
        }
        return "";
    }
    if (h.size() >= 26 && h.compare(1, 3, "PNG") == 0 && u8(h, 0) == 0x89) {
        const unsigned ct = u8(h, 25);   // IHDR's colour type
        return ct == 0 || ct == 4 ? "Gray" : "RGB";
    }
    if (h.size() >= 26 && h.compare(0, 4, "8BPS") == 0) {   // Photoshop
        switch ((u8(h, 24) << 8) | u8(h, 25)) {
        case 0: case 1: case 8: return "Gray";   // bitmap, grayscale, duotone
        case 4: return "CMYK";
        case 9: return "Lab";
        default: return "RGB";   // RGB, indexed, multichannel
        }
    }
    if (h.size() >= 8 && ((h[0] == 'I' && h[1] == 'I' && u8(h, 2) == 42 && u8(h, 3) == 0) ||
                          (h[0] == 'M' && h[1] == 'M' && u8(h, 2) == 0 && u8(h, 3) == 42))) {   // TIFF
        const bool le = h[0] == 'I';
        const auto num = [&](const std::string& s, std::size_t i, int n) {
            std::uint64_t v = 0;
            for (int k = 0; k < n; ++k) v |= static_cast<std::uint64_t>(u8(s, i + static_cast<std::size_t>(k))) << (8 * (le ? k : n - 1 - k));
            return v;
        };
        const std::uint64_t ifd = num(h, 4, 4);
        const std::string cnt = read(ifd, 2);
        if (cnt.size() < 2) return "";
        const std::size_t n = static_cast<std::size_t>(num(cnt, 0, 2));
        const std::string e = read(ifd + 2, n * 12);
        long photometric = -1, samples = 1;
        for (std::size_t i = 0; i + 12 <= e.size(); i += 12) {
            const auto tag = num(e, i, 2), type = num(e, i + 2, 2);
            const auto v = type == 3 ? num(e, i + 8, 2) : num(e, i + 8, 4);   // SHORT or LONG, inline
            if (tag == 262) photometric = static_cast<long>(v);
            if (tag == 277) samples = static_cast<long>(v);
        }
        switch (photometric) {
        case 0: case 1: return samples >= 3 ? "RGB" : "Gray";
        case 5: return "CMYK";   // separated (ink set 1)
        case 8: case 9: case 10: return "Lab";
        case -1: return "";
        default: return "RGB";   // RGB, palette, YCbCr ...
        }
    }
    if (h.size() >= 6 && (h.compare(0, 6, "GIF87a") == 0 || h.compare(0, 6, "GIF89a") == 0)) return "RGB";
    if (h.size() >= 2 && h[0] == 'B' && h[1] == 'M') return "RGB";
    if (h.size() >= 12 && h.compare(0, 4, "RIFF") == 0 && h.compare(8, 4, "WEBP") == 0) return "RGB";
    return "";
}

std::string picture_color(const std::string& bytes) {
    return picture_color([&bytes](std::uint64_t off, std::size_t n) {
        return off >= bytes.size() ? std::string{} : bytes.substr(static_cast<std::size_t>(off), n);
    });
}

namespace doc {

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
            const auto asp = picture_aspect(a);   // s188
    const double iw = asp.w, ih = asp.h;
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

bool set_picture_offset(DocJournal& j, Doc& d, const std::vector<Doc::Id>& ids, int axis, double pts) {   // s076
    if (axis < 0 || axis > 1) return false;
    std::vector<Doc::Id> frames;
    for (const auto& id : ids)
        if (const auto* f = d.as<Frame>(id); f && f->kind == FrameKind::Image && d.as<Asset>(f->asset)) {
            const auto& a = *d.as<Asset>(f->asset);
            const Rect now = picture_rect({0, 0, f->w, f->h}, *f, picture_aspect(a).w, picture_aspect(a).h);
            if (f->fit != ImageFit::Custom || std::abs((axis ? now.y : now.x) - pts) > 1e-9) frames.push_back(id);
        }
    if (frames.empty()) return false;
    return j.run("Picture position", frames, [&] {
        for (const auto& id : frames) {
            Frame f = *d.as<Frame>(id);
            const auto& a = *d.as<Asset>(f.asset);
            const Rect now = picture_rect({0, 0, f.w, f.h}, f, picture_aspect(a).w, picture_aspect(a).h);
            f.fit = ImageFit::Custom;
            f.pic_w = now.w;
            f.pic_x = axis == 0 ? pts : now.x;
            f.pic_y = axis == 1 ? pts : now.y;
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
        {"page_margins", [](DocJournal& j, Doc& d) {   // s062
             edit_page_margins(j, d, {d.pages().back()}, "Top margin", [](Margins& m) { m.top = 90; });
         }},
        {"rename_section", [](DocJournal& j, Doc& d) { rename_section(j, d, d.ids_of<Section>().back(), "Part One"); }},   // s060
        {"set_title", [](DocJournal& j, Doc& d) { set_title(j, d, "The Outer Ports"); }},   // s066
        {"set_trim", [](DocJournal& j, Doc& d) { set_trim(j, d, 5.5 * kPt, 8.5 * kPt); }},
        {"set_bleed", [](DocJournal& j, Doc& d) { set_bleed(j, d, d.settings().bleed + 9); }},
        {"set_master_size", [](DocJournal& j, Doc& d) { set_master_size(j, d, d.ids_of<Master>().front(), 8.5 * kPt, 11 * kPt); }},   // s147
        {"set_master_bleed", [](DocJournal& j, Doc& d) { set_master_bleed(j, d, d.ids_of<Master>().front(), 18); }},
        {"set_master_facing", [](DocJournal& j, Doc& d) { set_master_facing(j, d, d.ids_of<Master>().front(), false); }},
        {"set_facing", [](DocJournal& j, Doc& d) { set_facing(j, d, !d.settings().facing); }},
        {"set_wrap_beneath", [](DocJournal& j, Doc& d) { set_wrap_beneath(j, d, true); }},   // s069
        {"set_note_numbering", [](DocJournal& j, Doc& d) { set_note_numbering(j, d, NoteRestart::Page, NoteStyle::Symbols); }},   // s111
        {"set_note_area", [](DocJournal& j, Doc& d) { set_note_area(j, d, 50); }},   // s117
        {"set_note_look", [](DocJournal& j, Doc& d) { set_note_look(j, d, 100, 1, 3, "(continued)"); }},   // s118
        {"set_note_place", [](DocJournal& j, Doc& d) { set_note_place(j, d, NoteAt::StoryEnd, "Endnotes"); }},   // s120
        {"set_note_num", [](DocJournal& j, Doc& d) { set_note_num(j, d, Settings::NoteNum::Period); }},   // s126
        {"insert_anchor", [](DocJournal& j, Doc& d) { insert_anchor(j, d, "story.1", {4, 0}); }},   // s128
        {"insert_ref_to", [](DocJournal& j, Doc& d) { insert_ref_to(j, d, "story.1", {1, 4}, "story.1", 4, RefShow::TextPage); }},
        // s130: the sample has no reference -- one made and changed, as one step (the gate checks the step).
        {"set_ref", [](DocJournal& j, Doc& d) {
            j.run("t", {"story.1"}, [&] { insert_ref(j, d, "story.1", {1, 4}, "", RefShow::Page); set_ref(j, d, "story.1", 1, 0, RefShow::Custom, "p. #"); });
        }},
        {"retarget_ref", [](DocJournal& j, Doc& d) {
            j.run("t", {"story.1"}, [&] { insert_ref(j, d, "story.1", {1, 4}, "", RefShow::Page); retarget_ref(j, d, "story.1", 1, 0, "story.1", 4, -1, RefShow::Text); });
        }},
        {"insert_index_mark", [](DocJournal& j, Doc& d) { insert_index_mark(j, d, "story.1", {1, 4}, "Harbors", "smells"); }},   // s134
        {"set_index_mark", [](DocJournal& j, Doc& d) {   // s134: the sample has none -- made and changed in one step
            j.run("t", {"story.1"}, [&] { insert_index_mark(j, d, "story.1", {1, 4}, "Harbors"); set_index_mark(j, d, "story.1", 1, 0, "Ports", "fees"); });
        }},
        {"add_list", [](DocJournal& j, Doc& d) { add_list(j, d, ListKind::Index, "Index", d.pages().back()); }},   // s135
        {"set_list_cite_order", [](DocJournal& j, Doc& d) { set_list_cite_order(j, d, "list.2", true); }},   // s142
        {"insert_citation_several", [](DocJournal& j, Doc& d) {   // s143
            insert_citation(j, d, "story.1", {1, 4}, std::vector<SourceAt>{{"story.4", 0}, {"story.4", 2}}, "p. 14");
        }},
        {"add_cite_source", [](DocJournal& j, Doc& d) {   // s143: one made, then one added -- in one step
            j.run("t", {"story.1", "story.4"}, [&] { insert_citation(j, d, "story.1", {1, 4}, "story.4", 0); add_cite_source(j, d, "story.1", 1, 0, "story.4", 2); });
        }},
        {"insert_citation", [](DocJournal& j, Doc& d) { insert_citation(j, d, "story.1", {1, 4}, "story.4", 1); }},   // s141: the sample's Sources
        {"insert_ref_to_note", [](DocJournal& j, Doc& d) { insert_ref_to_note(j, d, "story.1", {1, 4}, "story.1", 5, 0, RefShow::Custom, "note *"); }},   // s129: the sample's note
        {"set_units", [](DocJournal& j, Doc& d) { set_units(j, d, Units::Millimetres); }},   // s073
        {"set_ruler_origin", [](DocJournal& j, Doc& d) { set_ruler_origin(j, d, RulerOrigin::Page); }},   // s154
        {"set_ruler_zero", [](DocJournal& j, Doc& d) { set_ruler_zero(j, d, 36, 18); }},
        {"override_master_items", [](DocJournal& j, Doc& d) {   // s169
            const auto pl = layout_pages(d, d.pages());
            override_master_items(j, d, master_items_in(d, pl, {-1e5, -1e5, 2e5, 2e5}));
        }},
        {"styles_as_one", [](DocJournal& j, Doc& d) {   // s162
            const auto st = d.ids_of<Style>();
            styles_as_one(j, d, "Style settings", [&] {
                edit_styles(j, d, {st.front()}, "Size", [](Style& s) { s.size = 13; });
                edit_styles(j, d, {st.front()}, "Leading", [](Style& s) { s.leading = 17; });
            });
        }},
        {"set_para_tabs", [](DocJournal& j, Doc& d) {   // s157
            for (const auto& sid : d.ids_of<Story>()) if (!d.as<Story>(sid)->paras.empty()) { set_para_tabs(j, d, sid, {0, 0}, {0, 0}, "Add tab", {{100, TabAlign::Left, ""}}); return; }
        }},
        {"set_shows_on", [](DocJournal& j, Doc& d) {   // s155
            for (const auto& m : d.ids_of<Master>())
                if (d.as<Master>(m)->facing && !d.frames_on(m).empty()) { set_shows_on(j, d, {d.frames_on(m).front()}, 3); return; }
        }},
        {"save_query", [](DocJournal& j, Doc& d) { save_query(j, d, {"Dashes", "--", "\u2014"}); }},   // s078
        {"delete_query", [](DocJournal& j, Doc& d) { delete_query(j, d, "Double spaces"); }},
        {"set_picture_offset", [](DocJournal& j, Doc& d) { set_picture_offset(j, d, {"frame.11"}, 0, -6); }},   // s076
        {"arrange_back", [](DocJournal& j, Doc& d) { arrange(j, d, {"frame.11"}, Arrange::Back); }},   // s070
        {"arrange_forward", [](DocJournal& j, Doc& d) { arrange(j, d, {"frame.1"}, Arrange::Forward); }},
        {"ignore_wrap", [](DocJournal& j, Doc& d) {
             edit_frames(j, d, {"frame.1"}, "Ignore text wrap", [](Frame& f) { f.ignore_wrap = true; });
         }},
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
        {"add_oval", [](DocJournal& j, Doc& d) { add_frame(j, d, "page.8", FrameKind::Shape, {54, 400, 50, 30}, 1, 12, false, ShapeKind::Oval); }},   // s197
        {"add_line", [](DocJournal& j, Doc& d) { add_path(j, d, "page.8", {corner_point(54, 300), corner_point(200, 300)}, true); }},
        {"add_path", [](DocJournal& j, Doc& d) {
             add_path(j, d, "page.8", {{54, 300, 54, 300, 80, 250, true}, {200, 330, 170, 360, 200, 330, false}, corner_point(120, 400)}, false); }},
        {"delete_frames", [](DocJournal& j, Doc& d) { delete_frames(j, d, {"frame.2", "frame.4"}); }},  // mid-thread
        {"override_master_item", [](DocJournal& j, Doc& d) { override_master_item(j, d, "page.7", "frame.13"); }},   // s025
        {"remove_overrides", [](DocJournal& j, Doc& d) { remove_overrides(j, d, {"frame.15"}); }},
        {"override_all", [](DocJournal& j, Doc& d) { override_all(j, d, {"page.7", "page.8"}); }},   // s072
        {"remove_all_overrides", [](DocJournal& j, Doc& d) { remove_all_overrides(j, d, d.pages()); }},
        {"set_para_local", [](DocJournal& j, Doc& d) {   // s093
             set_para_local(j, d, "story.1", {1, 0}, {2, 0}, "Align", [](ParaLocal& o) { o.align = Align::Center; });
         }},
        {"clear_para_local", [](DocJournal& j, Doc& d) {   // s093: the sample's centred dedication
             clear_para_local(j, d, "story.3", {0, 0}, {0, 0}, {"align"});
         }},
        {"reset_style_fields", [](DocJournal& j, Doc& d) { reset_style_fields(j, d, {"style.10"}, {"first_indent"}); }},   // s092
        {"set_list_entry_style", [](DocJournal& j, Doc& d) { set_list_entry_style(j, d, "list.1", 1, "style.3"); }},   // s090
        {"set_list_numbers", [](DocJournal& j, Doc& d) { set_list_numbers(j, d, "list.1", false); }},                 // s090
        {"set_list_styles", [](DocJournal& j, Doc& d) { set_list_styles(j, d, "list.1", {"style.2", "style.3"}); }},   // s088
        {"build_list", [](DocJournal& j, Doc& d) {   // s087
             build_list(j, d, "list.1", {{"Chapter One", "1"}, {"Chapter Two", "3"}});
         }},
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
        {"insert_note", [](DocJournal& j, Doc& d) { insert_note(j, d, "story.1", {1, 4}, "A note."); }},   // s109
        {"set_note_text", [](DocJournal& j, Doc& d) { set_note_text(j, d, "story.1", 5, 0, "The foldout opens left."); }},   // s114: the sample's note
        {"change_note_words", [](DocJournal& j, Doc& d) {   // s113
             change_note_words(j, d, "story.1", 5, 0, 4, 11, true, "map", "style.5", "Change");
         }},

        // s014: master margins / columns.
        {"edit_masters", [](DocJournal& j, Doc& d) {
             edit_masters(j, d, {"master.2", "master.3"}, "Margins", [](Master& m) { m.margins.top = 72; m.columns = 2; });
         }},
        // s012: columns.
        {"edit_frames", [](DocJournal& j, Doc& d) {
             edit_frames(j, d, {"frame.1", "frame.10"}, "Columns", [](Frame& f) { f.columns = 2; f.gutter = 18; f.widths = {2, 1}; });
         }},
        // s067: per-side insets / wrap offsets, a column's width typed.
        {"set_side_inset", [](DocJournal& j, Doc& d) { set_side(j, d, {"frame.1"}, SideOf::Inset, 3, 14); }},
        {"set_side_wrap", [](DocJournal& j, Doc& d) { set_side(j, d, {"frame.11"}, SideOf::Wrap, -1, 20); }},
        {"set_column_width", [](DocJournal& j, Doc& d) { set_column_width(j, d, {"frame.10"}, 0, 150); }},
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
        {"add_library_swatches", [](DocJournal& j, Doc& d) {   // s171
            doc::add_library_swatches(j, d, {Swatch{"Lib Red", 0, ColorModel::RGB, {200, 30, 30, 0}, "", 0xC81E1E},
                                             Swatch{"Lib Ink", 0, ColorModel::Spot, {0, 80, 90, 0}, "Lib Ink", 0xFF331A}}, "Test Library");
        }},
        {"duplicate_swatch", [](DocJournal& j, Doc& d) { duplicate_swatch(j, d, "swatch.3"); }},
        {"delete_unused_swatch", [](DocJournal& j, Doc& d) { delete_swatch(j, d, "swatch.4"); }},
        {"delete_unused_swatches", [](DocJournal& j, Doc& d) { delete_unused_swatches(j, d); }},   // s179
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
        {"wrap_shape", [](DocJournal& j, Doc& d) {   // s173: a traced outline, then the wrap
             set_wrap_shape(j, d, {"frame.4"}, {{d.as<Frame>("frame.4")->asset, {1.5f, 0.2f, 0.8f, 0.f, 1.f}}});
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
        if (d.side_of(pages[i]) == Side::Left && i + 1 < pages.size() &&   // s147: facing is per master now
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
        else if (const auto* m = d.as<Master>(id)) {
            w = m->w; h = m->h; master = true;
            if (m->facing && m->folds.empty()) {   // s150 (Scott): its spread, left then right
                out.push_back({id, {x, 0, w, h}, true, Side::Left});
                out.push_back({id, {x + w, 0, w, h}, true, Side::Right});
                x += 2 * w + gap;
                continue;
            }
        }
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
    return std::max(36.0, m.w - m.margins.inside - m.margins.outside);   // s147: the master's own width
}

double bleed_of(const Doc& d, const Doc::Id& owner) {   // s147
    if (const auto* m = d.as<Master>(owner)) return m->bleed;
    if (const auto* p = d.as<Page>(owner))
        if (const auto* m = d.as<Master>(p->master)) return m->bleed;
    return d.settings().bleed;
}

bool any_facing(const Doc& d) {   // s147
    for (const auto& id : d.ids_of<Master>()) if (d.as<Master>(id)->facing) return true;
    return d.ids_of<Master>().empty() && d.settings().facing;
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
    return (k >= 0 && k <= 3) || k == 5 || k == 6 || k == 7 ? k : -1;   // s128: 5 a reference, 6 an anchor; s134: 7 an index entry
}

const char* marker_sign(int k) {   // s128 (the signs were s023's, in two places)
    switch (k) {
    case 0: return "\xEF\xBC\x83";   // ＃
    case 1: return "\xE2\xB8\xB9";   // ⸹
    case 2: return "\xE3\x80\x8B";   // 》
    case 3: return "\xE3\x80\x8A";   // 《
    case 5: return "\xE2\x86\x92";   // → a cross-reference
    case 6: return "\xE2\x81\xA0";   // word joiner: an anchor, unseen
    case 7: return "\xE2\x81\xA0";   // s134: and an index entry
    default: return "\xEF\xBF\xBD";
    }
}

int marks_in(const std::string& t, const char* mark, std::size_t from, std::size_t to) {   // s128
    to = std::min(to, t.size());
    int n = 0;
    for (std::size_t i = t.find(mark, from); i != std::string::npos && i + 3 <= to; i = t.find(mark, i + 3)) ++n;
    return n;
}

void fit_marks(Para& p) {   // s128
    p.refs.resize(static_cast<std::size_t>(marks_in(p.text, kMarkRef)));
    p.anchors.resize(static_cast<std::size_t>(marks_in(p.text, kMarkAnchor)));
    p.index.resize(static_cast<std::size_t>(marks_in(p.text, kMarkIndex)));   // s134
}

bool note_at(const std::string& t, std::size_t i) {   // s109
    return i + 3 <= t.size() && static_cast<unsigned char>(t[i]) == 0xEE && static_cast<unsigned char>(t[i + 1]) == 0x80 &&
           static_cast<unsigned char>(t[i + 2]) == 0x84;
}

int notes_in(const std::string& t, std::size_t from, std::size_t to) {
    to = std::min(to, t.size());
    int n = 0;
    for (std::size_t i = t.find(kMarkNote, from); i != std::string::npos && i + 3 <= to; i = t.find(kMarkNote, i + 3)) ++n;
    return n;
}

int note_number(const Story& s, int p, int k) {
    int n = 0;
    for (int i = 0; i < p && i < static_cast<int>(s.paras.size()); ++i) n += static_cast<int>(s.paras[static_cast<std::size_t>(i)].notes.size());
    return n + k + 1;
}

int note_number(const Doc& d, const Story& s, int p, int k) {   // s124
    if (d.settings().note_restart != NoteRestart::Chapter) return note_number(s, p, k);
    int from = 0;   // the chapter's first paragraph: the last at or before p that starts a page
    for (int i = std::min(p, static_cast<int>(s.paras.size()) - 1); i > 0; --i) {
        const auto sa = doc::start_of(d, s.paras[static_cast<std::size_t>(i)]);
        if (sa == ParaStart::NextPage || sa == ParaStart::NextRightPage) { from = i; break; }
    }
    int n = 0;
    for (int i = from; i < p && i < static_cast<int>(s.paras.size()); ++i) n += static_cast<int>(s.paras[static_cast<std::size_t>(i)].notes.size());
    return n + k + 1;
}

void fit_notes(Para& p) {
    const auto n = static_cast<std::size_t>(notes_in(p.text));
    if (p.notes.size() != n) p.notes.resize(n);
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
    // s209: a text frame with a shape -- its lines kept inside it: what lies
    // outside the outline, band by band, is wrapped like any other frame.
    if (t && doc::has_outline(*t) && !(t->shape == ShapeKind::Path && t->open)) {
        Frame own = *t;
        own.stroke.clear();   // the words sit inside the outline itself
        const auto bands = outline_bands(own, d, 2);
        double y = 0;
        for (auto b : bands) {
            if (b.x1 - b.x0 < 24) continue;   // too narrow for words: a gap, as above and below the shape
            if (b.y0 > y + 0.01) out.push_back({-1, y, t->w + 1, b.y0, true});   // above the shape (a gap)
            if (b.x0 > 0.01) out.push_back({-1, b.y0, b.x0, b.y1, false});
            if (b.x1 < t->w - 0.01) out.push_back({b.x1, b.y0, t->w + 1, b.y1, false});
            y = std::max(y, b.y1);
        }
        if (y < t->h - 0.01) out.push_back({-1, y, t->w + 1, t->h + 1, true});
    }
    if (!t || t->ignore_wrap) return out;   // s069
    auto owners = master_chain(d, t->owner);
    owners.push_back(t->owner);
    // s069: the stacking order, bottom up -- the most basic master's frames
    // first, the text frame's own owner's last, each by z.
    const bool beneath = d.settings().wrap_beneath;
    bool above = false;   // walking up: past the text frame yet?
    for (const auto& o : owners)
        for (const auto& fid : d.frames_on(o)) {
            if (fid == id) { above = true; continue; }
            const auto* f = d.as<Frame>(fid);
            if (!f || f->wrap == Wrap::None) continue;
            if (beneath && !above) continue;   // below the text: it doesn't push
            if (o != t->owner && !master_item_on(d, fid, t->owner)) continue;   // s025: not shown on this page
            const double fx = f->x + (o != t->owner ? across_dx(d, *f, d.side_of(t->owner)) : 0.0);   // s155
            // s173: Shape -- the picture's traced outline, band by band, grown by the offsets.
            if (f->wrap == Wrap::Shape)
                if (const auto* a = d.as<Asset>(f->asset); (a && !a->shape.empty()) || doc::has_outline(*f)) {
                    const auto bands = doc::has_outline(*f) ? outline_bands(*f, d) : shape_bands(*f, *a);   // s200: a shape's own outline (s209: a shaped picture's too)
                    if (!bands.empty()) {
                        for (const auto& b : bands) {
                            WrapRect r{fx + b.x0 - f->wrap_off.left - t->x, f->y + b.y0 - f->wrap_off.top - t->y,
                                       fx + b.x1 + f->wrap_off.right - t->x, f->y + b.y1 + f->wrap_off.bottom - t->y, false};
                            if (r.x1 <= 0 || r.y1 <= 0 || r.x0 >= t->w || r.y0 >= t->h) continue;
                            out.push_back(r);
                        }
                        continue;
                    }
                }
            WrapRect r{fx - f->wrap_off.left - t->x, f->y - f->wrap_off.top - t->y,
                       fx + f->w + f->wrap_off.right - t->x, f->y + f->h + f->wrap_off.bottom - t->y,
                       f->wrap == Wrap::Jump};
            // Only what overlaps the text frame matters.
            if (r.x1 <= 0 || r.y1 <= 0 || r.x0 >= t->w || r.y0 >= t->h) continue;
            out.push_back(r);
        }
    return out;
}

std::vector<float> trace_shape(const std::uint32_t* px, int w, int h, int stride, int bands) {
    std::vector<float> out;
    if (!px || w <= 0 || h <= 0 || stride < w) return out;
    const auto at = [&](int x, int y) { return px[static_cast<std::size_t>(y) * static_cast<std::size_t>(stride) + static_cast<std::size_t>(x)]; };
    const auto ch = [](std::uint32_t c, int k) { return static_cast<int>((c >> (8 * k)) & 0xff); };
    bool clear = false;
    for (int y = 0; y < h && !clear; ++y)
        for (int x = 0; x < w; ++x)
            if (ch(at(x, y), 3) < 255) { clear = true; break; }
    // No see-through pixels: the corners' colour is the background, if they share one.
    constexpr int kTol = 24;
    const std::uint32_t corners[4] = {at(0, 0), at(w - 1, 0), at(0, h - 1), at(w - 1, h - 1)};
    const auto near = [&](std::uint32_t a, std::uint32_t b) {
        for (int k = 0; k < 3; ++k) if (std::abs(ch(a, k) - ch(b, k)) > kTol) return false;
        return true;
    };
    const std::uint32_t bg = corners[0];
    if (!clear)
        for (auto c : corners)
            if (!near(c, bg)) return out;   // a photo to its edges: nothing to trace
    const auto solid = [&](std::uint32_t c) { return clear ? ch(c, 3) >= 128 : !near(c, bg); };
    const int n = std::clamp(bands, 1, h);
    out.push_back(static_cast<float>(static_cast<double>(w) / h));
    bool any = false;
    for (int b = 0; b < n; ++b) {
        const int y0 = b * h / n, y1 = std::max(y0 + 1, (b + 1) * h / n);
        int lo = w, hi = -1;
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < lo; ++x) if (solid(at(x, y))) { lo = x; break; }
            for (int x = w - 1; x > hi; --x) if (solid(at(x, y))) { hi = x; break; }
        }
        if (hi < lo) { out.push_back(1.f); out.push_back(0.f); continue; }
        any = true;
        out.push_back(static_cast<float>(static_cast<double>(lo) / w));
        out.push_back(static_cast<float>(static_cast<double>(hi + 1) / w));
    }
    if (!any) out.clear();   // see-through all over: nothing to wrap
    return out;
}

std::vector<Band> shape_bands(const Frame& f, const Asset& a) {
    std::vector<Band> out;
    if (a.shape.size() < 3 || a.shape[0] <= 0) return out;
    const double aspect = a.shape[0];
    const Rect pr = picture_rect({0, 0, f.w, f.h}, f, aspect, 1.0);
    const std::size_t n = (a.shape.size() - 1) / 2;
    if (pr.w <= 0 || pr.h <= 0 || n == 0) return out;
    for (std::size_t i = 0; i < n; ++i) {
        const double s0 = a.shape[1 + 2 * i], s1 = a.shape[2 + 2 * i];
        if (s1 <= s0) continue;
        Band b{pr.x + s0 * pr.w, pr.y + pr.h * static_cast<double>(i) / static_cast<double>(n),
               pr.x + s1 * pr.w, pr.y + pr.h * static_cast<double>(i + 1) / static_cast<double>(n)};
        // The frame crops the picture.
        b.x0 = std::max(b.x0, 0.0); b.y0 = std::max(b.y0, 0.0);
        b.x1 = std::min(b.x1, f.w); b.y1 = std::min(b.y1, f.h);
        if (b.x1 > b.x0 && b.y1 > b.y0) out.push_back(b);
    }
    return out;
}

std::vector<Band> outline_bands(const Frame& f, const Doc& d, double step) {   // s200
    std::vector<Band> out;
    if (!doc::has_outline(f) || step <= 0) return out;
    const double grow = d.as<Swatch>(f.stroke) && f.stroke_w > 0 ? f.stroke_w / 2 : 0;
    const auto polys = doc::outline_polys(f, 0.25);
    for (double y0 = f.y - grow; y0 < f.y + f.h + grow; y0 += step) {
        const double y1 = std::min(y0 + step, f.y + f.h + grow);
        double lo = 1e300, hi = -1e300;
        for (const auto& poly : polys) {
            const std::size_t n = poly.size();
            const bool closed = !(f.shape == ShapeKind::Path && f.open);
            const std::size_t segs = closed ? n : n - 1;
            for (std::size_t i = 0; i < segs && n >= 2; ++i) {
                const auto a = poly[i], b = poly[(i + 1) % n];
                // The part of this edge inside the band (grown by the stroke).
                const double ya = std::min(a.y, b.y) - grow, yb = std::max(a.y, b.y) + grow;
                if (yb < y0 || ya > y1) continue;
                const auto x_at = [&](double y) {
                    if (std::abs(b.y - a.y) < 1e-12) return std::array<double, 2>{std::min(a.x, b.x), std::max(a.x, b.x)};
                    const double t = std::clamp((y - a.y) / (b.y - a.y), 0.0, 1.0);
                    const double x = a.x + (b.x - a.x) * t;
                    return std::array<double, 2>{x, x};
                };
                for (const double y : {std::max(y0, std::min(a.y, b.y)), std::min(y1, std::max(a.y, b.y))}) {
                    const auto xs = x_at(y);
                    lo = std::min(lo, xs[0] - grow);
                    hi = std::max(hi, xs[1] + grow);
                }
            }
        }
        if (hi < lo) continue;
        out.push_back({lo - f.x, y0 - f.y, hi - f.x, y1 - f.y});
    }
    return out;
}

bool on_panel(const Doc& d, const Placed& p, const Doc::Id& fid) {
    const auto* f = d.as<Frame>(fid);
    if (!f || f->owner != p.id) return false;
    return p.side == Side::Any || f->side == Side::Any || f->side == p.side;   // s155: across: Any, both
}

double across_dx(const Doc& d, const Frame& f, Side side) {   // s155
    if (!f.across || side != Side::Right) return 0;
    const auto* m = d.as<Master>(f.owner);
    return m ? -m->w : 0;
}

Side side_for(const Doc& d, const Doc::Id& id, Side panel) {   // s155
    if (d.as<Page>(id)) return d.side_of(id);
    return panel;
}

const Placed* panel_at(const std::vector<Placed>& pl, double x, double y) {
    for (const auto& p : pl)
        if (p.r.contains(x, y)) return &p;
    return nullptr;
}

// s150: the first panel it shows on (the canvas puts the panel last used
// first, so an item on both sides is handled where it was picked).
Rect frame_in_layout(const Doc& d, const std::vector<Placed>& pl, const Doc::Id& id) {
    const auto* f = d.as<Frame>(id);
    if (!f) return {};
    for (const auto& p : pl)
        if (on_panel(d, p, id)) return {p.r.x + f->x + across_dx(d, *f, p.side), p.r.y + f->y, f->w, f->h};
    return {};
}

Doc::Id hit_frame(const Doc& d, const std::vector<Placed>& pl, double x, double y, double tol) {
    // Every placed page's frames, even off the page: a frame may hang onto
    // the pasteboard.
    for (const auto& p : pl) {
        const auto own = d.frames_on(p.id);
        for (auto it = own.rbegin(); it != own.rend(); ++it) {   // topmost first
            if (!on_panel(d, p, *it)) continue;
            const auto& fr = *d.as<Frame>(*it);
            if (doc::has_outline(fr)) {   // s203: a line or a path by its outline, not its box
                if (doc::shape_hit(fr, x - p.r.x, y - p.r.y, tol)) return *it;
                continue;
            }
            const Rect r{p.r.x + fr.x, p.r.y + fr.y, fr.w, fr.h};
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
                const Side sd = d.side_of(p.id);
                const Rect r{p.r.x + f.x + across_dx(d, f, sd), p.r.y + f.y, f.w, f.h};
                // s155: an item across the spine is this page's only on its side of it
                if (f.across && (sd == Side::Left ? x > p.r.r() : sd == Side::Right ? x < p.r.x : false)) continue;
                if (doc::has_outline(f) ? doc::shape_hit(f, x - p.r.x - across_dx(d, f, sd), y - p.r.y, tol)   // s203
                                        : Rect{r.x - tol, r.y - tol, r.w + 2 * tol, r.h + 2 * tol}.contains(x, y)) {
                    if (page) *page = p.id;
                    return *it;
                }
            }
        }
    }
    return {};
}

std::vector<MasterItemOn> master_items_in(const Doc& d, const std::vector<Placed>& pl, const Rect& r) {   // s169
    std::vector<MasterItemOn> out;
    for (const auto& p : pl) {
        if (!d.as<Page>(p.id)) continue;
        const Side sd = d.side_of(p.id);
        for (const auto& m : master_chain(d, p.id))
            for (const auto& fid : d.frames_on(m)) {
                if (!master_item_on(d, fid, p.id)) continue;
                const auto& f = *d.as<Frame>(fid);
                Rect fr{p.r.x + f.x + across_dx(d, f, sd), p.r.y + f.y, f.w, f.h};
                if (f.across && sd == Side::Left) fr.w = std::max(0.0, std::min(fr.r(), p.r.r()) - fr.x);
                if (f.across && sd == Side::Right) { const double rr = fr.r(); fr.x = std::max(fr.x, p.r.x); fr.w = std::max(0.0, rr - fr.x); }
                const bool touch = fr.x <= r.r() && fr.r() >= r.x && fr.y <= r.b() && fr.b() >= r.y;
                if (touch && fr.w > 0) out.push_back({p.id, fid});
            }
    }
    return out;
}

std::vector<Doc::Id> frames_in(const Doc& d, const std::vector<Placed>& pl, const Rect& r) {
    std::vector<Doc::Id> out;
    for (const auto& p : pl)
        for (const auto& f : d.frames_on(p.id)) {
            if (!on_panel(d, p, f) || std::find(out.begin(), out.end(), f) != out.end()) continue;
            const auto& fr = *d.as<Frame>(f);
            if (Rect{p.r.x + fr.x, p.r.y + fr.y, fr.w, fr.h}.intersects(r)) out.push_back(f);   // s150: per panel
        }
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

const Margins* margins_of(const Doc& d, const Doc::Id& id) {
    if (const auto* p = d.as<Page>(id); p && p->margins) return &*p->margins;   // s062
    const auto* m = master_of(d, id);
    return m ? &m->margins : nullptr;
}

Rect margin_rect(const Doc& d, const Doc::Id& id) { return margin_rect(d, id, Side::Any); }

Rect margin_rect(const Doc& d, const Doc::Id& id, Side side) {
    const auto* m = master_of(d, id);
    const auto* pg = d.as<Page>(id);
    if (!m && !(pg && pg->margins)) return {};
    double w = m ? m->w : pg->w, h = m ? m->h : pg->h;
    if (pg) { w = pg->w; h = pg->h; }
    const auto& mg = *margins_of(d, id);
    // A left-hand page: the spine is on its right.
    const bool left = d.as<Page>(id) ? d.side_of(id) == Side::Left : side == Side::Left;   // s150: a master's left panel
    const double l = left ? mg.outside : mg.inside, r = left ? mg.inside : mg.outside;
    if (w - l - r < 1 || h - mg.top - mg.bottom < 1) return {};
    return {l, mg.top, w - l - r, h - mg.top - mg.bottom};
}

BaselineGrid baseline_grid(const Doc& d, const Doc::Id& id) {
    const auto* m = master_of(d, id);
    if (!m || m->grid < 1) return {};
    return {margins_of(d, id)->top, m->grid};   // s062: from the page's own top margin
}

std::pair<int, double> page_column_setup(const Doc& d, const Doc::Id& id) {
    const auto* m = master_of(d, id);
    if (!m) return {1, 12};
    return {std::clamp(m->columns, 1, 24), std::max(0.0, m->gutter)};
}

std::vector<ColumnSpan> page_columns(const Doc& d, const Doc::Id& id) { return page_columns(d, id, Side::Any); }

std::vector<ColumnSpan> page_columns(const Doc& d, const Doc::Id& id, Side side) {
    const Rect r = margin_rect(d, id, side);
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
        if (const Rect mr = margin_rect(d, p.id, p.side); mr.w > 0) {
            t.ys.insert(t.ys.end(), {p.r.y + mr.y, p.r.y + mr.b()});
            for (const auto& c : page_columns(d, p.id, p.side)) t.xs.insert(t.xs.end(), {p.r.x + c.x0, p.r.x + c.x1});
        }
        auto owners = p.master ? std::vector<Doc::Id>{} : master_chain(d, p.id);
        owners.push_back(p.id);
        for (const auto& o : owners) {
            for (const auto& g : guides_on(d, o)) {
                if (skip(g)) continue;   // s153: a guide being dragged isn't its own target
                const auto& gd = *d.as<Guide>(g);
                (gd.vertical ? t.xs : t.ys).push_back((gd.vertical ? p.r.x : p.r.y) + gd.pos);
            }
            for (const auto& f : d.frames_on(o)) {
                if (skip(f)) continue;
                if (o == p.id ? !on_panel(d, p, f) : !master_item_on(d, f, p.id)) continue;   // s150: what shows here
                const auto& fr = *d.as<Frame>(f);
                const Rect r{p.r.x + fr.x + across_dx(d, fr, side_for(d, p.id, p.side)), p.r.y + fr.y, fr.w, fr.h};   // s155
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

// ── the rulers' zero (s154) ─────────────────────────────────────────────────
namespace {
// The origin alone (no dragged zero point), from the owner's top-left.
Zero origin_of(const Doc& d, const Doc::Id& owner, Side side) {
    const auto o = d.settings().ruler_origin;
    if (o == RulerOrigin::Page) return {};
    if (const auto* m = d.as<Master>(owner)) {
        const bool spread = m->facing && m->folds.empty();
        const bool right = spread && side == Side::Right;
        if (o == RulerOrigin::Spread) return {right ? -m->w : 0.0, 0};
        return {spread && !right ? m->w : 0.0, 0};   // Spine: a both-sides item reads as on the left
    }
    const auto* pg = d.as<Page>(owner);
    if (!pg) return {};
    if (o == RulerOrigin::Spine) return {d.side_of(owner) == Side::Left ? pg->w : 0.0, 0};
    double before = 0;   // Spread: the pages before it in its spread
    for (const auto& id : spread_of(d, owner)) {
        if (id == owner) break;
        if (const auto* q = d.as<Page>(id)) before += q->w;
    }
    return {-before, 0};
}
}  // namespace

Zero ruler_zero(const Doc& d, const Doc::Id& owner, Side side) {
    Zero z = origin_of(d, owner, side);
    if (d.settings().ruler_origin != RulerOrigin::Spine) { z.x += d.settings().zero_x; z.y += d.settings().zero_y; }
    return z;
}

std::vector<RulerSpan> ruler_spans(const Doc& d, const std::vector<Placed>& pl) {
    std::vector<RulerSpan> out;
    const double inf = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < pl.size(); ++i) {
        const auto& p = pl[i];
        const double z = p.r.x + ruler_zero(d, p.id, p.side).x;
        if (!out.empty() && std::abs(pl[i - 1].r.r() - p.r.x) < 1e-6 && std::abs(out.back().zero - z) < 1e-6) {
            out.back().hi = p.r.r();
            continue;
        }
        if (!out.empty()) {
            const double cut = (pl[i - 1].r.r() + p.r.x) / 2;
            out.back().hi = cut;
            out.push_back({cut, p.r.r(), z});
        } else {
            out.push_back({-inf, p.r.r(), z});
        }
    }
    if (!out.empty()) out.back().hi = inf;
    else out.push_back({-inf, inf, d.settings().zero_x});
    return out;
}

Zero zero_at(const Doc& d, const std::vector<Placed>& pl, double x, double y) {
    const Placed* p = panel_at(pl, x, y);
    if (!p) {   // over the pasteboard: the page nearest across
        double best = std::numeric_limits<double>::infinity();
        for (const auto& q : pl) {
            const double gap = x < q.r.x ? q.r.x - x : (x > q.r.r() ? x - q.r.r() : 0);
            if (gap < best) { best = gap; p = &q; }
        }
    }
    if (!p) return {x, y};
    const Zero o = origin_of(d, p->id, p->side);
    return {x - p->r.x - o.x, y - p->r.y - o.y};
}

}  // namespace composix::core

namespace composix::core {
std::string ref_pattern(const XRef& r) {   // s129
    switch (r.show) {
    case RefShow::Page:     return "#";
    case RefShow::Text:     return "%";
    case RefShow::TextPage: return "\u201c%\u201d on page #";
    case RefShow::Note:     return "*";
    case RefShow::Custom:   return r.pattern.empty() ? std::string("#") : r.pattern;
    case RefShow::Cite: {   // s141; s143: a locator -- held to its number (no-break spaces: "[2, p. 14]" never splits)
        if (r.pattern.empty()) return "[^]";
        std::string loc;
        for (const char c : r.pattern) loc += c == ' ' ? std::string("\u00a0") : std::string(1, c);
        return "[^,\u00a0" + loc + "]";
    }
    }
    return "#";
}

int source_number(const Doc& d, const Story& s, int p) {   // s141
    std::vector<std::string> styles;
    for (const auto& lid : d.ids_of<GenList>())
        if (const auto& l = *d.as<GenList>(lid); l.kind == ListKind::Bibliography)
            styles.insert(styles.end(), l.from_styles.begin(), l.from_styles.end());
    const auto is_source = [&](const Para& x) { return std::find(styles.begin(), styles.end(), x.style) != styles.end(); };
    if (p < 0 || p >= static_cast<int>(s.paras.size()) || !is_source(s.paras[static_cast<std::size_t>(p)])) return 0;
    int n = 0;
    for (int q = 0; q <= p; ++q) n += is_source(s.paras[static_cast<std::size_t>(q)]);
    return n;
}

}  // namespace composix::core
