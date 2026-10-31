#include "model.hpp"

#include "rant.h"

#include <algorithm>
#include <stdexcept>

extern "C" RantSchemaDoc *i_rant_schema_doc_parse(RantAllocator *alloc, const char *text, int standalone,
                                                  RantSchemaError *err);

namespace codegen {

static std::string str(RantString s) { return s.len ? std::string(s.data, s.len) : std::string(); }

static const char *scalar_word(uint8_t kind) {
    switch (kind) {
    case RANT_U8: return "u8";
    case RANT_U16: return "u16";
    case RANT_U32: return "u32";
    case RANT_U64: return "u64";
    case RANT_I8: return "i8";
    case RANT_I16: return "i16";
    case RANT_I32: return "i32";
    case RANT_I64: return "i64";
    case RANT_F32: return "f32";
    case RANT_F64: return "f64";
    case RANT_BOOL: return "bool";
    default: throw std::runtime_error("a kind that is not a scalar");
    }
}

static RantSchemaFieldInfo row(const RantSchema *s, uint16_t i) {
    RantSchemaFieldInfo f;
    if (!rant_schema_field_at(s, i, &f)) throw std::runtime_error("a flat table row is missing");
    return f;
}

// The type at row i, leaving i past all its rows. A root row's name is the definition's own.
static Type type_at(const RantSchema *s, uint16_t &i, bool root) {
    const uint16_t n = rant_schema_field_count(s);
    const RantSchemaFieldInfo f = row(s, i++);
    Type t;
    if (f.type_name.len && !root) {
        t.kind = Type::Named;
        t.word = str(f.type_name);
        while (i < n && row(s, i).depth > f.depth) i++;
        return t;
    }
    switch (f.kind) {
    case RANT_STR: t.kind = Type::String; t.cap = f.str_cap; break;
    case RANT_VSTR: t.kind = Type::String; break;
    case RANT_MAP: t.kind = Type::Map; break;
    case RANT_ARR: case RANT_VARR:
        t.kind = Type::Array;
        t.count = f.kind == RANT_ARR ? f.count : 0;
        t.elem = std::make_shared<Type>(type_at(s, i, false));
        break;
    case RANT_STRUCT: case RANT_ENUM:
        throw std::runtime_error("a struct or an enum with no name");
    default:
        t.word = scalar_word(f.kind);
        break;
    }
    return t;
}

static std::vector<std::string> lines_of(RantString s) {
    std::vector<std::string> out;
    std::string line;
    for (size_t i = 0; i < s.len; i++) {
        if (s.data[i] == '\n') { out.push_back(line); line.clear(); }
        else line += s.data[i];
    }
    if (s.len) out.push_back(line);
    return out;
}

static void take_note(RantSchemaDoc *doc, uint32_t def, uint16_t member, Member &m) {
    const RantSchemaNote note = rant_schema_doc_note(doc, def, member);
    m.unit = str(note.unit);
    m.note = str(note.text);
}

static Def read_def(RantSchemaDoc *doc, uint32_t i) {
    RantSchemaDef info;
    if (!rant_schema_doc_at(doc, i, &info)) throw std::runtime_error("a definition is missing");
    const RantSchema *s = info.schema;
    const uint16_t n = rant_schema_field_count(s);
    Def d;
    d.name = str(rant_schema_name(s));
    d.about = lines_of(info.about);
    d.packed = rant_schema_size(s);
    d.hash = rant_schema_hash(s);
    for (uint16_t f = 0; f < n; f++) {
        const uint8_t kind = row(s, f).kind;
        if (kind == RANT_VSTR || kind == RANT_VARR || kind == RANT_MAP) d.fixed = false;
    }
    if (info.kind == RANT_STRUCT) {
        d.kind = Def::Struct;
        uint16_t member = 0;
        for (uint16_t f = 0; f < n; member++) {
            Member m;
            m.name = str(row(s, f).name);
            m.type = type_at(s, f, false);
            take_note(doc, i, member, m);
            d.members.push_back(m);
        }
    } else if (info.kind == RANT_ENUM) {
        d.kind = Def::Enum;
        d.backing = scalar_word(row(s, 0).backing);
        const uint16_t options = rant_schema_enum_count(s, 0);
        for (uint16_t o = 0; o < options; o++) {
            Member m;
            RantString name;
            if (!rant_schema_enum_variant(s, 0, o, &m.value, &name)) throw std::runtime_error("an option is missing");
            m.name = str(name);
            take_note(doc, i, o, m);
            d.members.push_back(m);
        }
    } else {
        d.kind = Def::Alias;
        uint16_t f = 0;
        Member m;
        m.type = type_at(s, f, true);
        take_note(doc, i, 0, m);
        d.members.push_back(m);
    }
    return d;
}

static uint32_t fnv(uint32_t h, const void *data, size_t len) {
    const uint8_t *p = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < len; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

Model load_model(const std::string &text, bool standalone) {
    RantAllocator alloc = rant_allocator_heap(0);
    RantSchemaError err;
    RantSchemaDoc *doc = i_rant_schema_doc_parse(&alloc, text.c_str(), standalone ? 1 : 0, &err);
    if (!doc)
        throw std::runtime_error(std::to_string(err.line) + ":" + std::to_string(err.column) + ": " +
                                 (err.why ? err.why : "refused"));
    Model model;
    try {
        model.revision = 2166136261u;
        for (uint32_t i = 0; i < rant_schema_doc_count(doc); i++) {
            model.defs.push_back(read_def(doc, i));
            const Def &d = model.defs.back();
            model.revision = fnv(model.revision, d.name.data(), d.name.size());
            model.revision = fnv(model.revision, &d.hash, sizeof d.hash);
        }
    } catch (...) {
        rant_schema_doc_free(doc);
        throw;
    }
    rant_schema_doc_free(doc);
    return model;
}

std::string snake(const std::string &name) {
    char out[256];
    if (!rant_field_name(name.c_str(), out, sizeof out)) throw std::runtime_error("no field name for " + name);
    return out;
}

std::string alias_member(const Def &d) {
    const Member &m = d.members[0];
    if (m.unit == "us") return "us";
    if (m.type.kind == Type::Array) {
        if (m.type.elem->kind == Type::Array) return "m";
        if (m.type.elem->kind == Type::Scalar && m.type.elem->word == "u8") return "bytes";
    }
    return "value";
}

std::string spell(const Type &t) {
    switch (t.kind) {
    case Type::Scalar: case Type::Named: return t.word;
    case Type::String: return t.cap ? "string<" + std::to_string(t.cap) + ">" : "string";
    case Type::Map: return "map";
    case Type::Array: return spell(*t.elem) + "[" + (t.count ? std::to_string(t.count) : "") + "]";
    }
    return "";
}

std::string comment_of(const Member &m) {
    if (m.unit.empty()) return m.note;
    return m.note.empty() ? "[" + m.unit + "]" : "[" + m.unit + "] " + m.note;
}

// One member per line with its comment after it, the comment's earlier lines above it.
static std::string rows_of(const Def &d, const std::vector<std::string> &code) {
    size_t width = 0;
    for (size_t i = 0; i < code.size(); i++)
        if (!comment_of(d.members[i]).empty()) width = std::max(width, code[i].size());
    std::string out;
    for (size_t i = 0; i < code.size(); i++) {
        std::string c = comment_of(d.members[i]);
        for (size_t nl; (nl = c.find('\n')) != std::string::npos; c.erase(0, nl + 1))
            out += "  -- " + c.substr(0, nl) + "\n";
        out += "  " + code[i];
        if (!c.empty()) out += std::string(width - code[i].size() + 2, ' ') + "-- " + c;
        out += "\n";
    }
    return out;
}

std::string body_of(const Def &d) {
    std::vector<std::string> code;
    switch (d.kind) {
    case Def::Alias: {
        const std::string c = comment_of(d.members[0]);
        return spell(d.members[0].type) + (c.empty() ? "" : "  -- " + c);
    }
    case Def::Enum:
        for (const Member &m : d.members) code.push_back(m.name + " = " + std::to_string(m.value));
        return "enum<" + d.backing + "> {\n" + rows_of(d, code) + "}";
    case Def::Struct: {
        bool noted = false;
        for (const Member &m : d.members) {
            code.push_back(m.name + ": " + spell(m.type));
            noted = noted || !comment_of(m).empty();
        }
        if (code.empty()) return "{ }";
        if (noted) return "{\n" + rows_of(d, code) + "}";
        std::string line = "{ ";
        for (size_t i = 0; i < code.size(); i++) line += (i ? ", " : "") + code[i];
        return line + " }";
    }
    }
    return "";
}

std::string definition_of(const Def &d) {
    std::string out;
    for (const std::string &line : d.about) out += "-- " + line + "\n";
    return out + d.name + (d.kind == Def::Struct ? " " : " = ") + body_of(d) + "\n";
}

}  // namespace codegen
