#pragma once
// core::Outline -- what the NAVIGATOR and the TILE VIEW read (s002).
//
// The navigator is segmented by key areas of the document (Pages, Masters,
// Stories, ...). Each section discloses to its individual items; selecting a
// SECTION shows every item in it as tiles with metadata, selecting ITEMS opens
// them. Multi-selection everywhere: the inspector shows the fields the
// selection SHARES, "Mixed" where they differ.
//
// This is a PROJECTION, not the document: project(doc) builds it (s004), and
// the views read only this. s002 drove it from a fixture through the same
// seam; the model replaced the fixture and the GTK side did not change.
//
// GTK-free: lives in composix_core, guarded headless by the selftest.
#include <cstdint>
#include <functional>
#include <optional>
#include <map>
#include <string>
#include <vector>

namespace composix::core {

enum class Segment { Document, Pages, Masters, Stories, Styles, Assets, Colors, Generated, Notes, Layers, Links };   // s339: Links (Hyperlinks)   // s115: Notes (an index into names); shown after Stories; s308: Layers

struct SegmentInfo {
    Segment     id;
    std::string key;      // stable, for widget names: "pages" -> nav.segment.pages
    std::string title;    // "Pages & Spreads"
    std::string icon;     // symbolic icon name for the navigator
};

// Navigator order. Every Segment appears exactly once (selftest).
const std::vector<SegmentInfo>& segments();
const SegmentInfo& segment_info(Segment s);

// s115: a footnote's item in the outline -- "note:<story>:<para>:<k>" (where
// it is now; the ids move with the text). parse: false when not one.
std::string note_item_id(const std::string& story, int para, int k);
bool parse_note_item_id(const std::string& id, std::string& story, int& para, int& k);

// How a tile draws its thumbnail. The painter lives on the GTK side; the core
// says only WHAT it is.
enum class ThumbKind { Page, Text, Image, Swatch, Style, Section, Generated };

struct Thumb {
    ThumbKind     kind   = ThumbKind::Page;
    double        w_in   = 6.0;      // page/image proportions (inches)
    double        h_in   = 9.0;
    std::uint32_t rgb    = 0x808080; // swatch / image tint
    bool          flag   = false;    // foldout / missing link / stale
    std::string   sample;            // first words of a story, style specimen
    int           panels = 1;        // pages: foldout panels
    bool          bold   = false;    // style specimens
    bool          italic = false;
    std::string   file;              // images (s009): the file to draw, "" = a stand-in
    std::int64_t  stamp  = 0;        // its time, so a changed file redraws
    std::string   family;            // style specimens (s011): its font
    std::string   page;              // s020: pages / masters -- the record to draw ("" = stand-in)
};

// A field the inspector may EDIT carries a key (what the UI asks the shell to
// change: "master", "size", "numbering", "starts") and the choices, as
// {value-id, label}. The shown value is the chosen label. Read-only: no key.
struct Choice {
    std::string id;      // what the verb receives: "master.2", "1" (preset index)
    std::string label;   // what the dropdown shows: "B-Body"
    bool operator==(const Choice&) const = default;
};

struct Field {
    std::string         label;
    std::string         value;
    std::string         edit;       // "" = read-only
    std::vector<Choice> choices;
    bool                buttons = false;   // s009: the choices are ACTIONS, one button each
    // s009 (Scott): a COMPACT row -- column headers over their values ("X Y W
    // H" over four numbers). `value` holds the values tab-separated; a
    // multi-selection says Mixed per column, not for the whole row.
    std::vector<std::string> cols;
    // s041: a value that can be TYPED (the choices stay as presets): a
    // length, read by parse_length with a bare number in `unit` points (1 =
    // pt, 72 = in); 0 = not typed. What it sends is points, in [lo, hi].
    // A compact row typed: each column edits on its own ("k:points").
    double unit = 0, lo = 0, hi = 0;
    // s060: a value typed as TEXT (a name; a whole number): an entry that
    // sends what's typed, as typed, on Enter or leaving it.
    bool text = false;
    // s067: a typed compact row of four sides (T R B L) with a "same all
    // round" link: linked (shown when all four agree), a value typed in
    // any column goes to all four ("a:points"); unlinked, to its own
    // ("k:points"). Linking unequal sides makes all four the first.
    bool link = false;
    // s071: a percentage typed ("40", "40%") -- sends the number, in [lo, hi].
    bool percent = false;
    // s092: a style's row that is its OWN (not from its base): the style
    // field's key ("size"), so the inspector can mark it and offer a reset.
    std::string own;
    // s161 (Scott): a heading the inspector folds this row under ("Text",
    // "Paragraph" ...; "" none). A folded group shows a line of its values.
    std::string group;
    bool operator==(const Field&) const = default;
};

struct Item {
    std::string        id;        // unique across the outline
    Segment            segment;
    std::string        title;     // "Page 7"
    std::string        badge;     // short corner tag: "Foldout", "Missing", "Spot"
    Thumb              thumb;
    std::vector<Field> fields;    // tile + inspector metadata, display order
    std::string        group;     // s052: a heading the tile view sorts under ("" = none)
};

class Outline {
public:
    void add(Item it) { m_items.push_back(std::move(it)); }

