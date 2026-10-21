#include "core/Shortcuts.hpp"

#include <algorithm>
#include <cctype>
#include <map>

namespace composix::core {

// ─────────────────────────────────────────────────────────────────────────────
// Accel formatting
// ─────────────────────────────────────────────────────────────────────────────
namespace {

std::string lower(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) o += char(std::tolower((unsigned char)c));
    return o;
}

// GTK key name -> display glyph. Single alpha keys uppercase; named keys map to a
// glyph where one reads better, otherwise the name is prettified.
std::string map_key(const std::string& key) {
    if (key.empty()) return "";
    static const std::map<std::string, std::string> named = {
        {"comma", ","},     {"period", "."},     {"question", "?"},
        {"slash", "/"},     {"minus", "\u2212"}, {"plus", "+"},
        {"equal", "="},     {"space", "Space"},
        {"Return", "Enter"},{"Escape", "Esc"},
        {"Left", "\u2190"}, {"Right", "\u2192"}, {"Up", "\u2191"}, {"Down", "\u2193"},
        {"Delete", "Del"},  {"BackSpace", "Backspace"},
        {"Page_Up", "Page Up"}, {"Page_Down", "Page Down"},
        {"KP_Add", "Keypad +"}, {"KP_Subtract", "Keypad \u2212"}, {"KP_0", "Keypad 0"},
    };
    auto it = named.find(key);
    if (it != named.end()) return it->second;
    if (key.size() == 1) {
        std::string s = key;
        s[0] = char(std::toupper((unsigned char)s[0]));
        return s;
    }
    std::string s = key;
    for (char& c : s)
        if (c == '_') c = ' ';
    return s;
}

// Split an accel into its modifier flags + bare key name. Shared by
// format_accel and any future consumer so they can't disagree.
struct Parsed { bool ctrl = false, alt = false, shift = false, super = false; std::string key; };

Parsed parse_accel(const std::string& accel) {
    Parsed p;
    std::size_t i = 0;
    while (i < accel.size()) {
        if (accel[i] == '<') {
            std::size_t j = accel.find('>', i);
            if (j == std::string::npos) break;
            const std::string ml = lower(accel.substr(i + 1, j - i - 1));
            if (ml == "ctrl" || ml == "control" || ml == "primary") p.ctrl = true;
            else if (ml == "alt" || ml == "mod1")                   p.alt = true;
            else if (ml == "shift")                                 p.shift = true;
            else if (ml == "super" || ml == "meta" || ml == "mod4") p.super = true;
            i = j + 1;
        } else {
            p.key = accel.substr(i);
            break;
        }
    }
    return p;
}

}  // namespace

std::string format_accel(const std::string& accel) {
    const Parsed p = parse_accel(accel);
    const std::string k = map_key(p.key);
    std::string out;
    auto add = [&](const std::string& s) {
        if (!out.empty()) out += "+";
        out += s;
    };
    if (p.ctrl)  add("Ctrl");
    if (p.alt)   add("Alt");
    if (p.shift) add("Shift");
    if (p.super) add("Super");
    if (!k.empty()) add(k);
    return out;
}

