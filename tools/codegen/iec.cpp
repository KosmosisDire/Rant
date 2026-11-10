// The IEC backend: the fixed standard types as the IEC 61131-3 types of the PLCnext library, in
// the C# form the eCLR reads. The library matches each to its schema by name at run time.
#include "backends.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

namespace codegen {

struct IecScalar {
    std::string cs, iec;
    uint32_t size;
};

static const IecScalar &iec_scalar(const std::string &word) {
    static const std::map<std::string, IecScalar> types = {
        { "u8", { "byte", "USINT", 1 } },    { "u16", { "ushort", "UINT", 2 } },
        { "u32", { "uint", "UDINT", 4 } },   { "u64", { "ulong", "ULINT", 8 } },
        { "i8", { "sbyte", "SINT", 1 } },    { "i16", { "short", "INT", 2 } },
        { "i32", { "int", "DINT", 4 } },     { "i64", { "long", "LINT", 8 } },
        { "f32", { "float", "REAL", 4 } },   { "f64", { "double", "LREAL", 8 } },
        { "bool", { "bool", "BOOL", 1 } } };
    return types.at(word);
}

// Words IEC 61131-3 and PLCnext Engineer keep for themselves, which no type or field may take.
static bool reserved(const std::string &name) {
    static const std::set<std::string> words = {
        "ABS", "ACTION", "AND", "ANY", "ARRAY", "AT", "BOOL", "BY", "BYTE", "CASE", "CHAR", "CLASS",
        "CONFIGURATION", "CONSTANT", "DATE", "DINT", "DO", "DT", "DWORD", "ELSE", "ELSIF", "END",
        "EXIT", "FALSE", "FOR", "FUNCTION", "FUNCTION_BLOCK", "IF", "INT", "INTERFACE", "LDT", "LINT",
        "LREAL", "LTIME", "LWORD", "METHOD", "MOD", "NOT", "OF", "ON", "OR", "POINTER", "PROGRAM",
        "REAL", "REF", "REPEAT", "RESOURCE", "RETAIN", "RETURN", "SINT", "STEP", "STRING", "STRUCT",
        "SUPER", "TASK", "THEN", "THIS", "TIME", "TO", "TOD", "TRANSITION", "TRUE", "TYPE", "UDINT",
        "UINT", "ULINT", "UNTIL", "USINT", "VAR", "WHILE", "WITH", "WORD", "WSTRING", "XOR" };
    std::string upper;
    for (char c : name) upper += (char)toupper((unsigned char)c);
    return words.count(upper) != 0;
}

static std::string iec_name(const std::string &name) {
    if (reserved(name)) throw std::runtime_error("the IEC types cannot name " + name + ", a reserved word");
    return name;
}

// A name C# keeps is written @name, which still compiles to the plain name Engineer reads.
static std::string cs_ident(const std::string &name) {
    static const std::set<std::string> words = {
        "abstract", "as", "base", "bool", "break", "byte", "case", "catch", "char", "checked", "class",
        "const", "continue", "decimal", "default", "delegate", "do", "double", "else", "enum", "event",
        "explicit", "extern", "false", "finally", "fixed", "float", "for", "foreach", "goto", "if",
        "implicit", "in", "int", "interface", "internal", "is", "lock", "long", "namespace", "new",
        "null", "object", "operator", "out", "override", "params", "private", "protected", "public",
        "readonly", "ref", "return", "sbyte", "sealed", "short", "sizeof", "stackalloc", "static",
        "string", "struct", "switch", "this", "throw", "true", "try", "typeof", "uint", "ulong",
        "unchecked", "unsafe", "ushort", "using", "virtual", "void", "volatile", "while" };
    return words.count(name) ? "@" + name : name;
}

static std::string pascal_of(const std::string &snake_name) {
    std::string out;
    bool start = true;
    for (char c : snake_name) {
        if (c == '_') { start = true; continue; }
        out += start ? (char)toupper((unsigned char)c) : c;
        start = false;
    }
    return out;
}

// Every IEC type one definition needs, written once each, ahead of the types that use them.
class Writer {
public:
    explicit Writer(const Model &model) {
        for (const Def &d : model.defs) defs_[d.name] = &d;
    }

