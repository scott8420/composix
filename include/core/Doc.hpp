#pragma once
// core::Doc -- THE document (s004). GTK-free; the selftest drives it headless.
//
// One store, every record by id. The journal (core/Undo.hpp) asks only
// get(id) / put(id, value), so a record is a std::variant of the kinds below
// and the whole document is one map. Every user choice is a verb in
// core::doc:: that goes through Journal::run -- the ONE DOOR -- and is listed
// in doc::verbs(), which the selftest walks (the gate: a write with the door
// shut is a stray and fails the build's tests).
//
// The shape, after REQUIREMENTS:
//   Settings    one record ("doc"): title, trim, bleed, facing pages
//   Section     starts AT a page (InDesign's model): the run of pages from its
//               start page to the next section's. Numbering, start-at, side.
//   Page        place (pos), its OWN size -- a foldout is a wide page with
//               fold positions -- and its master
//   Master      a page template: prefix + name, size, columns, based-on
//   Frame       on a page or master: geometry in points, text / image / shape;
//               a text frame names its story and the NEXT frame in the thread
//   Story       text apart from frames -- paragraphs, each tagged with a style
//               (semantic: what EPUB reflow and TOC generation need)
//   Style       paragraph / character
//   Swatch      process CMYK / RGB / gray / spot
//   Asset       an image: linked (a path) or embedded (in the project folder)
//   GenList     a generated list: TOC / bibliography / index, from styles
//
// Units: POINTS (1/72 in) everywhere in the model. Inches are a display unit.
// A record's PLACE (pos, owner) is in its value, so undoing a delete puts it
// back exactly where it was, under the same id.
#include "core/Undo.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace composix::core {

inline constexpr double kPt = 72.0;   // points per inch

enum class Numbering { Arabic, RomanLower, RomanUpper, AlphaLower, AlphaUpper };
enum class Side      { Any, Right, Left };
enum class FrameKind { Text, Image, Shape };
enum class StyleKind { Paragraph, Character };
enum class Align     { Left, Center, Right, Justify };
enum class ColorModel{ CMYK, RGB, Gray, Spot };
enum class ListKind  { Contents, Bibliography, Index };

struct Settings {
    std::string title  = "Untitled";
    double      trim_w = 6 * kPt, trim_h = 9 * kPt;
    double      bleed  = 0.125 * kPt;
    bool        facing = true;
    bool operator==(const Settings&) const = default;
};

struct Section {
    std::string name;
    std::string start_page;              // the page this section starts at
    Numbering   numbering = Numbering::Arabic;
    int         start_at  = 1;
    Side        starts    = Side::Any;   // where its first page wants to fall
    std::string master;                  // default master for new pages in it
    bool operator==(const Section&) const = default;
};

struct Page {
    double              pos = 0;         // order (ascending)
    double              w = 6 * kPt, h = 9 * kPt;
    std::vector<double> folds;           // x of each fold, from the left; empty = not a foldout
    std::string         master;          // "" = none
    bool operator==(const Page&) const = default;
};

struct Master {
    std::string         prefix;          // "B"
    std::string         name;            // "Body"
    double              pos = 0;
    double              w = 6 * kPt, h = 9 * kPt;
    std::vector<double> folds;
    int                 columns = 1;
    std::string         based_on;        // another master's id, or ""
    bool operator==(const Master&) const = default;
};

struct Frame {
    std::string owner;                   // page or master id
    FrameKind   kind = FrameKind::Text;
    double      x = 0, y = 0, w = 0, h = 0, rot = 0;
    double      z = 0;
    std::string story;                   // text
    std::string next;                    // text: the next frame in the thread
    std::string asset;                   // image
    bool operator==(const Frame&) const = default;
};

struct Para {
    std::string style;                   // a paragraph style id
    std::string text;
    bool operator==(const Para&) const = default;
};

struct Story {
    std::string       name;
    std::vector<Para> paras;
    bool operator==(const Story&) const = default;
};

struct Style {
    std::string name;
    StyleKind   kind   = StyleKind::Paragraph;
    double      pos    = 0;
    std::string family = "Serif";
    double      size   = 11, leading = 14;   // points; 0 = inherit (character styles)
    bool        bold   = false, italic = false;
    Align       align  = Align::Left;
    bool operator==(const Style&) const = default;
};

struct Swatch {
    std::string           name;
    double                pos   = 0;
    ColorModel            model = ColorModel::CMYK;
    std::array<double, 4> v{};          // CMYK 0-100 / RGB 0-255 / gray 0-100
    std::string           spot;          // the ink's name, for Spot
    std::uint32_t         preview = 0x808080;   // what the screen shows (spot / CMYK approximation)
    bool operator==(const Swatch&) const = default;
};

struct Asset {
    std::string name;                    // display name, "guild-map.tif"
    double      pos = 0;
    std::string path;                    // linked: absolute or project-relative; embedded: assets/<file>
    bool        embedded = false;
    std::string color = "RGB";           // "CMYK" / "RGB" / "Gray"
    int         px_w = 0, px_h = 0;      // pixels; 0 = vector
    bool operator==(const Asset&) const = default;
};

struct GenList {
    std::string              name;
    ListKind                 kind = ListKind::Contents;
    double                   pos  = 0;
    std::vector<std::string> from_styles;   // paragraph style ids gathered
    std::string              placed_on;     // page id, or ""
    int                      built_entries = -1;   // entries when last built; -1 = never
    bool operator==(const GenList&) const = default;
};

