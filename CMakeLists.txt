cmake_minimum_required(VERSION 3.20)
project(composix VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# s194: faster builds, each tool only when it's installed (none is required).
#  ccache  -- a rebuild of an unchanged file is a cache hit, even after
#             rm -rf build (Fedora/Ubuntu package: ccache). COMPOSIX_CCACHE=OFF
#             to leave it out.
#  mold / lld -- the links (three executables, Debug-sized) go faster than ld.bfd.
#             COMPOSIX_LINKER=bfd to keep the default.
#  Ninja is build.sh's choice (it passes -G Ninja when ninja is there).
option(COMPOSIX_CCACHE "Compile through ccache when it is installed" ON)
if(COMPOSIX_CCACHE AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
    find_program(CCACHE_PROGRAM ccache)
    if(CCACHE_PROGRAM)
        set(CMAKE_C_COMPILER_LAUNCHER   ${CCACHE_PROGRAM})
        set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
        message(STATUS "ccache: ${CCACHE_PROGRAM}")
    endif()
endif()
set(COMPOSIX_LINKER "" CACHE STRING "mold, lld or bfd (empty: the fastest one installed)")
if(NOT COMPOSIX_LINKER)
    find_program(MOLD_PROGRAM mold)
    find_program(LLD_PROGRAM ld.lld)
    if(MOLD_PROGRAM)
        set(COMPOSIX_LINKER_USE mold)
    elseif(LLD_PROGRAM)
        set(COMPOSIX_LINKER_USE lld)
    endif()
else()
    set(COMPOSIX_LINKER_USE ${COMPOSIX_LINKER})
endif()
if(COMPOSIX_LINKER_USE AND NOT COMPOSIX_LINKER_USE STREQUAL "bfd")
    include(CheckLinkerFlag)
    check_linker_flag(CXX "-fuse-ld=${COMPOSIX_LINKER_USE}" COMPOSIX_HAS_LD_${COMPOSIX_LINKER_USE})
    if(COMPOSIX_HAS_LD_${COMPOSIX_LINKER_USE})
        add_link_options("-fuse-ld=${COMPOSIX_LINKER_USE}")
        message(STATUS "linker: ${COMPOSIX_LINKER_USE}")
    endif()
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTKMM REQUIRED gtkmm-4.0)
find_package(spdlog REQUIRED)
find_package(Threads REQUIRED)   # s102: work off the UI thread (the progress dialog)
find_package(ZLIB REQUIRED)      # s202: the core unzips SwatchBooker (.sbz) files
# s213: Clipper2 (Angus Johnson, Boost 1.0) -- shapes united / subtracted /
# intersected, paths offset. The system's when there is one (Fedora:
# clipper2-devel, as Curvz uses), else the copy in third_party/clipper2.
find_package(Clipper2 QUIET)
if(NOT Clipper2_FOUND AND NOT TARGET Clipper2)
    add_library(Clipper2 STATIC
        third_party/clipper2/src/clipper.engine.cpp
        third_party/clipper2/src/clipper.offset.cpp
        third_party/clipper2/src/clipper.rectclip.cpp)
    target_include_directories(Clipper2 SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/clipper2/include)
    target_compile_options(Clipper2 PRIVATE -w)
    message(STATUS "Clipper2: the copy in third_party")
else()
    message(STATUS "Clipper2: the system's")
endif()

# ── composix_core -- the GTK-free core (the model side of the one real seam) ──────
# No gtkmm on this library's link line, BY DESIGN: a stray UI include under
# core/ fails to compile, so the seam is enforced by the build graph and the
# core stays headless-testable. (docs/two-layer-seam.md.)
# s038: the hyphenation patterns (third_party/hyphen) become a source file, so
# the app needs no hyphenation package at run time. s190: a language each
# (code:file) -- the patterns and, where there are any, the exceptions.
set(CX_HYPH_LANGS "en-US:en-us" "en-GB:en-gb" "de:de-1996" "fr:fr" "es:es" "it:it" "nl:nl" "pt:pt")
set(CX_HYPH_SETS "")
set(CX_HYPH_COUNT 0)
foreach(pair IN LISTS CX_HYPH_LANGS)
    string(REPLACE ":" ";" parts "${pair}")
    list(GET parts 0 code)
    list(GET parts 1 file)
    set(pat ${CMAKE_CURRENT_SOURCE_DIR}/third_party/hyphen/hyph-${file}.pat.txt)
    set(hyp ${CMAKE_CURRENT_SOURCE_DIR}/third_party/hyphen/hyph-${file}.hyp.txt)
    file(READ ${pat} P)
    set(E "")
    if(EXISTS ${hyp})
        file(READ ${hyp} E)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${hyp})
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${pat})
    string(APPEND CX_HYPH_SETS "    {\"${code}\", R\"HYPH(${P})HYPH\", R\"HYPH(${E})HYPH\"},\n")
    math(EXPR CX_HYPH_COUNT "${CX_HYPH_COUNT} + 1")
