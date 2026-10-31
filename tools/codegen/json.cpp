// The JSON backend: the whole model, for a tool that reads no schema text.
#include "backends.hpp"

#include <cstdio>

namespace codegen {

static std::string quoted(const std::string &s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else if (c == '\t') out += "\\t";
        else if (c == '\r') out += "\\r";
        else if ((unsigned char)c < 0x20) { char u[8]; snprintf(u, sizeof u, "\\u%04x", c); out += u; }
        else out += c;
    }
    return out + "\"";
}

static std::string kind_of(const Def &d) {
    return d.kind == Def::Struct ? "struct" : d.kind == Def::Enum ? "enum" : "alias";
}

static std::string member_json(const Def &d, const Member &m) {
    std::string out = "{ ";
    if (d.kind != Def::Alias) out += "\"name\": " + quoted(m.name) + ", ";
    if (d.kind == Def::Enum) out += "\"value\": " + std::to_string(m.value) + ", ";
    else out += "\"type\": " + quoted(spell(m.type)) + ", ";
    return out + "\"unit\": " + quoted(m.unit) + ", \"note\": " + quoted(m.note) + " }";
}

std::vector<Output> write_json(const Model &model) {
    std::string out = "{\n  \"revision\": " + std::to_string(model.revision) + ",\n  \"types\": [\n";
    for (size_t i = 0; i < model.defs.size(); i++) {
        const Def &d = model.defs[i];
        out += "    { \"name\": " + quoted(d.name) + ", \"kind\": " + quoted(kind_of(d));
        if (d.kind == Def::Enum) out += ", \"backing\": " + quoted(d.backing);
        out += ", \"fixed\": " + std::string(d.fixed ? "true" : "false");
        out += ", \"size\": " + std::to_string(d.packed) + ",\n      \"about\": [";
        for (size_t a = 0; a < d.about.size(); a++) out += (a ? ", " : "") + quoted(d.about[a]);
        out += "],\n      \"members\": [";
        for (size_t m = 0; m < d.members.size(); m++)
            out += (m ? ",\n        " : "\n        ") + member_json(d, d.members[m]);
        out += d.members.empty() ? "] }" : "\n      ] }";
        out += i + 1 < model.defs.size() ? ",\n" : "\n";
    }
    return { { "types/stdtypes.json", out + "  ]\n}\n" } };
}

}  // namespace codegen
