#include "internal.hpp"

#include <zlib.h>

#include <cmath>
#include <cctype>
#include <cstdio>
#include <ctime>

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

std::string Objects::write(int root, int info, const std::string& version, const std::string& trailer) const {
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
           " 0 R /Info " + std::to_string(info) + " 0 R" + (trailer.empty() ? "" : " " + trailer) + " >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    return out;
}

namespace {
std::tm local(long long t, long* off) {
    const std::time_t tt = static_cast<std::time_t>(t);
    std::tm tm{};
    localtime_r(&tt, &tm);
    *off = tm.tm_gmtoff;
    return tm;
}
std::string xml_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '&') o += "&amp;";
        else if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else o += c;
    }
    return o;
}
}  // namespace

std::string pdf_date(long long t) {
    long off = 0;
    const std::tm tm = local(t, &off);
    char buf[64];
    const long a = off < 0 ? -off : off;
    std::snprintf(buf, sizeof buf, "D:%04d%02d%02d%02d%02d%02d%c%02ld'%02ld'", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, off < 0 ? '-' : '+', a / 3600, (a % 3600) / 60);
    return buf;
}

std::string xmp_date(long long t) {
    long off = 0;
    const std::tm tm = local(t, &off);
    char buf[64];
    const long a = off < 0 ? -off : off;
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d%c%02ld:%02ld", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, off < 0 ? '-' : '+', a / 3600, (a % 3600) / 60);
    return buf;
}

// Two FNV-1a 64-bit hashes of the seed (different offsets): 128 bits, hex.
std::string doc_id(const std::string& seed) {
    std::uint64_t a = 1469598103934665603ULL, b = 0x84222325cbf29ce4ULL;
    for (unsigned char c : seed) {
        a = (a ^ c) * 1099511628211ULL;
        b = (b ^ c) * 1099511628211ULL;
        b ^= b >> 29;
    }
    char buf[40];
    std::snprintf(buf, sizeof buf, "%016llX%016llX", static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
    return buf;
}

std::string xmp(Standard s, const std::string& title, const std::string& creator, const std::string& date,
                const std::string& id) {
    std::string uuid = id.substr(0, 8) + "-" + id.substr(8, 4) + "-" + id.substr(12, 4) + "-" + id.substr(16, 4) + "-" + id.substr(20, 12);
    for (auto& c : uuid) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string x =
        "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
        "<rdf:Description rdf:about=\"\"\n"
        " xmlns:dc=\"http://purl.org/dc/elements/1.1/\"\n"
        " xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\"\n"
        " xmlns:pdf=\"http://ns.adobe.com/pdf/1.3/\"\n"
        " xmlns:xmpMM=\"http://ns.adobe.com/xap/1.0/mm/\"\n"
        " xmlns:pdfx=\"http://ns.adobe.com/pdfx/1.3/\"\n"
        " xmlns:pdfxid=\"http://www.npes.org/pdfx/ns/id/\">\n"
        "<dc:format>application/pdf</dc:format>\n"
        "<dc:title><rdf:Alt><rdf:li xml:lang=\"x-default\">" + xml_escape(title) + "</rdf:li></rdf:Alt></dc:title>\n"
        "<xmp:CreateDate>" + date + "</xmp:CreateDate>\n<xmp:ModifyDate>" + date + "</xmp:ModifyDate>\n"
        "<xmp:MetadataDate>" + date + "</xmp:MetadataDate>\n<xmp:CreatorTool>" + xml_escape(creator) + "</xmp:CreatorTool>\n"
        "<pdf:Producer>Plate</pdf:Producer>\n<pdf:Trapped>False</pdf:Trapped>\n"
        "<xmpMM:DocumentID>uuid:" + uuid + "</xmpMM:DocumentID>\n<xmpMM:InstanceID>uuid:" + uuid + "</xmpMM:InstanceID>\n"
        "<xmpMM:VersionID>1</xmpMM:VersionID>\n<xmpMM:RenditionClass>default</xmpMM:RenditionClass>\n";
    if (s == Standard::X4) x += "<pdfxid:GTS_PDFXVersion>PDF/X-4</pdfxid:GTS_PDFXVersion>\n";
    if (s == Standard::X1a_2003) x += "<pdfx:GTS_PDFXVersion>PDF/X-1:2003</pdfx:GTS_PDFXVersion>\n"
                                      "<pdfx:GTS_PDFXConformance>PDF/X-1a:2003</pdfx:GTS_PDFXConformance>\n";
    x += "</rdf:Description>\n</rdf:RDF>\n</x:xmpmeta>\n";
    x += std::string(2000, ' ') + "\n<?xpacket end=\"w\"?>";   // room to edit in place
    return x;
}

}  // namespace plate::detail