endforeach()
configure_file(src/core/HyphenData.cpp.in ${CMAKE_CURRENT_BINARY_DIR}/HyphenData.cpp @ONLY)

add_library(composix_core STATIC
    src/core/Recents.cpp
    src/core/Settings.cpp
    src/core/TextMap.cpp
    # The SHORTCUT-REGISTRY paradigm: one list, two consumers (App wires accels
    # from it, ShortcutsDialog renders from it) so a key and its advertised row
    # can't drift. GTK-free, so it lives in core and the selftest guards it
    # headless -- a pure test proves no two chords collide.
    src/core/Shortcuts.cpp
    # The UNDO paradigm (s046): core/Undo.hpp is the journal (header-only, a
    # template over the app's store); Stones is the demo model + its verbs +
    # the verb table the selftest gate walks. docs/undo.md.
    src/core/Stones.cpp
    # s002: the projection the navigator / tiles / inspector read.
    src/core/Outline.cpp
    # s004: the document model, its verbs, the sample, JSON, the projection.
    src/core/Doc.cpp
    src/core/Sample.cpp
    src/core/NewDoc.cpp
    src/core/Find.cpp
    src/core/Swatches.cpp
    src/core/DocJson.cpp
    src/core/Project.cpp
    # s038: hyphenation -- Liang's algorithm; the TeX patterns embedded below.
    src/core/Hyphen.cpp
    src/core/Pathfinder.cpp   # s213: shapes combined (Clipper2), curves fitted back
    src/core/Zip.cpp          # s235: zip entries (ODT, SwatchBooker)
    src/core/Xml.cpp          # s235: an XML tree (ODT import)
    src/core/Odt.cpp          # s235: an OpenDocument text file, read
    ${CMAKE_CURRENT_BINARY_DIR}/HyphenData.cpp
)
target_include_directories(composix_core PUBLIC include)
target_link_libraries(composix_core PUBLIC ZLIB::ZLIB PRIVATE Clipper2)   # s202; s213: Clipper2
# Vendored nlohmann/json (header-only, MIT), PRIVATE -- an implementation detail
# of the pump, no consumer inherits the include (CANON: surprise-free build).
target_include_directories(composix_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# ── Plate (s015): our PDF writer -- a standalone library in libs/plate, reusable
# by other apps (its own CMakeLists, tests, README). plate_pango feeds it Pango
# lines. composix_layout's print exporter writes through it.
add_subdirectory(libs/plate)

# ── composix_layout -- text flow (s007): Pango + cairo, NO gtk ─────────────────
# Its own library so the flow is measured and selftested headless. The app and
# the selftest both link it; core stays free of it.
pkg_check_modules(PANGOCAIRO REQUIRED pangocairo)
add_library(composix_layout STATIC
    src/layout/Flow.cpp
    # s013: PDF export (cairo's PDF surface; images handed in by the app).
    src/layout/Export.cpp
    # s015: print PDF through Plate (page boxes, CMYK colour, subset fonts).
    src/layout/PlateExport.cpp
    src/layout/Preflight.cpp
    # s053: the colour report -- RGB proofed through the press profile.
    src/layout/ColorReport.cpp
    src/layout/Psd.cpp          # s185: a PSD's merged picture
    # s100: the generated long test book (fitted by flowing it).
    src/layout/BigBook.cpp
    src/layout/OdtImport.cpp    # s236: an ODT made a document (its pages fitted by flowing it)
    # s103: page thumbnails on a worker thread, from a document snapshot.
    src/layout/ThumbStore.cpp
)
target_include_directories(composix_layout PUBLIC include ${PANGOCAIRO_INCLUDE_DIRS})
target_link_libraries(composix_layout PUBLIC composix_core plate_pango ${PANGOCAIRO_LIBRARIES} Threads::Threads)
target_link_directories(composix_layout PUBLIC ${PANGOCAIRO_LIBRARY_DIRS})
target_compile_options(composix_layout PUBLIC ${PANGOCAIRO_CFLAGS_OTHER})

# ── Headless selftest -- exercises the core with no display ────────────────────
# s196: its blocks in eight files, so they compile side by side (one file was
# the build's slowest by far). src/selftest/Selftest.hpp says how to add one.
add_executable(composix_selftest src/selftest.cpp
    src/selftest/part1.cpp src/selftest/part2.cpp src/selftest/part3.cpp src/selftest/part4.cpp
    src/selftest/part5.cpp src/selftest/part6.cpp src/selftest/part7.cpp src/selftest/part8.cpp)
target_link_libraries(composix_selftest PRIVATE composix_core composix_layout Threads::Threads)
# s202: where the real swatch files (third_party/swatch-samples) are
target_compile_definitions(composix_selftest PRIVATE COMPOSIX_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

# ── s100: timings on the generated long book -- run by hand, not a gate ───────
add_executable(composix_bench src/bench.cpp)
target_link_libraries(composix_bench PRIVATE composix_core composix_layout Threads::Threads)

# ── App resources (gresource) -- the RESOURCE-PIPELINE paradigm ────────────────
# glib-compile-resources embeds resources/composix.gresource.xml into a C source;
# App::on_activate registers it and points the icon theme at the bundle so
# set_icon_name("composix-logo-symbolic") resolves out of the binary. The generated
# .c compiles as C (project enables C above) so composix_resources_get_resource()
# has C linkage, matching the extern "C" prototype in App.cpp. Adding an icon:
# drop the .svg in resources/icons/scalable/apps/, list it in the .xml, rebuild.
# (docs/resources-and-dialog-lifetime.md)
find_program(GLIB_COMPILE_RESOURCES glib-compile-resources REQUIRED)
set(COMPOSIX_GRESOURCE_XML "${CMAKE_CURRENT_SOURCE_DIR}/resources/composix.gresource.xml")
set(COMPOSIX_RESOURCES_C   "${CMAKE_CURRENT_BINARY_DIR}/composix-resources.c")
file(GLOB COMPOSIX_RESOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/scalable/apps/*.svg"
     "${CMAKE_CURRENT_SOURCE_DIR}/resources/*.css")
add_custom_command(
    OUTPUT  "${COMPOSIX_RESOURCES_C}"
    COMMAND "${GLIB_COMPILE_RESOURCES}"
            --target "${COMPOSIX_RESOURCES_C}"
            --generate-source
            --c-name composix_resources
            composix.gresource.xml
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/resources"
    DEPENDS "${COMPOSIX_GRESOURCE_XML}" ${COMPOSIX_RESOURCE_FILES}
    COMMENT "Compiling gresource bundle (composix-resources.c)"
    VERBATIM)

# ── composix -- the GTK app ───────────────────────────────────────────────────────
# The Shell is a SPLIT CLASS across TUs by category (see the index in
# include/Shell.hpp); all parts compile into the one composix::Shell.
add_executable(composix
    ${COMPOSIX_RESOURCES_C}
    src/main.cpp
    src/App.cpp
    # Follows the desktop's light/dark preference (GTK4 will not do it for us
    # and we do not link libadwaita). Needs no new dependency: the D-Bus and
    # GSettings calls are GIO, which gtkmm-4.0 already brings in.
    src/Appearance.cpp
    src/Shell.cpp
    src/Shell_zones.cpp
    src/Shell_bindings.cpp
    src/Shell_handlers.cpp
    src/Shell_new.cpp
    src/Shell_find.cpp
    src/Shell_colorreport.cpp
    src/Shell_swatch.cpp
    src/Shell_paths.cpp         # s214: Pathfinder, Offset Path, Outline Stroke
    src/Shell_libraries.cpp
    src/Shell_start.cpp
    src/Shell_preflight.cpp
    src/Shell_notes.cpp    # s110: footnotes -- insert, and their words in a popover
    src/Shell_notes_panel.cpp   # s121: Footnotes & Endnotes, one panel
    src/Shell_xref_panel.cpp    # s133: Cross-References, one panel
    src/Shell_index.cpp         # s138: the index -- mark, edit, new
    src/Shell_sources_panel.cpp # s144: Sources, one panel
    src/Shell_xref.cpp     # s128: cross-references -- insert one at the cursor
    src/Shell_reflow.cpp   # s106: the far end of a long story on a worker
    src/Shell_busy.cpp
    src/Shell_helpers.cpp
    # s002: the segmented navigator, tile view, item preview, inspector.
    src/views/Navigator.cpp
    src/views/TileView.cpp
    src/views/ItemView.cpp
    src/views/StoryEditor.cpp
    src/views/Inspector.cpp
    src/views/Thumbs.cpp
    src/views/PageThumbs.cpp
    # s003: the inspector's handle + pull tab (from Samson).
    src/views/PanelHandle.cpp
    # s005: the page canvas.
    src/views/Canvas.cpp
    src/views/PagePaint.cpp   # s104: the canvas page, drawn on any thread
    # s006: rulers.
    src/views/Ruler.cpp
    # s009: probing + decoding image files.
    src/views/Images.cpp
    src/AboutWindow.cpp
    src/ShortcutsDialog.cpp
    src/Registry.cpp
    src/Log.cpp
)
target_include_directories(composix PRIVATE include ${GTKMM_INCLUDE_DIRS})
target_link_libraries(composix     PRIVATE ${GTKMM_LIBRARIES} composix_core composix_layout spdlog::spdlog Threads::Threads)
target_link_directories(composix   PRIVATE ${GTKMM_LIBRARY_DIRS})
target_compile_options(composix    PRIVATE ${GTKMM_CFLAGS_OTHER})
# s192: the About window's version, and the session that built it (bump
# COMPOSIX_BUILD with each session's ship, so the two machines can tell).
set(COMPOSIX_BUILD "s250")
# s194: on the About window's file alone -- on the whole target, each bump
# rebuilt every app file and missed ccache.
set_source_files_properties(src/AboutWindow.cpp PROPERTIES
    COMPILE_DEFINITIONS "COMPOSIX_VERSION=\"${PROJECT_VERSION}\";COMPOSIX_BUILD=\"${COMPOSIX_BUILD}\"")
# s187: libtiff, when it's there -- a CMYK TIFF's own separations into the
# print PDF (without it a CMYK TIFF goes through RGB, as before).
pkg_check_modules(TIFF libtiff-4)
if(TIFF_FOUND)
    target_compile_definitions(composix PRIVATE COMPOSIX_HAVE_TIFF=1)
    target_include_directories(composix PRIVATE ${TIFF_INCLUDE_DIRS})
    target_link_libraries(composix PRIVATE ${TIFF_LIBRARIES})
    target_link_directories(composix PRIVATE ${TIFF_LIBRARY_DIRS})
    message(STATUS "libtiff ${TIFF_VERSION}: CMYK TIFFs print as CMYK")
else()
    message(STATUS "libtiff not found: CMYK TIFFs print through RGB")
endif()

# ── s195: precompiled headers ────────────────────────────────────────────────
# The standard library a TU each (every core / layout file opens with the same
# dozen), gtkmm's commonest widgets for the app. System headers only: our own
# change too often, and the core's PCH stays GTK-free like the core. Files
# still include what they use -- COMPOSIX_PCH=OFF builds without, and must.
option(COMPOSIX_PCH "Precompile the common system headers" ON)
if(COMPOSIX_PCH)
    set(CX_PCH_STD
        <algorithm> <array> <chrono> <cmath> <cstddef> <cstdint> <cstdio> <cstring>
        <filesystem> <fstream> <functional> <map> <memory> <optional> <set>
        <sstream> <string> <string_view> <unordered_map> <utility> <vector>)
    target_precompile_headers(composix_core     PRIVATE ${CX_PCH_STD})
    target_precompile_headers(composix_layout   PRIVATE ${CX_PCH_STD} <cairo.h> <pango/pangocairo.h>)
    target_precompile_headers(composix_selftest PRIVATE ${CX_PCH_STD} <regex> <cairo.h>)
    target_precompile_headers(composix PRIVATE ${CX_PCH_STD}
        <glibmm/main.h> <glibmm/dispatcher.h> <glibmm/miscutils.h> <sigc++/signal.h>
        <gdkmm/cursor.h> <pangomm/layout.h>
        <gtkmm/alertdialog.h> <gtkmm/box.h> <gtkmm/button.h> <gtkmm/checkbutton.h>
        <gtkmm/drawingarea.h> <gtkmm/dropdown.h> <gtkmm/droptarget.h> <gtkmm/entry.h>
        <gtkmm/eventcontrollerfocus.h> <gtkmm/eventcontrollerkey.h> <gtkmm/filedialog.h>
        <gtkmm/flowbox.h> <gtkmm/flowboxchild.h> <gtkmm/gestureclick.h> <gtkmm/gesturedrag.h>
        <gtkmm/grid.h> <gtkmm/image.h> <gtkmm/label.h> <gtkmm/listbox.h> <gtkmm/menubutton.h>
        <gtkmm/popover.h> <gtkmm/scrolledwindow.h> <gtkmm/spinbutton.h> <gtkmm/stringlist.h>
        <gtkmm/textview.h> <gtkmm/togglebutton.h> <gtkmm/window.h>)
    # The resource bundle is C: no C++ header for it.
    set_source_files_properties(${COMPOSIX_RESOURCES_C} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    # ccache and a PCH: hits need these relaxed (ccache's own advice).
    if(CMAKE_CXX_COMPILER_LAUNCHER MATCHES "ccache")
        set(CX_CCACHE_ENV ${CMAKE_COMMAND} -E env
            CCACHE_SLOPPINESS=pch_defines,time_macros,include_file_mtime,include_file_ctime)
        foreach(t composix_core composix_layout composix_selftest composix)
            set_target_properties(${t} PROPERTIES CXX_COMPILER_LAUNCHER "${CX_CCACHE_ENV};${CCACHE_PROGRAM}")
        endforeach()
    endif()
endif()
