// core/Xml -- see the header.
#include "core/Xml.hpp"

#include <cctype>
#include <cstdint>

namespace composix::core::xml {

const std::string* Node::attr(const std::string& n) const {
    for (const auto& [k, v] : attrs) if (k == n) return &v;
    return nullptr;
}
std::string Node::attr_or(const std::string& n, const std::string& fallback) const {
    const auto* v = attr(n);
    return v ? *v : fallback;
}
const Node* Node::child(const std::string& n) const {
    for (const auto& k : kids) if (k.name == n) return &k;
    return nullptr;
}
std::vector<const Node*> Node::children(const std::string& n) const {
    std::vector<const Node*> out;
    for (const auto& k : kids) if (k.name == n) out.push_back(&k);
    return out;
}
const Node* Node::find(const std::string& n) const {
    for (const auto& k : kids) {
        if (k.name == n) return &k;
        if (const auto* f = k.find(n)) return f;
    }
    return nullptr;
}

namespace {
void utf8(std::string& o, std::uint32_t c) {
    if (c < 0x80) o += static_cast<char>(c);
    else if (c < 0x800) { o += static_cast<char>(0xC0 | (c >> 6)); o += static_cast<char>(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { o += static_cast<char>(0xE0 | (c >> 12)); o += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); o += static_cast<char>(0x80 | (c & 0x3F)); }
    else { o += static_cast<char>(0xF0 | (c >> 18)); o += static_cast<char>(0x80 | ((c >> 12) & 0x3F)); o += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); o += static_cast<char>(0x80 | (c & 0x3F)); }
}

std::string decode(const std::string& s, std::size_t a, std::size_t b) {
    std::string o;
    o.reserve(b - a);
    for (std::size_t i = a; i < b; ++i) {
        if (s[i] != '&') { o += s[i]; continue; }
        const auto e = s.find(';', i);
        if (e == std::string::npos || e > b || e - i > 12) { o += '&'; continue; }
        const std::string ent = s.substr(i + 1, e - i - 1);
        if (ent == "lt") o += '<';
        else if (ent == "gt") o += '>';
        else if (ent == "amp") o += '&';
        else if (ent == "quot") o += '"';
        else if (ent == "apos") o += '\'';
        else if (!ent.empty() && ent[0] == '#') {
            std::uint32_t c = 0;
            try { c = static_cast<std::uint32_t>(ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X') ? std::stoul(ent.substr(2), nullptr, 16) : std::stoul(ent.substr(1))); }
            catch (...) { o += '&'; continue; }
            if (c > 0x10FFFF) c = 0xFFFD;
            utf8(o, c);
        } else { o += '&'; continue; }
        i = e;
    }
    return o;
}

struct Parser {
    const std::string& s;
    std::size_t i = 0;
    std::string err;
    bool fail(const std::string& why) { if (err.empty()) err = why + " at byte " + std::to_string(i); return false; }
    static bool name_char(char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == ':' || c == '_' || c == '-' || c == '.' || static_cast<unsigned char>(c) >= 0x80;
    }
    void ws() { while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i; }
    // Skip what isn't content: <?..?>, <!-- -->, <!DOCTYPE ..>. True if it skipped.
    bool skip_misc() {
        if (s.compare(i, 2, "<?") == 0) { const auto e = s.find("?>", i); i = e == std::string::npos ? s.size() : e + 2; return true; }
        if (s.compare(i, 4, "<!--") == 0) { const auto e = s.find("-->", i); i = e == std::string::npos ? s.size() : e + 3; return true; }
        if (s.compare(i, 9, "<!DOCTYPE") == 0) {
            int depth = 0;
            for (; i < s.size(); ++i) {
                if (s[i] == '[') ++depth;
                else if (s[i] == ']') --depth;
                else if (s[i] == '>' && depth <= 0) { ++i; break; }
            }
            return true;
        }
        return false;
    }
    bool element(Node& n, int depth) {
        if (depth > 1000) return fail("nested too deep");
        ++i;   // '<'
        const auto a = i;
        while (i < s.size() && name_char(s[i])) ++i;
        n.name = s.substr(a, i - a);
        if (n.name.empty()) return fail("an element without a name");
        for (;;) {   // attributes
            ws();
            if (i >= s.size()) return fail("unfinished tag");
            if (s[i] == '/') {
                if (s.compare(i, 2, "/>") != 0) return fail("bad empty tag");
                i += 2;
                return true;
            }
            if (s[i] == '>') { ++i; break; }
            const auto k0 = i;
            while (i < s.size() && name_char(s[i])) ++i;
            const std::string key = s.substr(k0, i - k0);
            if (key.empty()) return fail("bad attribute");
            ws();
            if (i >= s.size() || s[i] != '=') return fail("attribute without a value");
            ++i;
            ws();
            if (i >= s.size() || (s[i] != '"' && s[i] != '\'')) return fail("unquoted attribute");
            const char q = s[i++];
            const auto e = s.find(q, i);
            if (e == std::string::npos) return fail("unfinished attribute");
            n.attrs.emplace_back(key, decode(s, i, e));
            i = e + 1;
        }
        // content
        for (;;) {
            if (i >= s.size()) return fail("<" + n.name + "> not closed");
            if (s[i] == '<') {
                if (s.compare(i, 2, "</") == 0) {
                    const auto e = s.find('>', i);
                    if (e == std::string::npos) return fail("bad end tag");
                    std::string nm = s.substr(i + 2, e - i - 2);
                    while (!nm.empty() && std::isspace(static_cast<unsigned char>(nm.back()))) nm.pop_back();
                    if (nm != n.name) return fail("</" + nm + "> closes <" + n.name + ">");
                    i = e + 1;
                    return true;
                }
                if (s.compare(i, 9, "<![CDATA[") == 0) {
                    const auto e = s.find("]]>", i);
                    if (e == std::string::npos) return fail("unfinished CDATA");
                    Node t;
                    t.text = s.substr(i + 9, e - i - 9);
                    if (!n.kids.empty() && n.kids.back().is_text()) n.kids.back().text += t.text; else n.kids.push_back(std::move(t));
                    i = e + 3;
                    continue;
                }
                if (skip_misc()) continue;
                Node k;
                if (!element(k, depth + 1)) return false;
                n.kids.push_back(std::move(k));
                continue;
            }
            const auto e = s.find('<', i);
            const auto end = e == std::string::npos ? s.size() : e;
            Node t;
            t.text = decode(s, i, end);
            if (!n.kids.empty() && n.kids.back().is_text()) n.kids.back().text += t.text; else n.kids.push_back(std::move(t));
            i = end;
        }
    }
};
}  // namespace

std::optional<Node> parse(const std::string& text, std::string* err) {
    Parser p{text};
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) p.i = 3;   // a BOM
    for (;;) {
        p.ws();
        if (p.i >= text.size()) { if (err) *err = "no element"; return std::nullopt; }
        if (text[p.i] != '<') { if (err) *err = "text before the first element"; return std::nullopt; }
        if (p.skip_misc()) continue;
        break;
    }
    Node root;
    if (!p.element(root, 0)) {
        if (err) *err = p.err;
        return std::nullopt;
    }
    return root;
}

}  // namespace composix::core::xml