std::string ShortcutSpec::display_keys() const {
    if (!keys.empty()) return keys;
    std::string out;
    for (const auto& a : accels) {
        if (!out.empty()) out += "  /  ";
        out += format_accel(a);
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// The registry  (authored section [A-Z] -> row)
// ─────────────────────────────────────────────────────────────────────────────
const std::vector<ShortcutSpec>& shortcut_registry() {
    // Fields: {section, action, accels, keys, description}
    // action non-empty => wired via set_accels_for_action. keys empty => derived.
    // Deliberately small -- a seed proves the SHAPE, not a product's key set. It
    // carries one doc-only row so that half of the paradigm is exercised too.
    static const std::vector<ShortcutSpec> kReg = {
        // ── Diagnostics ───────────────────────────────────────────────────────
        {"Diagnostics", "win.dump-registry", {"<Ctrl>d"}, "",
         "Dump the widget registry to the log"},

        // ── Edit (s046) ───────────────────────────────────────────────────────
        // Doc-only ON PURPOSE: an accelerator would take Ctrl+Z from every text
        // box. The window hears these instead (Shell_bindings.cpp).
        {"Edit", "", {}, "Ctrl+Z", "Undo -- anywhere but a text box (which keeps its own)"},
        {"Edit", "", {}, "Ctrl+Shift+Z  /  Ctrl+Y", "Redo"},

        // ── Frames (s070) ─────────────────────────────────────────────────────
        // Shift turns [ ] into { } on the way in, so the braces are bound too.
        {"Frames", "win.bring-front",   {"<Ctrl><Shift>bracketright", "<Ctrl><Shift>braceright"}, "Ctrl+Shift+]", "Bring the picked frames to the front"},
        {"Frames", "win.bring-forward", {"<Ctrl>bracketright"}, "", "Bring the picked frames forward one"},
        {"Frames", "win.duplicate-frames", {"<Ctrl><Shift><Alt>d"}, "", "Duplicate the picked frames (a step down and right; text: a copy of the story)"},   // s282
        {"Frames", "win.group",          {"<Ctrl>g"}, "", "Group the picked frames -- picked and moved as one (Ctrl+click: one inside)"},   // s300
        {"Frames", "win.ungroup",        {"<Ctrl><Shift>g"}, "", "Ungroup"},
        {"Frames", "win.lock",           {"<Ctrl>l"}, "", "Lock the picked frames -- not picked or moved until unlocked"},   // s301
        {"Frames", "win.unlock-spread",  {"<Ctrl><Alt>l"}, "", "Unlock every frame on the spread shown"},
        {"Frames", "win.step-repeat",    {"<Ctrl><Shift><Alt>u"}, "", "Step and Repeat -- copies of the picked frames, each a step on"},   // s284 (Ctrl+Alt+U is the index mark here)
        {"Frames", "win.copy-frames",    {}, "Ctrl+C", "Copy the picked frames (not while typing: then the words)"},
        {"Frames", "win.cut-frames",     {}, "Ctrl+X", "Cut the picked frames"},
        {"Frames", "win.paste-frames",   {}, "Ctrl+V", "Paste frames -- in the middle of the view, on the page there"},
        {"Frames", "win.paste-in-place", {}, "Ctrl+Alt+Shift+V", "Paste frames where they were, on the page shown"},
        {"Frames", "win.send-backward", {"<Ctrl>bracketleft"}, "", "Send the picked frames backward one"},
        {"Frames", "win.send-back",     {"<Ctrl><Shift>bracketleft", "<Ctrl><Shift>braceleft"}, "Ctrl+Shift+[", "Send the picked frames to the back"},
        // s212: compound paths (InDesign's keys)
        {"Frames", "win.make-compound",    {"<Ctrl>8"}, "", "Make the picked shapes one compound path (where they overlap: holes)"},
        {"Frames", "win.release-compound", {"<Ctrl><Alt>8"}, "", "Release a compound path into its separate paths"},


        // ── General ───────────────────────────────────────────────────────────
        {"General", "win.new",           {"<Ctrl>n"}, "", "New document"},
        {"General", "win.open",          {"<Ctrl>o"}, "", "Open a project"},
        {"General", "win.save",          {"<Ctrl>s"}, "", "Save"},
        {"General", "win.save-as",       {"<Ctrl><Shift>s"}, "", "Save as a new project folder"},
        {"General", "win.close-project", {"<Ctrl>w"}, "", "Close the project (asking to save changes) -- an empty Untitled one in its place"},   // s145
        {"General", "win.export-pdf",    {"<Ctrl>e"}, "", "Export PDF"},
        {"General", "win.story-editor",  {"<Ctrl><Shift>y"}, "", "Story editor: the story being edited / picked, as running text"},
        {"General", "win.find",          {"<Ctrl>f"}, "", "Find / Change (across every story)"},
        {"General", "win.place",         {"<Ctrl><Shift>p"}, "", "Place an image (into the picked image frame, or click / drag on a page)"},
        {"General", "win.shortcuts",     {"<Ctrl>question", "<Ctrl>slash"}, "",
         "Keyboard shortcuts (this window)"},
        {"General", "win.about",         {"F1"}, "", "About Composix"},
        {"General", "win.quit",          {"<Ctrl>q"}, "", "Quit"},

        // ── Mouse ─────────────────────────────────────────────────────────────
        // Doc-only: a gesture is not a GAction, but the reader still needs it.
        {"Mouse", "", {}, "Click a recent", "Open that project"},
        {"Mouse", "", {}, "Ctrl+click  /  Shift+click", "Add to the selection (navigator, tiles)"},
        {"Mouse", "", {}, "Double-click a tile", "Open it (or the whole selection)"},
        {"Mouse", "", {}, "Drag a frame / a handle", "Move / resize (Shift: one axis / keep shape; Alt: from the centre)"},
        {"Mouse", "", {}, "Arrow keys", "Nudge the picked frames 1 pt (Shift: 10 pt)"},
        {"Mouse", "", {}, "V  /  A  /  T  /  F  /  M", "Select / Direct (a picture in its frame) / Text frame / Image frame / Shape tool (canvas)"},
        {"Mouse", "", {}, "Delete", "Delete the picked frames (a thread heals around them)"},
        {"Mouse", "", {}, "Click an out-port, then a frame", "Thread the text on into that frame"},
        {"Mouse", "", {}, "Alt+click an out-port", "Break the thread after that frame"},
        {"Mouse", "", {}, "Double-click a text frame  /  T + click", "Edit its text (a new text frame starts editing)"},
        {"Mouse", "", {}, "Drag  /  double-click in text", "Select text  /  a word"},
        {"Mouse", "", {}, "Ctrl+Left / Right, Home / End", "Text: by word, to the line's ends (Ctrl: the story's)"},
        {"Mouse", "", {}, "Ctrl+A / C / X / V", "Text: select the story, copy, cut, paste"},
        {"Mouse", "", {}, "Esc (editing)", "Stop editing; the frame stays picked"},
        {"Mouse", "", {}, "Drag page tiles  /  Delete", "Reorder the pages  /  delete the picked pages"},
        {"Mouse", "", {}, "Text tool: click inside the margins", "A text frame filling that page column"},
        {"Mouse", "", {}, "Drag a gutter (a picked text frame)", "Make one column wider, its neighbour narrower"},
        {"Mouse", "", {}, "Drop an image file on a page", "Place it there (on an image frame: into it)"},
        {"Mouse", "", {}, "Tab ruler", "While typing: click the strip over the column to add a tab stop; drag to move, drag off to remove; double-click: left / centre / right / decimal; Shift+double-click: dot leader. These are the paragraph's own stops (the selected paragraphs'); \u21BA Reset to style puts back its style's"},
        {"Mouse", "", {}, "Ctrl+Shift+click", "Override a master item on a page (its own copy; Remove override goes back)"},
        {"Mouse", "", {}, "Esc", "Cancel a drag, put down a loaded image, then clear the picks"},

        // ── Text (s023: special characters, while editing) ─────────────────
        {"Text", "win.bold",                {"<Ctrl>b"}, "", "Bold the selected words (or the word at the caret) -- again: off"},
        {"Text", "win.italic",              {"<Ctrl>i"}, "", "Italic the selected words (or the word at the caret) -- again: off"},
        {"Text", "win.underline",           {"<Ctrl>u"}, "", "Underline the selected words -- again: off"},   // s263
        {"Text", "win.strikethrough",       {"<Ctrl><Alt>s"}, "", "Strike through the selected words -- again: off"},
        {"Text", "win.superscript",         {"<Ctrl><Alt>equal", "<Ctrl><Alt>plus"}, "Ctrl+Alt+=", "Superscript (smaller, raised) -- again: normal"},
        {"Text", "win.subscript",           {"<Ctrl><Alt>minus"}, "", "Subscript (smaller, lowered) -- again: normal"},
        {"Text", "win.all-caps",            {"<Ctrl><Shift>k"}, "", "All caps (the letters as typed are kept) -- again: off"},
        {"Text", "win.small-caps",          {"<Ctrl><Shift>h"}, "", "Small caps -- again: off"},
        {"Text", "", {}, "Alt+Left / Right (typing)", "Kerning at the caret, 20/1000 em (Ctrl: 100); a selection: tracking"},   // s265
        {"Text", "", {}, "Alt+Shift+Up / Down (typing)", "Baseline shift of the selection (or word), 1 pt (Ctrl: 5)"},
        {"Text", "win.insert-page-number",  {"<Ctrl><Alt><Shift>n"}, "", "Insert the page number (each page shows its own)"},
        {"Text", "win.insert-section-name", {"<Ctrl><Alt><Shift>e"}, "", "Insert the section name (a running head)"},
        {"Text", "win.insert-next-page",    {"<Ctrl><Alt>bracketright"}, "", "Insert the next frame's page number (\"continued on\")"},
        {"Text", "win.insert-prev-page",    {"<Ctrl><Alt>bracketleft"}, "", "Insert the previous frame's page number (\"continued from\")"},
        {"Text", "win.insert-footnote",     {"<Ctrl><Alt>f"}, "", "Insert a footnote at the text cursor, and type its words"},   // s110
        {"Text", "win.index-mark",          {"<Ctrl><Alt>u"}, "", "Mark the selection for the index (its words the term, to start)"},   // s138
        {"Text", "", {}, "Keypad Enter", "Column break -- the text after it starts the next column (s055)"},
        {"Text", "", {}, "Ctrl+Shift+-", "Discretionary hyphen -- the word may break only there (one at its start: never)"},
        {"Text", "", {}, "Shift+Keypad Enter", "Frame break -- the text after it starts the next frame"},
        {"Text", "", {}, "Ctrl+Keypad Enter", "Page break -- the text after it starts the next page"},

        // ── Tools (s212: the canvas's letters, doc-only -- the canvas hears them) ──
        {"Tools", "", {}, "V", "Select"},
        {"Tools", "", {}, "A", "Direct -- a picture in its frame; a shape's points (Shift-click, marquee, arrows, Delete)"},
        {"Tools", "", {}, "T", "Text frame"},
        {"Tools", "", {}, "F", "Image frame"},
        {"Tools", "", {}, "M  /  L  /  \\", "Rectangle / Oval / Line (the Shape button: click it again for its menu, Polygon too)"},
        {"Tools", "", {}, "P", "Pen -- click corners, drag curves; on an open path's end: carry it on"},
        {"Tools", "", {}, "I", "Eyedropper -- the page's colour onto the fill (Shift: stroke; Alt: a swatch)"},

        // ── View (s002) ───────────────────────────────────────────────────────
        {"View", "", {}, "Page Up / Page Down", "The previous / next spread (pages open with their facing page)"},
        {"View", "win.zoom-in",          {"<Ctrl>equal", "<Ctrl>plus"}, "", "Zoom in"},
        {"View", "win.zoom-out",         {"<Ctrl>minus"}, "", "Zoom out"},
        {"View", "win.zoom-fit",         {"<Ctrl>0"}, "", "Fit the pages"},
        {"View", "win.zoom-100",         {"<Ctrl>1"}, "", "Actual size (100%)"},
        {"View", "win.zoom-200",         {"<Ctrl>2"}, "", "Zoom to 200%"},   // s241 (Scott)
        {"View", "win.zoom-300",         {"<Ctrl>3"}, "", "Zoom to 300%"},
        {"View", "", {}, "Ctrl+scroll", "Zoom about the pointer"},
        {"View", "", {}, "Space+drag  /  middle-drag", "Pan the canvas"},
        {"View", "win.toggle-rulers",    {"<Ctrl>r"}, "", "Show / hide the rulers"},
        {"View", "win.toggle-guides",    {"<Ctrl>apostrophe"}, "", "Show / hide the guides"},
        {"View", "win.toggle-grid",      {"<Ctrl><Alt>apostrophe"}, "", "Show / hide the baseline grid"},
        {"View", "win.toggle-hidden",    {"<Ctrl><Alt>i"}, "", "Show / hide hidden characters (spaces, tabs, paragraph ends, breaks)"},
        {"View", "win.toggle-proof",     {"<Ctrl><Alt>y"}, "", "Proof colours: the pages as the export's press profile will print them"},   // s099
        {"View", "win.toggle-snap",      {"<Ctrl>semicolon"}, "", "Snap to guides & edges on / off"},
        {"View", "win.toggle-guide-lock", {"<Ctrl><Alt>semicolon"}, "", "Lock / unlock the guides (locked: not moved or pulled off)"},   // s153
        {"View", "", {}, "Drag out of a ruler", "Pull a guide onto a page -- it snaps to edges, margins, columns, guides (off the page removes it)"},
        {"View", "win.go-up",            {"<Alt>Left", "<Alt>Up"}, "", "Back -- up a level, from an open page, master, story ... to its section's tiles (mouse: back button)"},
        {"View", "win.go-forward",       {"<Alt>Right"}, "", "Forward -- back into what you just came up from (mouse: forward button)"},
        {"View", "win.go-to-page",       {"<Ctrl>j"}, "", "Go to page -- type a folio (8, iv), +N for the Nth page, or a master's prefix (A)"},   // s277
        {"View", "win.prev-spread",      {}, "Page Up", "The previous spread"},
        {"View", "win.next-spread",      {}, "Page Down", "The next spread"},
        {"View", "win.toggle-nav",       {"<Ctrl><Alt>1"}, "", "Show / hide the navigator"},
        {"View", "win.toggle-inspector", {"<Ctrl><Alt>2"}, "", "Show / hide the inspector"},

    };
    return kReg;
}

// ─────────────────────────────────────────────────────────────────────────────
// Collision detection
// ─────────────────────────────────────────────────────────────────────────────
std::vector<std::string> find_accel_collisions() {
    std::map<std::string, std::pair<int, bool>> seen;  // accel -> {count, any_action}
    for (const auto& s : shortcut_registry())
        for (const auto& a : s.accels) {
            auto& e = seen[a];
            e.first += 1;
            if (!s.action.empty()) e.second = true;
        }
    std::vector<std::string> out;
    for (const auto& [accel, e] : seen)
        if (e.first >= 2 && e.second) out.push_back(accel);  // std::map keeps it sorted+unique
    return out;
}

}  // namespace composix::core
