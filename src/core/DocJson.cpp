// core::Doc <-> document.json, and the project folder (s004).
//
// The project is a FOLDER (REQUIREMENTS §8, as Folio): <name>/document.json
// plus assets/ for embedded files. The JSON is a list of records, each
// {"id", "kind", ...fields}, so a hand-read diff shows what changed and an
// unknown kind from a newer Composix is skipped, not fatal.
#include "core/Doc.hpp"

#include "json.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace composix::core {

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr int kFormat = 1;

template <class E> int en(E e) { return static_cast<int>(e); }
template <class E> E de(const json& j, const char* k, E dflt) {
    return j.contains(k) ? static_cast<E>(j.at(k).get<int>()) : dflt;
}
template <class T> T val(const json& j, const char* k, T dflt) {
    return j.contains(k) ? j.at(k).get<T>() : dflt;
}

json to(const Record& r) {
    return std::visit([](const auto& v) -> json {
        using T = std::decay_t<decltype(v)>;
        json j;
        if constexpr (std::is_same_v<T, Settings>) {
            j = {{"kind", "settings"}, {"title", v.title}, {"trim_w", v.trim_w}, {"trim_h", v.trim_h},
                 {"bleed", v.bleed}, {"facing", v.facing}};
        } else if constexpr (std::is_same_v<T, Section>) {
            j = {{"kind", "section"}, {"name", v.name}, {"start_page", v.start_page},
                 {"numbering", en(v.numbering)}, {"start_at", v.start_at}, {"starts", en(v.starts)},
                 {"master", v.master}};
        } else if constexpr (std::is_same_v<T, Page>) {
            j = {{"kind", "page"}, {"pos", v.pos}, {"w", v.w}, {"h", v.h}, {"folds", v.folds},
                 {"master", v.master}};
        } else if constexpr (std::is_same_v<T, Master>) {
            j = {{"kind", "master"}, {"prefix", v.prefix}, {"name", v.name}, {"pos", v.pos}, {"w", v.w},
                 {"h", v.h}, {"folds", v.folds}, {"columns", v.columns}, {"based_on", v.based_on},
                 {"margins", {v.margins.top, v.margins.bottom, v.margins.inside, v.margins.outside}},
                 {"gutter", v.gutter}};
        } else if constexpr (std::is_same_v<T, Frame>) {
            j = {{"kind", "frame"}, {"owner", v.owner}, {"type", en(v.kind)}, {"x", v.x}, {"y", v.y},
                 {"w", v.w}, {"h", v.h}, {"rot", v.rot}, {"z", v.z}, {"story", v.story},
                 {"next", v.next}, {"asset", v.asset}, {"fit", en(v.fit)},
                 {"columns", v.columns}, {"gutter", v.gutter}, {"balance", v.balance}, {"widths", v.widths}};
        } else if constexpr (std::is_same_v<T, Story>) {
            json paras = json::array();
            for (const auto& p : v.paras) {
                json jp = {{"style", p.style}, {"text", p.text}};
                if (!p.runs.empty()) {
                    json runs = json::array();
                    for (const auto& r : p.runs) runs.push_back({{"start", r.start}, {"end", r.end}, {"style", r.style}});
                    jp["runs"] = runs;
                }
                paras.push_back(jp);
            }
            j = {{"kind", "story"}, {"name", v.name}, {"paras", paras}};
        } else if constexpr (std::is_same_v<T, Style>) {
            j = {{"kind", "style"}, {"name", v.name}, {"type", en(v.kind)}, {"pos", v.pos},
                 {"family", v.family}, {"size", v.size}, {"leading", v.leading}, {"bold", v.bold},
                 {"italic", v.italic}, {"align", en(v.align)}, {"space_before", v.space_before},
                 {"space_after", v.space_after}};
        } else if constexpr (std::is_same_v<T, Swatch>) {
            j = {{"kind", "swatch"}, {"name", v.name}, {"pos", v.pos}, {"model", en(v.model)},
                 {"v", v.v}, {"spot", v.spot}, {"preview", v.preview}};
        } else if constexpr (std::is_same_v<T, Asset>) {
            j = {{"kind", "asset"}, {"name", v.name}, {"pos", v.pos}, {"path", v.path},
                 {"embedded", v.embedded}, {"color", v.color}, {"px_w", v.px_w}, {"px_h", v.px_h},
                 {"source", v.source}, {"stamp", v.stamp}};
        } else if constexpr (std::is_same_v<T, Guide>) {
            j = {{"kind", "guide"}, {"owner", v.owner}, {"vertical", v.vertical}, {"pos", v.pos}};
        } else if constexpr (std::is_same_v<T, GenList>) {
            j = {{"kind", "list"}, {"name", v.name}, {"type", en(v.kind)}, {"pos", v.pos},
                 {"from_styles", v.from_styles}, {"placed_on", v.placed_on},
                 {"built_entries", v.built_entries}};
        }
        return j;
    }, r);
}

