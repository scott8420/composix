#pragma once
// core/Swatches -- s171: colour libraries. Where swatches come from besides
// the document: My Library (the user's own, every document's), the palettes
// other programs keep on the machine (GIMP, Inkscape, LibreOffice, Scribus),
// and files the user opens. Their formats read and written here, GTK-free:
//
//   .ase   Adobe Swatch Exchange -- read, written (CMYK, RGB, Lab, Gray; spot)
//   .gpl   GIMP / Inkscape / Krita palette -- read, written (RGB)
//   .soc   LibreOffice / OpenOffice palette -- read (RGB)
//   .xml   Scribus swatches -- read (CMYK, RGB, Lab; spot)
//   .aco   Photoshop swatches -- read (RGB, HSB, CMYK, Lab, Gray; names in v2)
//   .json  Composix library (My Library's own file) -- read, written, lossless
//
// A library colour is a plain Swatch (name, model, values, spot ink,
// preview). Lab colours come in as RGB (D50 Lab to sRGB); a Lab spot keeps
// its ink with a CMYK alternate made from that. Using one copies it into
// the document (doc::add_library_swatches), its `from` naming the library:
// the document always carries its own colours.
#include "core/Doc.hpp"

#include <optional>
#include <string>
#include <vector>

namespace composix::core::swatches {

enum class Format { Unknown, ASE, GPL, SOC, Scribus, ACO, Composix };
enum class LibKind { Mine, System, Imported };

struct Library {
    std::string         name;      // shown ("Tango Icons", "My Library")
    std::string         path;      // its file ("" = none yet)
    std::string         source;    // who keeps it: "GIMP", "Inkscape", "LibreOffice", "Scribus", "File"...
    LibKind             kind   = LibKind::Imported;
    Format              format = Format::Unknown;
    std::vector<Swatch> colors;
};

const char* format_name(Format f);              // "Adobe Swatch Exchange"...
Format format_of(const std::string& path, const std::string& bytes);   // by content, then extension
bool   writable(Format f);                       // ASE, GPL, Composix

// Bytes in, a library out (`name` from the file when it has one, else
// empty); nullopt with `err` set when it isn't one.
std::optional<Library> parse(const std::string& bytes, Format f, std::string* err = nullptr);
// A file: read, sniffed, parsed; its name falls back to the file's stem.
std::optional<Library> read_file(const std::string& path, std::string* err = nullptr);

// Bytes out. GPL keeps only the screen colours (its colours are RGB).
std::string to_ase(const std::vector<Swatch>& colors);
std::string to_gpl(const std::string& name, const std::vector<Swatch>& colors);
std::string to_json(const std::string& name, const std::vector<Swatch>& colors);
bool write_file(const std::string& path, const std::string& name, const std::vector<Swatch>& colors,
                Format f, std::string* err = nullptr);

// CIE Lab (D50) to sRGB, 0xRRGGBB, clipped.
std::uint32_t lab_to_rgb(double L, double a, double b);

// My Library: a Composix library file (missing = empty). Saving makes its folder.
Library load_mine(const std::string& file);
bool    save_mine(const Library& lib);
// Colours into a library by the document's rule: the same name and values
// is there already (skipped), a name taken by other values is numbered.
// How many went in.
int     merge_into(Library& lib, const std::vector<Swatch>& colors);

// Where other programs keep palettes: $XDG_DATA_HOME / $XDG_DATA_DIRS's
// gimp, inkscape, scribus folders, GIMP's and Inkscape's config folders,
// LibreOffice's install, Flatpak's. Each palette file found (readable
// formats only), once, sorted by source then name. `home` / `data_dirs` /
// `extra` stand in for the environment and the fixed installs (the selftest's).
struct Found {
    std::string path;
    std::string source;   // "GIMP", "Inkscape", "LibreOffice", "Scribus"
};
std::vector<Found> system_palettes();
std::vector<Found> system_palettes(const std::string& home, const std::vector<std::string>& data_dirs,
                                   const std::vector<Found>& extra = {});   // more folders, each its source

}  // namespace composix::core::swatches

namespace composix::core::doc {
// s171: library colours into the document, one step ("Add 3 swatches from
// X"). A colour whose name and values the document has already is that
// swatch; a name taken by other values gets " 2", " 3"... Each new one's
// `from` is `library`. The ids, in order (existing or new); no step when
// nothing is new.
std::vector<Doc::Id> add_library_swatches(DocJournal& j, Doc& d, const std::vector<Swatch>& colors,
                                          const std::string& library);
}  // namespace composix::core::doc
