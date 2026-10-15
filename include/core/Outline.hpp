#pragma once
// core::Outline -- what the NAVIGATOR and the TILE VIEW read (s002).
//
// The navigator is segmented by key areas of the document (Pages, Masters,
// Stories, ...). Each section discloses to its individual items; selecting a
// SECTION shows every item in it as tiles with metadata, selecting ITEMS opens
// them. Multi-selection everywhere: the inspector shows the fields the
// selection SHARES, "Mixed" where they differ.
//
// This is a PROJECTION, not the document. s002 fills it from a fixture
// (fixture_outline) so the shell is driven through the real seam from day one
// (CANON: fixture data through the real seam); when the page model lands it
// produces the same Outline, and nothing on the GTK side changes.
//
// GTK-free: lives in composix_core, guarded headless by the selftest.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace composix::core {

enum class Section { Document, Pages, Masters, Stories, Styles, Assets, Colors, Generated };

struct SectionInfo {
    Section     id;
    std::string key;      // stable, for widget names: "pages" -> nav.section.pages
    std::string title;    // "Pages & Spreads"
    std::string icon;     // symbolic icon name for the navigator
};

// Navigator order. Every Section appears exactly once (selftest).
const std::vector<SectionInfo>& sections();
const SectionInfo& section_info(Section s);

// How a tile draws its thumbnail. The painter lives on the GTK side; the core
// says only WHAT it is.
enum class ThumbKind { Page, Text, Image, Swatch, Style, Section, Generated };

struct Thumb {
    ThumbKind     kind   = ThumbKind::Page;
    double        w_in   = 6.0;      // page/image proportions (inches)
    double        h_in   = 9.0;
    std::uint32_t rgb    = 0x808080; // swatch / image tint
    bool          flag   = false;    // foldout / missing link / stale / overset
    std::string   sample;            // first words of a story, style specimen
};

struct Field {
    std::string label;
    std::string value;
};

struct Item {
    std::string        id;        // unique across the outline
    Section            section;
    std::string        title;     // "Page 7"
    std::string        badge;     // short corner tag: "Foldout", "Missing", "Spot"
    Thumb              thumb;
    std::vector<Field> fields;    // tile + inspector metadata, display order
};

class Outline {
public:
    void add(Item it) { m_items.push_back(std::move(it)); }

    std::vector<const Item*> in(Section s) const;
    const Item*              find(const std::string& id) const;
    std::size_t              size() const { return m_items.size(); }
    const std::vector<Item>& items() const { return m_items; }

private:
    std::vector<Item> m_items;
};

// The s002 fixture: a small illustrated book -- front matter, a chapter, a
// foldout map, linked and embedded images (one missing), RGB + CMYK + spot.
Outline fixture_outline();

// What the inspector shows for a multi-selection: the fields every item has,
// in the first item's order; the value where all agree, kMixed where not.
// Empty selection -> empty.
inline constexpr const char* kMixed = "Mixed";
std::vector<Field> shared_fields(const std::vector<const Item*>& sel);

// The inspector's heading for a selection: "Page 7", "3 pages", "5 items".
std::string selection_title(const std::vector<const Item*>& sel);

}  // namespace composix::core
