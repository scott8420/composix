#pragma once
// core::PlaceQueue -- s063: the pictures a loaded cursor carries. Place
// (several picked at once) loads them; the first is the one a click puts
// down; after each placement the next comes up; the arrow keys turn
// through them; Esc drops the lot. GTK-free (the canvas holds one).
#include "core/Doc.hpp"

#include <cstddef>
#include <vector>

namespace composix::core {

class PlaceQueue {
public:
    void load(std::vector<ImageFile> files) { m_files = std::move(files); }
    void clear() { m_files.clear(); }
    bool empty() const { return m_files.empty(); }
    std::size_t size() const { return m_files.size(); }
    // The one a click puts down (empty() first).
    const ImageFile& current() const { return m_files.front(); }
    // After the current one is placed: it goes, the next comes up.
    void placed() { if (!m_files.empty()) m_files.erase(m_files.begin()); }
    // Turn through them: +1 the next becomes current (the current to the
    // back), -1 the last comes to the front.
    void cycle(int dir) {
        if (m_files.size() < 2) return;
        if (dir > 0) { auto f = m_files.front(); m_files.erase(m_files.begin()); m_files.push_back(f); }
        else { auto f = m_files.back(); m_files.pop_back(); m_files.insert(m_files.begin(), f); }
    }
    // The one after the current (for the cursor's preview), or null.
    const ImageFile* next() const { return m_files.size() > 1 ? &m_files[1] : nullptr; }

private:
    std::vector<ImageFile> m_files;
};

}  // namespace composix::core
