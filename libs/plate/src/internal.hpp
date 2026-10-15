#pragma once
// Plate internals: the object table, numbers, strings, compression.
#include "plate/plate.hpp"

#include <map>
#include <string>
#include <vector>

#include <lcms2.h>

namespace plate {
// s016: LittleCMS state -- the press's profile and the transforms to it.
struct Document::Cms {
    cmsHPROFILE   out = nullptr, srgb = nullptr;
    cmsHTRANSFORM img = nullptr, col = nullptr;   // RGB 8-bit -> CMYK 8-bit; RGB double -> CMYK double
    std::string   out_bytes, srgb_bytes, name;
    Intent        intent = Intent::RelativeColorimetric;
    ~Cms();
    bool rebuild(std::string* err);
};
}  // namespace plate

namespace plate::detail {

// A PDF name, with # escapes for anything outside the regular characters.
std::string pdf_name(const std::string& s);

// Numbers as PDF writes them: up to 4 decimals, no trailing zeros, no "-0".
std::string num(double v);
// A PDF literal string, escaped; non-ASCII becomes UTF-16BE hex with a BOM.
std::string text_string(const std::string& utf8);
std::string hex_utf16be(const std::string& utf8);   // for ToUnicode (no BOM)
std::string deflate(const std::string& data);

class Objects {
public:
    int  reserve() { m_body.emplace_back(); return static_cast<int>(m_body.size()); }
    void set(int id, std::string body) { m_body[static_cast<std::size_t>(id - 1)] = std::move(body); }
    int  add(std::string body) { const int id = reserve(); set(id, std::move(body)); return id; }
    // A stream object: `dict` holds the extra entries (no << >>); Flate when asked.
    int  stream(const std::string& dict, const std::string& data, bool compress = true);
    std::string write(int root, int info, const std::string& version) const;
private:
    std::vector<std::string> m_body;
};

}  // namespace plate::detail
