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
#include <optional>
#include <string>
#include <vector>

namespace composix::core {

enum class Segment { Document, Pages, Masters, Stories, Styles, Assets, Colors, Generated };

struct SegmentInfo {
    Segment     id;
    std::string key;      // stable, for widget names: "pages" -> nav.segment.pages
    std::string title;    // "Pages & Spreads"
    std::string icon;     // symbolic icon name for the navigator
};

// Navigator order. Every Segment appears exactly once (selftest).
const std::vector<SegmentInfo>& segments();
const SegmentInfo& segment_info(Segment s);

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
    bool operator==(const Field&) const = default;
};

struct Item {
    std::string        id;        // unique across the outline
    Segment            segment;
    std::string        title;     // "Page 7"
    std::string        badge;     // short corner tag: "Foldout", "Missing", "Spot"
    Thumb              thumb;
    std::vector<Field> fields;    // tile + inspector metadata, display order
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

class Doc;
// The document, as the navigator / tiles / inspector see it. `project_dir`
// resolves linked assets' relative paths ("" = unsaved: relative links can't
// be found). Item ids ARE record ids, so a pick names records directly.
// `overset` (s007): the stories whose text runs past their last frame --
// measured by the layout library, which core cannot see; null = unknown.
Outline project(const Doc& d, const std::string& project_dir,
                const std::vector<std::string>* overset = nullptr);

// s005: frames picked on the canvas, as items the inspector reads (fields:
// Kind, On, X, Y, W, H, and Story + Thread or Asset). Not in the navigator.
std::vector<Item> frame_items(const Doc& d, const std::vector<std::string>& frames,
                              const std::string& project_dir = {});
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
