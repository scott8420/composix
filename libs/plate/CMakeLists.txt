# Plate -- a small PDF writer for print. Standalone: build it on its own
# (cmake -S libs/plate -B build-plate) or add_subdirectory() it from an app.
cmake_minimum_required(VERSION 3.20)
project(plate VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PLATE_HB REQUIRED harfbuzz harfbuzz-subset)
find_package(ZLIB REQUIRED)

# plate -- the writer: HarfBuzz (faces + subsetter) and zlib, nothing else.
add_library(plate STATIC src/util.cpp src/document.cpp)
target_include_directories(plate PUBLIC include ${PLATE_HB_INCLUDE_DIRS})
target_link_libraries(plate PUBLIC ${PLATE_HB_LIBRARIES} ZLIB::ZLIB)
target_link_directories(plate PUBLIC ${PLATE_HB_LIBRARY_DIRS})

# plate_pango -- optional: Pango lines as glyph runs.
pkg_check_modules(PLATE_PANGO pango pangocairo)
if(PLATE_PANGO_FOUND)
    add_library(plate_pango STATIC src/pango.cpp)
    target_include_directories(plate_pango PUBLIC include ${PLATE_PANGO_INCLUDE_DIRS})
    target_link_libraries(plate_pango PUBLIC plate ${PLATE_PANGO_LIBRARIES})
    target_link_directories(plate_pango PUBLIC ${PLATE_PANGO_LIBRARY_DIRS})
endif()

# The library's own test (headless): plate_test
if(PLATE_PANGO_FOUND)
    add_executable(plate_test tests/plate_test.cpp)
    target_link_libraries(plate_test PRIVATE plate_pango)
endif()