    std::string source(const Model &model) {
        for (const Def &d : model.defs) {
            if (!d.fixed) continue;
            if (d.kind == Def::Alias) {
                // An alias to an array is a type of its own name, any other names what it holds.
                if (d.members[0].type.kind == Type::Array) array_type(d.name, d.members[0].type, about(d));
                continue;
            }
            named(d.name);
        }
        return out_;
    }

private:
    struct Shape {
        std::string cs, iec;   // the C# type and the IEC type its [DataType] names
        uint32_t size;         // the bytes the eCLR gives it
        bool init;             // holds a string, so needs Init()
        bool scalar = false;   // an elementary type, whose C# type alone is ambiguous
    };

    std::map<std::string, const Def *> defs_;
    std::map<std::string, Shape> done_;
    std::map<std::string, std::string> arrays_;   // each array type's name and the schema text it came from
    std::string out_;

    static std::string about(const Def &d) {
        std::string joined;
        for (size_t i = 0; i < d.about.size(); i++) joined += (i ? " " : "") + d.about[i];
        return joined;
    }

    void emit(const std::string &text) { out_ += (out_.empty() ? "" : "\n") + text; }

    static std::string doc(const std::string &text, const std::string &indent) {
        if (text.empty()) return "";
        std::string safe;
        for (char c : text) {
            if (c == '&') safe += "&amp;";
            else if (c == '<') safe += "&lt;";
            else if (c == '>') safe += "&gt;";
            else safe += c;
        }
        return indent + "/// <summary>" + safe + "</summary>\n";
    }

    Shape string_type(uint16_t cap) {
        const std::string name = "String" + std::to_string(cap);
        if (done_.count(name)) return done_.at(name);
        const std::string size = std::to_string(6 + cap);
        emit("    [String(" + std::to_string(cap) + ")]\n"
             "    [StructLayout(LayoutKind.Explicit, Size = " + size + ")]\n"
             "    public struct " + name + "\n    {\n"
             "        [FieldOffset(0)]\n        public IecStringEx s;\n\n"
             "        public void Init()\n        {\n            s.maximumLength = " + std::to_string(cap) + ";\n        }\n"
             "    }\n");
        return done_[name] = { name, name, (uint32_t)(6 + cap), true };
    }

    Shape array_type(const std::string &name, const Type &t, const std::string &text) {
        const auto seen = arrays_.find(name);
        if (done_.count(name) && (seen == arrays_.end() || seen->second != spell(t)))
            throw std::runtime_error("the IEC name " + name + " is taken by another type, rename a field or type");
        if (done_.count(name)) return done_.at(name);
        arrays_[name] = spell(t);
        std::vector<uint16_t> counts;
        const Type *e = &t;
        for (; e->kind == Type::Array; e = e->elem.get()) {
            if (!e->count) throw std::runtime_error(name + " is no fixed array");
            counts.push_back(e->count);
        }
        const Shape elem = shape(*e);
        if (elem.init) throw std::runtime_error("the IEC types hold no array of strings, " + name);
        uint32_t size = elem.size;
        std::string dims, bounds;
        for (size_t i = 0; i < counts.size(); i++) {
            const std::string at = counts.size() > 1 ? std::to_string(i) : "";
            const std::string upper = std::to_string(counts[i] - 1);
            dims += "    [ArrayDimension(" + std::to_string(i) + ", 0, " + upper + ")]\n";
            bounds += "        public const int LB" + at + " = 0;\n        public const int UB" + at + " = " + upper + ";\n";
            size *= counts[i];
        }
        emit(doc(text, "    ") + "    [Array(" + std::to_string(counts.size()) + ")]\n" + dims +
             "    [DataType(\"" + elem.iec + "\")]\n"
             "    [StructLayout(LayoutKind.Explicit, Size = " + std::to_string(size) + ")]\n"
             "    public struct " + iec_name(name) + "\n    {\n"
             "        [FieldOffset(0)]\n        [DataType(\"" + elem.iec + "\")]\n"
             "        public " + elem.cs + " Anchor;\n" + bounds + "    }\n");
        return done_[name] = { name, name, size, false };
    }