    std::vector<const Item*> in(Segment s) const;
    const Item*              find(const std::string& id) const;
    std::size_t              size() const { return m_items.size(); }
    const std::vector<Item>& items() const { return m_items; }

private:
    std::vector<Item> m_items;
};

// s068: one level up from opened items -- the section they belong to (the
// first one found decides); none when nothing given is in the outline.
std::optional<Segment> parent_of(const Outline& o, const std::vector<std::string>& ids);

// s277: the pages laid out as the navigator's card shows them -- each spread
// a row of slots, "" an empty side: a facing book's lone right-hand page gets
// an empty left beside it (and a lone left page an empty right), so every
// spine lines up. `groups` are core::spreads (empty: not facing -- one page a
// row, no padding); `is_left` says a page's side. Pages in no group stand
// alone, in order.
std::vector<std::vector<std::string>> spread_slots(const std::vector<std::string>& pages,
                                                   const std::vector<std::vector<std::string>>& groups,
                                                   const std::function<bool(const std::string&)>& is_left);

// s280: the inspector's rows with their groups in `order` (Scott: groups
// dragged into an order of his own). Rows before the first group stay first;
// rows of no group found among or after the groups follow them; groups the
// order doesn't name keep their own order, after the named.
std::vector<Field> order_groups(const std::vector<Field>& fields, const std::vector<std::string>& order);

class Doc;
// The document, as the navigator / tiles / inspector see it. `project_dir`
// resolves linked assets' relative paths ("" = unsaved: relative links can't
// be found). Item ids ARE record ids, so a pick names records directly.
// `overset` (s007): the stories whose text runs past their last frame --
// measured by the layout library, which core cannot see; null = unknown.
// `fonts` (s011): the installed families, which the style fields offer (the
// layout library lists them; null = the Font field is read-only).
// `stale` (s089): Contents lists out of date -> why (the layout's check);
// null = judged by the count of entries, as before.
Outline project(const Doc& d, const std::string& project_dir,
                const std::vector<std::string>* overset = nullptr,
                const std::vector<std::string>* fonts = nullptr,
                const std::map<std::string, std::string>* stale = nullptr);

// s005: frames picked on the canvas, as items the inspector reads (fields:
// Kind, On, X, Y, W, H, and Story + Thread or Asset). Not in the navigator.
std::vector<Choice> tint_choices(int least = 5);   // s021: "100%" .. "5%" (least 0: also "0%")
std::vector<Choice> turn_ref_choices();   // s231: Top left .. Bottom right, ids "0".."8"
std::vector<Item> frame_items(const Doc& d, const std::vector<std::string>& frames,
                              const std::string& project_dir = {});
// s011: the text being edited -- Paragraph style / Character style (editable:
// "pstyle" / "cstyle"), Selection. Mixed where the selection differs.
struct TextPos;
std::vector<Field> text_fields(const Doc& d, const std::string& story, TextPos a, TextPos b);
// s336: typing in a table's cell -- which cell, its words' character style,
// the selection, and the words' own formatting (the rows text_fields ends
// with). No paragraph rows: a cell's paragraphs take the table's style.
std::vector<Field> cell_text_fields(const Doc& d, const std::string& story, int para, int row, int col, TextPos a, TextPos b);
// "Text frame", "3 frames".
std::string frames_title(const std::vector<Item>& items);

// What the inspector shows for a multi-selection: the fields every item has,
// in the first item's order; the value where all agree, kMixed where not.
// A field stays editable only if every item offers the same edit and choices.
// Empty selection -> empty.
inline constexpr const char* kMixed = "Mixed";
std::vector<Field> shared_fields(const std::vector<const Item*>& sel);

// The inspector's heading for a selection: "Page 7", "3 pages", "5 items".
std::string selection_title(const std::vector<const Item*>& sel);

}  // namespace composix::core
