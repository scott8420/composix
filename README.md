# Composix

A page-layout application for the GNOME desktop -- gtkmm 4 / C++23. Text and
image frames on pages, threaded text, styles, and PDF/X export. The far end of
the pipeline: paint in Krita -> touch up -> place in Composix -> PDF/X.

Seeded from **Cairn** (the house gtkmm paradigm seed); the paradigms below are
inherited and still exercised. Drawing / text tools will be patterned on Curvz.

## The one non-negotiable

Every paradigm here **compiles and is exercised** — the GTK app builds, and the
GTK-free core runs under `cairn_selftest` (`87 pass / 0 fail`). An un-run
reference is worse than nothing: it looks authoritative and lies (CANON: "a claim
in the doc isn't a fact until a consumer exercises it"). So nothing goes in as
dead code pretending to work. Where a paradigm can't be honestly exercised here
(e.g. a scripting resolver with no DSL to drive it), it's *documented* and points
at the source, never faked.

## What's in it

| Paradigm | Where | Distilled from |
|---|---|---|
| `Named<W>` + live registry (dotted names, `dump()`) | `include/widgets/`, `Registry.*` | Optik |
| Per-area spdlog, runtime-toggleable | `Log.*` | Optik / Curvz |
| Split class by category + header-as-index | `Shell.hpp` + `Shell_{zones,bindings,handlers,helpers}.cpp` | Curvz / Optik |
| Actions, parameterised actions, Stack/StackSidebar | `Shell_*` | Curvz |
| JSON persistence pump + recent menu (two surfaces) | `core/Recents.*`, `Shell_helpers.cpp` | Optik |
| Forward/inverse text mapping + round-trip test | `core/TextMap.*`, `selftest.cpp` | Folio |
| Resource pipeline (symbolic icon from a compiled-in gresource) + dialog lifetime (hide-on-close About) | `resources/`, `AboutWindow.*`, `App.cpp` | Optik / Curvz |
| The model↔UI seam (proven three ways) | `docs/two-layer-seam.md` | Folio |
| The widget-naming fork (which half to build) | `docs/widget-naming.md` | Curvz vs Optik |
| Resources & dialog lifetime (the two gotchas) | `docs/resources-and-dialog-lifetime.md` | Optik / Curvz |
| Following the desktop's light/dark preference | `Appearance.*`, `docs/following-the-desktop-appearance.md` | Plunk / Slate |
| **Undo: every user choice a step, one door + a gate** -- a journal of before/after pictures by id, gestures as one step, merging, selection, Ctrl+Z from anywhere but a text box | `core/Undo.hpp`, `core/Stones.*`, the Undo page, `docs/undo.md` | Curvz / jot (s046) |
| **Segmented navigator -> tiles -> inspector** (s002): sections disclose to items; a section shows as tiles with metadata, items open; multi-select with shared fields | `core/Outline.*`, `views/*`, `resources/composix.css`, `docs/look.md` | Composix |
| **The document model** (s004): one store of records by id, verbs through the journal, sample book, document.json project folder, projection to the views | `core/Doc.*`, `core/Sample.cpp`, `core/DocJson.cpp`, `core/Project.cpp` | Composix |
| **The page canvas** (s005): viewport (zoom about the pointer, fit, pan), pages at true size, Select tool (pick, marquee, move, 8-handle resize) -- every drag one journal gesture | `core/Geometry.hpp`, `views/Canvas.*` | Curvz (ideas) |
| **Rulers, guides, snapping** (s006): rulers read the canvas viewport; guides are records pulled from the rulers; snap targets gathered at press | `views/Ruler.*`, `core::snap_targets / snap_any` | Curvz (ideas) |
| **Text flow** (s007): stories flowed through threaded frames with Pango, measured at 72 dpi unhinted, overset detected -- a GTK-free library | `layout/Flow.*` (`composix_layout`) | Composix |
| The tree, and drag-and-drop on it — **documented, not compiled** | `docs/tree-and-dnd.md` | Folio |

Three sources, each earning a page only where it's the *cleanest* instance:
**Curvz** = the GTK disciplines, **Optik** = the seam + the pump, **Folio** = the
seam proven three ways + the mapping.

## Build & run

```
./build.sh
./build/cairn_selftest     # exercises the GTK-free core
./build/cairn              # the app: menu -> "Remember a folder...", then
                           # Open recent; the Registry page dumps the live tree.
                           # The logo (top-left) and menu -> "About Cairn" open
                           # the hide-on-close About window (a symbolic icon
                           # resolved from the compiled-in gresource bundle).
                           # The Undo page: delete, drag, rename -- Ctrl+Z.
./build/composix_bench     # s100: timings on the generated ~400-page book
                           # (flow, a keystroke's reflow, drawing, thumbnails);
                           # --pages N, --no-pictures, --pdf. Not a gate.
```

Deps (Ubuntu/Debian): `libgtkmm-4.0-dev libspdlog-dev cmake g++`.
Deps (Fedora): `gtkmm4.0-devel spdlog-devel cmake gcc-c++` (pangocairo comes with gtkmm). The resource
bundle is compiled by `glib-compile-resources`, which ships with the GLib dev
tools that `libgtkmm-4.0-dev` already pulls in.

## Install & uninstall (s331)

```
./install.sh                # after ./build.sh: into ~/.local, for you (no sudo)
./install.sh --build        # build, then install
./install.sh --system       # into /usr/local, for everyone (sudo)
./install.sh --prefix DIR   # anywhere else
./uninstall.sh              # takes out what install.sh put in (same options)
./uninstall.sh --purge      # ... and your Composix settings, recents, cache
```

It installs the binary, a launcher (`io.github.scott8420.Composix.desktop`,
so Composix shows in the desktop's applications) and its icon (`data/`) --
the app's own icons and stylesheet are inside the binary. A manifest in
`PREFIX/share/composix/` records what went in; uninstall removes exactly that.
Never touched: your documents, the swatch library and the pictures ODT import
saved (`~/.local/share/composix/swatches`, `imported/`), even with `--purge`.

## Handing the tree to a session

```
../scripts/composix_backup.sh s002_page_model   # -> backups/s002_page_model.zip
```

Allowlist-based, so build artifacts can't ride along. Paths derive from the
script's own location. Previous zips retire to `backups/Old/`. The zip carries
`composix/`, `docs/` and `scripts/` (`fixes/` stays local).

## A note on what a stone looks like

CANON gained two entries (*A milestone ends at a surface*, *Fixture
data through the real seam*) that bear directly on how a project
seeded from here should open. Both are marked **banked — one instance,
unverified**, which is the seed's own standard: an entry earns
permanent status by a project hitting the pain and recovering, not by
a conversation agreeing it sounds right. Read the banner before
treating them as settled.

## Growth backlog (each a future stone, added only when it earns a page)

**The tree is the one that matters.** The GTK4 list/tree stack — `ListView`,
`ColumnView`, `ListStore`, `SignalListItemFactory`, `TreeListModel`, selection
models, sorters, filters — is absent here and is the most-rediscovered, most
gotcha-laden paradigm in GTK4. Every app we've built needs it. It is the next
stone, and it becomes one by being *built in anger in a real project and
distilled back*, not invented in the seed.

`docs/tree-and-dnd.md` holds the interim map, distilled from Folio's binder and
marked as not-compiled-here. That marking is not decoration: it is the
difference between this page and every other one in the table.

Also queued: `GtkRevealer` drawer · DrawingArea + Cairo overlay & hit-testing
(from Optik's box overlay) · two-controller CAPTURE keystroke · `TextView` /
`TextBuffer` with live tag styling. Each ships compiled and exercised, or it
doesn't ship.

(Landed since v1: **undo** (s046) -- a project seeded from here starts with
the journal and the gate; replace `core/Stones` with the real model and keep
every verb going through `Journal::run`. And the resource pipeline + hide-on-close dialog lifetime — see the
table above and `docs/resources-and-dialog-lifetime.md`.)