std::optional<Record> from(const json& j) {
    const std::string k = val<std::string>(j, "kind", "");
    if (k == "settings") {
        Settings v;
        v.title = val(j, "title", v.title);
        v.trim_w = val(j, "trim_w", v.trim_w);
        v.trim_h = val(j, "trim_h", v.trim_h);
        v.bleed = val(j, "bleed", v.bleed);
        v.facing = val(j, "facing", v.facing);
        return v;
    }
    if (k == "section") {
        Section v;
        v.name = val<std::string>(j, "name", "");
        v.start_page = val<std::string>(j, "start_page", "");
        v.numbering = de(j, "numbering", Numbering::Arabic);
        v.start_at = val(j, "start_at", 1);
        v.starts = de(j, "starts", Side::Any);
        v.master = val<std::string>(j, "master", "");
        return v;
    }
    if (k == "page") {
        Page v;
        v.pos = val(j, "pos", 0.0);
        v.w = val(j, "w", v.w);
        v.h = val(j, "h", v.h);
        v.folds = val(j, "folds", std::vector<double>{});
        v.master = val<std::string>(j, "master", "");
        return v;
    }
    if (k == "master") {
        Master v;
        v.prefix = val<std::string>(j, "prefix", "");
        v.name = val<std::string>(j, "name", "");
        v.pos = val(j, "pos", 0.0);
        v.w = val(j, "w", v.w);
        v.h = val(j, "h", v.h);
        v.folds = val(j, "folds", std::vector<double>{});
        v.columns = val(j, "columns", 1);
        v.based_on = val<std::string>(j, "based_on", "");
        if (j.contains("margins") && j.at("margins").is_array() && j.at("margins").size() == 4) {
            const auto& m = j.at("margins");
            v.margins = {m[0].get<double>(), m[1].get<double>(), m[2].get<double>(), m[3].get<double>()};
        }
        v.gutter = val(j, "gutter", 12.0);
        return v;
    }
    if (k == "frame") {
        Frame v;
        v.owner = val<std::string>(j, "owner", "");
        v.kind = de(j, "type", FrameKind::Text);
        v.x = val(j, "x", 0.0); v.y = val(j, "y", 0.0);
        v.w = val(j, "w", 0.0); v.h = val(j, "h", 0.0);
        v.rot = val(j, "rot", 0.0); v.z = val(j, "z", 0.0);
        v.story = val<std::string>(j, "story", "");
        v.next = val<std::string>(j, "next", "");
        v.asset = val<std::string>(j, "asset", "");
        v.fit = de(j, "fit", ImageFit::Fill);
        v.columns = val(j, "columns", 1);
        v.gutter = val(j, "gutter", 12.0);
        v.balance = val(j, "balance", false);
        v.widths = val(j, "widths", std::vector<double>{});
        return v;
    }
    if (k == "story") {
        Story v;
        v.name = val<std::string>(j, "name", "");
        if (j.contains("paras"))
            for (const auto& p : j.at("paras"))
            {
                Para para{val<std::string>(p, "style", ""), val<std::string>(p, "text", ""), {}};
                if (p.contains("runs"))
                    for (const auto& r : p.at("runs"))
                        para.runs.push_back({val(r, "start", 0), val(r, "end", 0), val<std::string>(r, "style", "")});
                v.paras.push_back(std::move(para));
            }
        return v;
    }
    if (k == "style") {
        Style v;
        v.name = val<std::string>(j, "name", "");
        v.kind = de(j, "type", StyleKind::Paragraph);
        v.pos = val(j, "pos", 0.0);
        v.family = val(j, "family", v.family);
        v.size = val(j, "size", v.size);
        v.leading = val(j, "leading", v.leading);
        v.bold = val(j, "bold", false);
        v.italic = val(j, "italic", false);
        v.align = de(j, "align", Align::Left);
        v.space_before = val(j, "space_before", 0.0);
        v.space_after = val(j, "space_after", 0.0);
        return v;
    }
    if (k == "swatch") {
        Swatch v;
        v.name = val<std::string>(j, "name", "");
        v.pos = val(j, "pos", 0.0);
        v.model = de(j, "model", ColorModel::CMYK);
        v.v = val(j, "v", std::array<double, 4>{});
        v.spot = val<std::string>(j, "spot", "");
        v.preview = val(j, "preview", v.preview);
        return v;
    }
    if (k == "asset") {
        Asset v;
        v.name = val<std::string>(j, "name", "");
        v.pos = val(j, "pos", 0.0);
        v.path = val<std::string>(j, "path", "");
        v.embedded = val(j, "embedded", false);
        v.color = val(j, "color", v.color);
        v.px_w = val(j, "px_w", 0);
        v.px_h = val(j, "px_h", 0);
        v.source = val<std::string>(j, "source", "");
        v.stamp = val(j, "stamp", std::int64_t{0});
        return v;
    }
    if (k == "guide") {
        Guide v;
        v.owner = val<std::string>(j, "owner", "");
        v.vertical = val(j, "vertical", true);
        v.pos = val(j, "pos", 0.0);
        return v;
    }
    if (k == "list") {
        GenList v;
        v.name = val<std::string>(j, "name", "");
        v.kind = de(j, "type", ListKind::Contents);
        v.pos = val(j, "pos", 0.0);
        v.from_styles = val(j, "from_styles", std::vector<std::string>{});
        v.placed_on = val<std::string>(j, "placed_on", "");
        v.built_entries = val(j, "built_entries", -1);
        return v;
    }
    return std::nullopt;   // a kind this Composix doesn't know: skipped
}

}  // namespace

