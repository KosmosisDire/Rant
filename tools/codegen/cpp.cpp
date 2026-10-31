// The C++ backend: the structs of rant_types.hpp and the reflection rant.hpp registers.
#include "backends.hpp"
#include "cstyle.hpp"

#include <map>
#include <set>
#include <stdexcept>

namespace codegen {

// A member name a C++ compiler reserves takes a trailing underscore.
static std::string cpp_name(const std::string &name) {
    static const std::set<std::string> words = {
        "alignas", "alignof", "and", "asm", "auto", "bitand", "bitor", "bool", "break", "case", "catch", "char",
        "class", "compl", "const", "constexpr", "continue", "decltype", "default", "delete", "do", "double",
        "else", "enum", "explicit", "export", "extern", "false", "float", "for", "friend", "goto", "if", "inline",
        "int", "long", "mutable", "namespace", "new", "noexcept", "not", "nullptr", "operator", "or", "private",
        "protected", "public", "register", "return", "short", "signed", "sizeof", "static", "struct", "switch",
        "template", "this", "throw", "true", "try", "typedef", "typeid", "typename", "union", "unsigned", "using",
        "virtual", "void", "volatile", "while", "xor" };
    return words.count(name) ? name + "_" : name;
}

// An enum option X11 defines as a macro takes one too, so a program may include both.
static std::string cpp_option(const std::string &name) {
    static const std::set<std::string> macros = { "None", "Bool", "Status", "True", "False", "Success" };
    return macros.count(name) ? name + "_" : name;
}

static std::string cpp_scalar(const std::string &word) {
    static const std::map<std::string, std::string> types = {
        { "u8", "uint8_t" }, { "u16", "uint16_t" }, { "u32", "uint32_t" }, { "u64", "uint64_t" },
        { "i8", "int8_t" }, { "i16", "int16_t" }, { "i32", "int32_t" }, { "i64", "int64_t" },
        { "f32", "float" }, { "f64", "double" }, { "bool", "bool" } };
    return types.at(word);
}

// The type as C++ spells it inside namespace rant::types.
static std::string cpp_type(const Type &t) {
    switch (t.kind) {
    case Type::Scalar: return cpp_scalar(t.word);
    case Type::Named: return t.word;
    case Type::String: return t.cap ? "String<" + std::to_string(t.cap) + ">" : "std::string";
    case Type::Array:
        if (t.count) return "std::array<" + cpp_type(*t.elem) + ", " + std::to_string(t.count) + ">";
        return "std::vector<" + cpp_type(*t.elem) + ">";
    case Type::Map: break;
    }
    throw std::runtime_error("the C++ types hold no map");
}

// The element and dimensions of a fixed array, which a member writes as a C array.
static std::string c_array(const Type &t, std::string &dims) {
    const Type *e = &t;
    for (; e->kind == Type::Array && e->count; e = e->elem.get()) dims += "[" + std::to_string(e->count) + "]";
    return cpp_type(*e);
}

// An enum starts on its 0 option, or its first when it has none.
static std::string default_option(const Def &d) {
    for (const Member &m : d.members)
        if (m.value == 0) return d.name + "::" + cpp_option(m.name);
    return d.name + "::" + cpp_option(d.members[0].name);
}

using Defs = std::map<std::string, const Def *>;

// One member with its zero default, so a value never starts out as noise.
static std::string cpp_member(const Type &t, const std::string &name, const Defs &defs) {
    if (t.kind == Type::Array && t.count) {
        std::string dims;
        const std::string elem = c_array(t, dims);
        return elem + " " + name + dims + " = {};";
    }
    const std::string decl = cpp_type(t) + " " + name;
    if (t.kind == Type::Scalar) return decl + (t.word == "bool" ? " = false;" : " = 0;");
    if (t.kind == Type::Named && defs.at(t.word)->kind == Def::Enum)
        return decl + " = " + default_option(*defs.at(t.word)) + ";";
    return decl + ";";
}

static std::string cpp_def(const Def &d, const Defs &defs) {
    std::vector<std::pair<std::string, std::string>> rows;
    if (d.kind == Def::Enum) {
        for (const Member &m : d.members)
            rows.push_back({ cpp_option(m.name) + " = " + std::to_string(m.value) + ",", comment_of(m) });
        return about_of(d) + "enum class " + d.name + " : " + cpp_scalar(d.backing) + " {\n" + member_lines(rows) + "};\n";
    }
    if (d.members.empty()) return about_of(d) + "struct " + d.name + " {};\n";
    if (d.kind == Def::Alias) {
        rows.push_back({ cpp_member(d.members[0].type, alias_member(d), defs), comment_of(d.members[0]) });
    } else {
        for (const Member &m : d.members)
            rows.push_back({ cpp_member(m.type, cpp_name(m.name), defs), comment_of(m) });
    }
    return about_of(d) + "struct " + d.name + " {\n" + member_lines(rows) + "};\n";
}

static std::string types_header(const Model &model, const std::string &extras) {
    static const std::string MARKER = "/* @TYPES@ */\n";
    const size_t at = extras.find(MARKER);
    if (at == std::string::npos) throw std::runtime_error("types/extras.hpp has no marker");

    Defs defs;
    std::string body;
    for (const Def &d : model.defs) {
        defs[d.name] = &d;
        body += cpp_def(d, defs) + "\n";
    }
    body += "/* Each fixed type is its wire bytes, so it copies as is and passes to the C structs. */\n";
    for (const Def &d : model.defs)
        if (d.fixed && d.packed && d.kind != Def::Enum)
            body += "static_assert(sizeof(" + d.name + ") == " + std::to_string(d.packed) + ", \"" + d.name +
                    " must be its wire bytes\");\n";

    const std::string rev = revision_hex(model.revision);
    std::string out = "/* Generated by rant_codegen from types/stdtypes.schema and types/extras.hpp, do not edit. */\n";
    out += "/* The standard types as C++ structs. It stands alone, so a library may use the types without\n"
           " * the middleware: dist/rant_types.hpp is this file, and dist/rant.hpp holds a copy. */\n";
    out += "#if defined(RANT_TYPES_HPP) && RANT_TYPES_HPP != " + rev + "\n";
    out += "#error \"two different versions of Rant's standard types in one program, build all against one Rant\"\n";
    out += "#endif\n#ifndef RANT_TYPES_HPP\n#define RANT_TYPES_HPP " + rev + "\n\n";
    out += extras.substr(0, at) + body + extras.substr(at + MARKER.size());
    return out + "\n#endif /* RANT_TYPES_HPP */\n";
}

// What an alias copies as: rant::String<256>, uint8_t[16], double[3][3].
static std::string alias_repr(const Type &t) {
    if (t.kind == Type::Array && t.count) {
        std::string dims;
        const std::string elem = c_array(t, dims);
        return elem + dims;
    }
    if (t.kind == Type::String && t.cap) return "rant::" + cpp_type(t);
    if (t.kind == Type::Named) return "rant::types::" + t.word;
    return cpp_type(t);
}

static std::string reflect_body(const Model &model) {
    std::string out = "/* Generated by rant_codegen from types/stdtypes.schema, do not edit. */\n";
    out += "/* Each standard type's wire name and members, spliced into rant.hpp by tools/pack.cmake. */\n";
    for (const Def &d : model.defs) {
        if (d.kind == Def::Alias) {
            out += "RANT_STD_ALIAS(" + d.name + ", " + alias_repr(d.members[0].type) + ");\n";
        } else if (d.kind == Def::Enum) {
            out += "RANT_STD_ENUM(" + d.name + ",";
            for (const Member &m : d.members)
                out += "\n    RANT_STD_O(" + d.name + ", " + cpp_option(m.name) + ", \"" + m.name + "\")";
            out += ");\n";
        } else {
            out += "RANT_STD_STRUCT(" + d.name + ",";
            for (const Member &m : d.members)
                out += "\n    RANT_STD_F(" + d.name + ", " + cpp_name(m.name) + ", \"" + m.name + "\")";
            out += ");\n";
        }
    }
    return out;
}

std::vector<Output> write_cpp(const Model &model, const std::string &extras) {
    return { { "bindings/cpp/rant_types.hpp", types_header(model, extras) },
             { "bindings/cpp/rant_reflect.inc", reflect_body(model) } };
}

}  // namespace codegen