using Record = std::variant<Settings, Section, Page, Master, Frame, Story, Style, Swatch, Asset, GenList>;

class Doc {
public:
    using Id    = std::string;
    using Value = Record;

    // ── the journal's contract ──────────────────────────────────────────────
    std::optional<Record> get(const Id& id) const;
    void put(const Id& id, const std::optional<Record>& v);

    // ── typed reading ───────────────────────────────────────────────────────
    template <class T> const T* as(const Id& id) const {
        const auto it = m_recs.find(id);
        return it == m_recs.end() ? nullptr : std::get_if<T>(&it->second);
    }
    // Every id of one kind, in its natural order (pos where it has one; for
    // sections, the order of their start pages).
    template <class T> std::vector<Id> ids_of() const;

    const Settings& settings() const;
    std::vector<Id> pages() const { return ids_of<Page>(); }
    std::size_t     size() const { return m_recs.size(); }
    const std::map<Id, Record>& records() const { return m_recs; }

    // Pages: where they fall.
    int         page_index(const Id& page) const;       // 0-based in page order; -1 absent
    Id          section_of(const Id& page) const;       // "" = before any section (implicit)
    std::string folio(const Id& page) const;            // "iv", "7"
    Side        side_of(const Id& page) const;          // Right / Left (facing), Any (single)
    std::vector<Id> pages_of(const Id& section) const;
    std::vector<Id> frames_on(const Id& owner) const;   // by z

    // A fresh id with a prefix: "page" -> "page.13". Never reused in a session.
    Id fresh_id(const std::string& prefix);
    // A place just after `after` among T (append when "" / absent / empty).
    template <class T> double pos_after(const Id& after) const;

    // ── the one-door guard (as Cairn's Stones) ──────────────────────────────
    void set_door(std::function<bool()> open) { m_door = std::move(open); }
    int  strays() const { return m_strays; }
    struct Loading {   // a load is not a user choice and is not undoable
        explicit Loading(Doc& d) : m_d(d) { ++m_d.m_loading; }
        ~Loading() { --m_d.m_loading; }
        Doc& m_d;
    };
    void set_on_change(std::function<void()> f) { m_changed = std::move(f); }

    // Start over (a new / opened document). Not a user step.
    void reset();

private:
    std::map<Id, Record>       m_recs;
    std::map<std::string, int> m_next;   // fresh_id counters per prefix
    int                        m_strays = 0;
    int                        m_loading = 0;
    std::function<bool()>      m_door;
    std::function<void()>      m_changed;
};

using DocJournal = Journal<Doc>;

// Display helpers.
std::string format_folio(int n, Numbering how);
std::string inches(double pt);                          // 432 -> "6", 423 -> "5.875"
std::string size_text(double w, double h, std::size_t folds);   // "17.75 × 9 in (3-panel)"
std::string numbering_text(Numbering n);
std::string side_text(Side s);
std::string master_label(const Doc& d, const Doc::Id& master);  // "B-Body", "None"

// ── the verbs: every user choice on the document ───────────────────────────
// Each goes through the journal; the UI calls these and nothing else writes.
namespace doc {

// Size presets the inspector offers for a page: the trim, a 2-panel and a
// 3-panel foldout (each fold-in panel 1/8 in narrower, so it tucks inside).
struct SizePreset { std::string label; double w, h; std::vector<double> folds; };
std::vector<SizePreset> size_presets(const Settings& s);

std::vector<Doc::Id> add_pages(DocJournal& j, Doc& d, const Doc::Id& after, int count,
                               const Doc::Id& master);
bool remove_pages(DocJournal& j, Doc& d, const std::vector<Doc::Id>& pages);
bool move_pages(DocJournal& j, Doc& d, const std::vector<Doc::Id>& pages, const Doc::Id& after);
bool set_master(DocJournal& j, Doc& d, const std::vector<Doc::Id>& pages, const Doc::Id& master);
bool set_page_size(DocJournal& j, Doc& d, const std::vector<Doc::Id>& pages, const SizePreset& sz);

Doc::Id add_section(DocJournal& j, Doc& d, const Doc::Id& start_page, const std::string& name,
                    Numbering n, int start_at);
bool set_numbering(DocJournal& j, Doc& d, const std::vector<Doc::Id>& sections, Numbering n);
bool set_starts(DocJournal& j, Doc& d, const std::vector<Doc::Id>& sections, Side s);
bool remove_section(DocJournal& j, Doc& d, const Doc::Id& section);

// The neighbour that takes the selection when `pages` go (next, else previous).
Doc::Id neighbour_page(const Doc& d, const std::vector<Doc::Id>& pages);

// The verb table the selftest walks: every verb, runnable on the sample.
struct Verb {
    std::string name;
    std::function<void(DocJournal&, Doc&)> run;
};
const std::vector<Verb>& verbs();

}  // namespace doc

// The built-in sample (a small illustrated book) -- what the app opens to
// before a project is opened, and the selftest's fixture. Loaded, not undoable.
void load_sample(Doc& d);

// ── the project folder: <name>/document.json + assets/ ─────────────────────
bool        save_project(const Doc& d, const std::string& dir, std::string* err = nullptr);
bool        load_project(Doc& d, const std::string& dir, std::string* err = nullptr);
std::string to_json(const Doc& d);                       // the document.json text
bool        from_json(Doc& d, const std::string& text, std::string* err = nullptr);

}  // namespace composix::core
