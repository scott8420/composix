cmake_minimum_required(VERSION 3.20)
project(composix VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTKMM REQUIRED gtkmm-4.0)
find_package(spdlog REQUIRED)

# ── composix_core -- the GTK-free core (the model side of the one real seam) ──────
# No gtkmm on this library's link line, BY DESIGN: a stray UI include under
# core/ fails to compile, so the seam is enforced by the build graph and the
# core stays headless-testable. (docs/two-layer-seam.md.)
add_library(composix_core STATIC
    src/core/Recents.cpp
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
    src/core/DocJson.cpp
    src/core/Project.cpp
)
target_include_directories(composix_core PUBLIC include)
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
)
target_include_directories(composix_layout PUBLIC include ${PANGOCAIRO_INCLUDE_DIRS})
target_link_libraries(composix_layout PUBLIC composix_core plate_pango ${PANGOCAIRO_LIBRARIES})
target_link_directories(composix_layout PUBLIC ${PANGOCAIRO_LIBRARY_DIRS})
target_compile_options(composix_layout PUBLIC ${PANGOCAIRO_CFLAGS_OTHER})

# ── Headless selftest -- exercises the core with no display ────────────────────
add_executable(composix_selftest src/selftest.cpp)
target_link_libraries(composix_selftest PRIVATE composix_core composix_layout)

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
    src/Shell_preflight.cpp
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
target_link_libraries(composix     PRIVATE ${GTKMM_LIBRARIES} composix_core composix_layout spdlog::spdlog)
target_link_directories(composix   PRIVATE ${GTKMM_LIBRARY_DIRS})
target_compile_options(composix    PRIVATE ${GTKMM_CFLAGS_OTHER})
