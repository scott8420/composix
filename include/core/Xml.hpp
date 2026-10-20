#pragma once
// core/Xml -- a small XML reader into a tree (s235, for ODT import). Mixed
// content kept: an element's children are elements and text runs, in
// order. Entities (the five, &#n; &#xh;) and CDATA decoded; comments,
// processing instructions and DOCTYPE skipped. Names as written, prefixes
// and all ("text:p"). Not validating; nesting held to 1000 deep.
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace composix::core::xml {

struct Node {
    std::string name;                                       // "" for a text run
    std::string text;                                       // a text run's characters
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<Node> kids;
    bool is_text() const { return name.empty(); }
    const std::string* attr(const std::string& n) const;    // null when absent
    std::string attr_or(const std::string& n, const std::string& fallback = {}) const;
    const Node* child(const std::string& n) const;          // the first element so named
    std::vector<const Node*> children(const std::string& n) const;
    const Node* find(const std::string& n) const;           // the first so named, anywhere below
};

std::optional<Node> parse(const std::string& text, std::string* err = nullptr);

}  // namespace composix::core::xml