std::string to_json(const Doc& d) {
    json recs = json::array();
    for (const auto& [id, r] : d.records()) {   // std::map: stable id order, clean diffs
        json j = to(r);
        j["id"] = id;
        recs.push_back(std::move(j));
    }
    json root = {{"composix", kFormat}, {"records", recs}};
    return root.dump(2);
}

bool from_json(Doc& d, const std::string& text, std::string* err) {
    json root;
    try {
        root = json::parse(text);
    } catch (const std::exception& e) {
        if (err) *err = std::string("not JSON: ") + e.what();
        return false;
    }
    if (!root.is_object() || !root.contains("records") || !root.at("records").is_array()) {
        if (err) *err = "not a Composix document (no records)";
        return false;
    }
    if (val(root, "composix", 0) > kFormat) {
        if (err) *err = "written by a newer Composix (format " + std::to_string(val(root, "composix", 0)) + ")";
        return false;
    }
    try {
        d.reset();
        Doc::Loading load(d);
        for (const auto& j : root.at("records")) {
            const std::string id = val<std::string>(j, "id", "");
            if (id.empty()) continue;
            if (auto r = from(j)) d.put(id, *r);
        }
    } catch (const std::exception& e) {
        if (err) *err = std::string("bad record: ") + e.what();
        return false;
    }
    return true;
}

bool save_project(const Doc& d, const std::string& dir, std::string* err, const std::string& from_dir) {
    std::error_code ec;
    fs::create_directories(fs::path(dir) / "assets", ec);
    if (ec) { if (err) *err = ec.message(); return false; }
    // Save As: the embedded files come along (s009).
    if (!from_dir.empty() && fs::path(from_dir) != fs::path(dir)) {
        for (const auto& aid : d.ids_of<Asset>()) {
            const auto* a = d.as<Asset>(aid);
            if (!a->embedded || fs::path(a->path).is_absolute()) continue;
            const fs::path src = fs::path(from_dir) / a->path, dst = fs::path(dir) / a->path;
            if (!fs::exists(src, ec) || fs::exists(dst, ec)) continue;
            fs::create_directories(dst.parent_path(), ec);
            fs::copy_file(src, dst, ec);
            if (ec) { if (err) *err = "copying " + a->name + ": " + ec.message(); return false; }
        }
    }
    // Write beside, then rename: a crash mid-save never leaves half a document.
    const fs::path out = fs::path(dir) / "document.json";
    const fs::path tmp = fs::path(dir) / "document.json.tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { if (err) *err = "cannot write " + tmp.string(); return false; }
        f << to_json(d) << '\n';
        if (!f) { if (err) *err = "write failed: " + tmp.string(); return false; }
    }
    fs::rename(tmp, out, ec);
    if (ec) { if (err) *err = ec.message(); return false; }
    return true;
}

bool load_project(Doc& d, const std::string& dir, std::string* err) {
    const fs::path in = fs::path(dir) / "document.json";
    std::ifstream f(in, std::ios::binary);
    if (!f) { if (err) *err = "no document.json in " + dir; return false; }
    std::stringstream ss;
    ss << f.rdbuf();
    return from_json(d, ss.str(), err);
}

}  // namespace composix::core
