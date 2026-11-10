// What the C and C++ backends write alike: comments and member lines.
#pragma once

#include "model.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace codegen {

// A comment's text, safe inside /* */. Lines after the first take the usual star.
inline std::string c_comment(const std::string &text) {
    std::string out;
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == '*' && i + 1 < text.size() && text[i + 1] == '/') out += "* ";
        else if (text[i] == '\n') out += "\n * ";
        else out += text[i];
    }
    return "/* " + out + " */";
}

inline std::string about_of(const Def &d) {
    std::string joined;
    for (size_t i = 0; i < d.about.size(); i++) joined += (i ? "\n" : "") + d.about[i];
    return joined.empty() ? "" : c_comment(joined) + "\n";
}

// Member lines with their comments lined up.
inline std::string member_lines(const std::vector<std::pair<std::string, std::string>> &rows) {
    size_t width = 0;
    for (const auto &r : rows)
        if (!r.second.empty()) width = std::max(width, r.first.size());
    std::string out;
    for (const auto &r : rows) {
        out += "    " + r.first;
        if (!r.second.empty()) out += std::string(width - r.first.size() + 2, ' ') + c_comment(r.second);
        out += "\n";
    }
    return out;
}

inline std::string revision_hex(uint32_t revision) {
    char hex[16];
    snprintf(hex, sizeof hex, "0x%08X", revision);
    return hex;
}

}  // namespace codegen
