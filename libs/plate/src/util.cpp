#include "internal.hpp"

#include <zlib.h>

#include <cmath>
#include <cstdio>

namespace plate::detail {

std::string num(double v) {
    if (std::abs(v) < 0.00005) return "0";
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.4f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

namespace {
// UTF-8 -> code points (lenient: a bad byte becomes U+FFFD).
std::vector<std::uint32_t> decode(const std::string& s) {
    std::vector<std::uint32_t> out;
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (n == 0 || i + n > s.size()) { out.push_back(0xFFFD); ++i; continue; }
        std::uint32_t cp = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
        for (int k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        out.push_back(cp);
        i += static_cast<std::size_t>(n);
    }
    return out;
}
void hex16(std::string& out, std::uint32_t u) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "%04X", u & 0xFFFF);
    out += buf;
}
}  // namespace

std::string hex_utf16be(const std::string& utf8) {
    std::string out;
    for (std::uint32_t cp : decode(utf8)) {
        if (cp >= 0x10000) {
            cp -= 0x10000;
            hex16(out, 0xD800 + (cp >> 10));
            hex16(out, 0xDC00 + (cp & 0x3FF));
        } else {
            hex16(out, cp);
        }
    }
    return out;
}

std::string text_string(const std::string& utf8) {
    bool ascii = true;
    for (unsigned char c : utf8) ascii = ascii && c < 0x80;
    if (!ascii) return "<FEFF" + hex_utf16be(utf8) + ">";
    std::string out = "(";
    for (char c : utf8) {
        if (c == '(' || c == ')' || c == '\\') out += '\\';
        out += c;
    }
    return out + ")";
}

std::string deflate(const std::string& data) {
    uLongf n = compressBound(static_cast<uLong>(data.size()));
    std::string out(n, '\0');
    if (compress2(reinterpret_cast<Bytef*>(out.data()), &n, reinterpret_cast<const Bytef*>(data.data()),
                  static_cast<uLong>(data.size()), 6) != Z_OK)
        return {};
    out.resize(n);
    return out;
}

int Objects::stream(const std::string& dict, const std::string& data, bool compress) {
    std::string body = data;
    std::string d = dict;
    if (compress && !data.empty()) {
        body = deflate(data);
        d += " /Filter /FlateDecode";
    }
    return add("<< " + d + " /Length " + std::to_string(body.size()) + " >>\nstream\n" + body + "\nendstream");
}

std::string Objects::write(int root, int info, const std::string& version) const {
    std::string out = "%PDF-" + version + "\n%\xE2\xE3\xCF\xD3\n";
    std::vector<std::size_t> offs;
    for (std::size_t i = 0; i < m_body.size(); ++i) {
        offs.push_back(out.size());
        out += std::to_string(i + 1) + " 0 obj\n" + m_body[i] + "\nendobj\n";
    }
    const std::size_t xref = out.size();
    out += "xref\n0 " + std::to_string(m_body.size() + 1) + "\n0000000000 65535 f \n";
    char buf[32];
    for (auto o : offs) {
        std::snprintf(buf, sizeof buf, "%010zu 00000 n \n", o);
        out += buf;
    }
    out += "trailer\n<< /Size " + std::to_string(m_body.size() + 1) + " /Root " + std::to_string(root) +
           " 0 R /Info " + std::to_string(info) + " 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    return out;
}

}  // namespace plate::detail