    // The shape of a field's type, writing the types it needs first.
    Shape shape(const Type &t, const std::string &owner = "", const std::string &field = "") {
        switch (t.kind) {
        case Type::Scalar: {
            const IecScalar &s = iec_scalar(t.word);
            return { s.cs, s.iec, s.size, false, true };
        }
        case Type::String:
            if (!t.cap) throw std::runtime_error("the IEC types hold no unbounded string");
            return string_type(t.cap);
        case Type::Array:
            return array_type(owner + pascal_of(field), t, "");
        case Type::Named: {
            const Def &d = *defs_.at(t.word);
            if (d.kind != Def::Alias) return named(d.name);
            const Type &inner = d.members[0].type;
            return inner.kind == Type::Array ? array_type(d.name, inner, about(d)) : shape(inner);
        }
        case Type::Map: break;
        }
        throw std::runtime_error("the IEC types hold no map");
    }

    Shape named(const std::string &name) {
        if (done_.count(name)) return done_.at(name);
        const Def &d = *defs_.at(name);
        if (d.kind == Def::Enum) return enumeration(d);
        std::string fields, inits;
        uint32_t size = 0, align = 1;
        for (const Member &m : d.members) {
            const Shape s = shape(m.type, d.name, m.name);
            const uint32_t a = alignment(m.type);
            size = (size + a - 1) / a * a + s.size;
            align = std::max(align, a);
            std::string note = comment_of(m);
            for (char &c : note)
                if (c == '\n') c = ' ';
            fields += doc(note, "        ") + (s.scalar ? "        [DataType(\"" + s.iec + "\")]\n" : "") +
                      "        public " + s.cs + " " + cs_ident(iec_name(m.name)) + ";\n";
            if (s.init) inits += "            " + cs_ident(m.name) + ".Init();\n";
        }
        // IEC has no empty struct, so one holds a BOOL the library never sends.
        if (d.members.empty()) {
            fields = "        /// <summary>A placeholder, never on the wire</summary>\n"
                     "        [DataType(\"BOOL\")]\n        public bool unused;\n";
            size = align = 1;
        }
        std::string text = doc(about(d), "    ") + "    [Structure]\n    public struct " + iec_name(d.name) +
                           "\n    {\n" + fields;
        if (!inits.empty()) text += "\n        public void Init()\n        {\n" + inits + "        }\n";
        emit(text + "    }\n");
        return done_[name] = { name, name, (size + align - 1) / align * align, !inits.empty() };
    }

    Shape enumeration(const Def &d) {
        const IecScalar &backing = iec_scalar(d.backing);
        std::string text = doc(about(d), "    ") + "    [Enumeration]\n    public enum " + iec_name(d.name) + " : " +
                           backing.cs + "\n    {\n";
        for (const Member &m : d.members) text += "        " + cs_ident(iec_name(m.name)) + " = " + std::to_string(m.value) + ",\n";
        emit(text + "    }\n");
        return done_[d.name] = { d.name, d.name, backing.size, false };
    }

    // C alignment, which the eCLR follows: a scalar's own size, a string's header, the widest inside.
    uint32_t alignment(const Type &t) {
        switch (t.kind) {
        case Type::Scalar: return iec_scalar(t.word).size;
        case Type::String: return 2;
        case Type::Array: return alignment(*t.elem);
        case Type::Named: {
            const Def &d = *defs_.at(t.word);
            if (d.kind == Def::Enum) return iec_scalar(d.backing).size;
            if (d.kind == Def::Alias) return alignment(d.members[0].type);
            uint32_t a = 1;
            for (const Member &m : d.members) a = std::max(a, alignment(m.type));
            return a;
        }
        case Type::Map: break;
        }
        return 1;
    }
};

std::vector<Output> write_iec(const Model &model) {
    std::string out = "// Generated by rant_codegen from types/stdtypes.schema, do not edit.\n";
    out += "// The fixed standard types as IEC 61131-3 types for the PLCnext library. A field keeps its\n";
    out += "// wire name, and the library matches each type to its schema by name and layout.\n";
    out += "using System.Iec61131Lib;\nusing System.Runtime.InteropServices;\n";
    out += "using Iec61131.Engineering.Prototypes.Common;\nusing Iec61131.Engineering.Prototypes.Types;\n\n";
    out += "namespace Rant\n{\n" + Writer(model).source(model) + "}\n";
    return { { "bindings/plcnext/StandardTypes.cs", out } };
}

}  // namespace codegen
