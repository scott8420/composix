// core/Zip -- see the header.
#include "core/Zip.hpp"

#include <zlib.h>

#include <cstdint>

namespace composix::core::zip {
namespace {
unsigned u16(const std::string& z, std::size_t i) {
    return static_cast<unsigned>(static_cast<unsigned char>(z[i])) | static_cast<unsigned>(static_cast<unsigned char>(z[i + 1])) << 8;
}
std::uint32_t u32(const std::string& z, std::size_t i) { return static_cast<std::uint32_t>(u16(z, i)) | static_cast<std::uint32_t>(u16(z, i + 2)) << 16; }

struct Central { std::string name; unsigned method = 0; std::uint32_t csize = 0, usize = 0, local = 0; };

// The central directory, from the end record back (a comment may follow it).
std::vector<Central> directory(const std::string& z) {
    std::vector<Central> out;
    if (z.size() < 22) return out;
    std::size_t eocd = std::string::npos;
    for (std::size_t i = z.size() - 22 + 1; i-- > (z.size() > 22 + 65535 ? z.size() - 22 - 65535 : 0);)
        if (z.compare(i, 4, "PK\x05\x06") == 0) { eocd = i; break; }
    if (eocd == std::string::npos) return out;
    const unsigned count = u16(z, eocd + 10);
    std::size_t at = u32(z, eocd + 16);
    for (unsigned k = 0; k < count && at + 46 <= z.size() && z.compare(at, 4, "PK\x01\x02") == 0; ++k) {
        Central c;
        c.method = u16(z, at + 10);
        c.csize = u32(z, at + 20);
        c.usize = u32(z, at + 24);
        const unsigned nlen = u16(z, at + 28), xlen = u16(z, at + 30), clen = u16(z, at + 32);
        c.local = u32(z, at + 42);
        if (at + 46 + nlen > z.size()) break;
        c.name = z.substr(at + 46, nlen);
        out.push_back(std::move(c));
        at += 46 + nlen + xlen + clen;
    }
    return out;
}

std::string inflate_raw(const char* p, std::size_t n, std::size_t hint, bool& ok) {
    ok = false;
    std::string out(hint ? hint : n * 4 + 1024, '\0');
    z_stream zs{};
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return {};
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(p));
    zs.avail_in = static_cast<uInt>(n);
    int rc = Z_OK;
    std::size_t done = 0;
    while (rc == Z_OK) {
        if (done == out.size()) out.resize(out.size() * 2);
        zs.next_out = reinterpret_cast<Bytef*>(out.data() + done);
        zs.avail_out = static_cast<uInt>(out.size() - done);
        rc = inflate(&zs, Z_NO_FLUSH);
        done = out.size() - zs.avail_out;
        if (rc == Z_BUF_ERROR && zs.avail_in == 0) break;
    }
    inflateEnd(&zs);
    if (rc != Z_STREAM_END) return {};
    out.resize(done);
    ok = true;
    return out;
}
}  // namespace

std::vector<std::string> names(const std::string& zip) {
    std::vector<std::string> out;
    for (const auto& c : directory(zip)) out.push_back(c.name);
    return out;
}

std::string entry(const std::string& z, const std::string& name, bool* ok) {
    if (ok) *ok = false;
    for (const auto& c : directory(z)) {
        if (c.name != name) continue;
        if (c.local + 30 > z.size() || z.compare(c.local, 4, "PK\x03\x04") != 0) return {};
        const std::size_t data = c.local + 30 + u16(z, c.local + 26) + u16(z, c.local + 28);
        if (data + c.csize > z.size()) return {};
        if (c.method == 0) {
            if (ok) *ok = true;
            return z.substr(data, c.csize);
        }
        if (c.method != 8) return {};
        bool good = false;
        auto out = inflate_raw(z.data() + data, c.csize, c.usize, good);
        if (ok) *ok = good;
        return out;
    }
    return {};
}

std::string make_stored(const std::vector<std::pair<std::string, std::string>>& files) {
    std::string z, cd;
    const auto p16 = [](std::string& s, unsigned v) { s += static_cast<char>(v & 0xff); s += static_cast<char>((v >> 8) & 0xff); };
    const auto p32 = [&](std::string& s, std::uint32_t v) { p16(s, v & 0xffff); p16(s, v >> 16); };
    for (const auto& [name, data] : files) {
        const auto crc = static_cast<std::uint32_t>(crc32(0, reinterpret_cast<const Bytef*>(data.data()), static_cast<uInt>(data.size())));
        const auto local = static_cast<std::uint32_t>(z.size());
        z += "PK\x03\x04";
        p16(z, 20); p16(z, 0); p16(z, 0); p16(z, 0); p16(z, 0);
        p32(z, crc); p32(z, static_cast<std::uint32_t>(data.size())); p32(z, static_cast<std::uint32_t>(data.size()));
        p16(z, static_cast<unsigned>(name.size())); p16(z, 0);
        z += name;
        z += data;
        cd += "PK\x01\x02";
        p16(cd, 20); p16(cd, 20); p16(cd, 0); p16(cd, 0); p16(cd, 0); p16(cd, 0);
        p32(cd, crc); p32(cd, static_cast<std::uint32_t>(data.size())); p32(cd, static_cast<std::uint32_t>(data.size()));
        p16(cd, static_cast<unsigned>(name.size())); p16(cd, 0); p16(cd, 0); p16(cd, 0); p16(cd, 0); p32(cd, 0); p32(cd, local);
        cd += name;
    }
    const auto at = static_cast<std::uint32_t>(z.size());
    z += cd;
    z += "PK\x05\x06";
    p16(z, 0); p16(z, 0); p16(z, static_cast<unsigned>(files.size())); p16(z, static_cast<unsigned>(files.size()));
    p32(z, static_cast<std::uint32_t>(cd.size())); p32(z, at); p16(z, 0);
    return z;
}

}  // namespace composix::core::zip
