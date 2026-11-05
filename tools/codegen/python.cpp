// The Python backend: the dataclasses of the rant_types package and the stubs a type checker reads.
// The middleware reflects over the classes at run time, so nothing else is generated for it.
#include "backends.hpp"

#include <map>
#include <set>
#include <stdexcept>

namespace codegen {

// A name Python reserves takes a trailing underscore, which the library's name rule drops.
static std::string py_name(const std::string &name) {
    static const std::set<std::string> words = {
        "False", "None", "True", "and", "as", "assert", "async", "await", "break", "class", "continue", "def", "del",
        "elif", "else", "except", "finally", "for", "from", "global", "if", "import", "in", "is", "lambda", "nonlocal",
        "not", "or", "pass", "raise", "return", "try", "while", "with", "yield" };
    return words.count(name) ? name + "_" : name;
}

static std::string docstring(const std::string &text) {
    std::string out;
    for (char c : text) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
        if (c == '\n') out += "    ";
    }
    return "    \"\"\"" + out + "\"\"\"\n";
}

static std::string about_text(const Def &d) {
    std::string joined;
    for (size_t i = 0; i < d.about.size(); i++) joined += (i ? "\n" : "") + d.about[i];
    return joined;
}

using Defs = std::map<std::string, const Def *>;

static const Def &def_of(const Type &t, const Defs &defs) { return *defs.at(t.word); }

static bool is_kind(const Type &t, const Defs &defs, Def::Kind kind) {
    return t.kind == Type::Named && def_of(t, defs).kind == kind;
}

static bool is_float(const std::string &word) { return word == "f32" || word == "f64"; }

// An array of bytes is a Python bytes, on the wire and as a value.
static bool is_bytes(const Type &t) {
    return t.kind == Type::Array && t.elem->kind == Type::Scalar && t.elem->word == "u8";
}

// The annotation the reflection reads.
static std::string annotation(const Type &t, const Defs &defs) {
    switch (t.kind) {
    case Type::Scalar: return t.word;
    case Type::String: return t.cap ? "string(" + std::to_string(t.cap) + ")" : "str";
    case Type::Named:
        if (is_kind(t, defs, Def::Enum)) return "enum(" + t.word + ", " + def_of(t, defs).backing + ")";
        return t.word;
    case Type::Array: {
        const std::string elem = annotation(*t.elem, defs);
        if (!t.count) return "list[" + elem + "]";
        const std::string count = std::to_string(t.count);
        if (t.elem->kind == Type::Scalar || t.elem->kind == Type::String || t.elem->kind == Type::Array)
            return elem + "[" + count + "]";
        return "_Array(" + elem + ", " + count + ")";
    }
    case Type::Map: break;
    }
    throw std::runtime_error("the Python types hold no map");
}

static std::string enum_default(const Def &d) {
    for (const Member &m : d.members)
        if (m.value == 0) return d.name + "." + py_name(m.name);
    return d.name + "." + py_name(d.members[0].name);
}

// The expression of a type's zero value. shared is cleared when each value needs its own copy.
static std::string zero(const Type &t, const Defs &defs, bool &shared) {
    switch (t.kind) {
    case Type::Scalar:
        if (t.word == "bool") return "False";
        return is_float(t.word) ? "0.0" : "0";
    case Type::String: return "\"\"";
    case Type::Named: {
        const Def &d = def_of(t, defs);
        if (d.kind == Def::Enum) return enum_default(d);
        if (d.kind == Def::Alias) return zero(d.members[0].type, defs, shared);
        shared = false;
        return d.name + "()";
    }
    case Type::Array: {
        if (is_bytes(t)) return t.count ? "bytes(" + std::to_string(t.count) + ")" : "b\"\"";
        shared = false;
        if (!t.count) return "[]";
        bool elem_shared = true;
        const std::string elem = zero(*t.elem, defs, elem_shared);
        const std::string count = std::to_string(t.count);
        if (elem_shared) return "[" + elem + "] * " + count;
        return "[" + elem + " for _ in range(" + count + ")]";
    }
    case Type::Map: break;
    }
    throw std::runtime_error("the Python types hold no map");
}

// The default on a dataclass field, a factory for anything mutable.
static std::string field_default(const Type &t, const Defs &defs) {
    bool shared = true;
    const std::string value = zero(t, defs, shared);
    if (shared) return value;
    if (t.kind == Type::Named && def_of(t, defs).kind == Def::Struct)
        return "_dataclasses.field(default_factory=" + t.word + ")";
    if (t.kind == Type::Array && !t.count) return "_dataclasses.field(default_factory=list)";
    return "_dataclasses.field(default_factory=lambda: " + value + ")";
}

static std::string trailing(const Member &m) {
    std::string note = comment_of(m);
    for (char &c : note)
        if (c == '\n') c = ' ';
    return note.empty() ? "" : "  # " + note;
}

static std::string runtime_def(const Def &d, const Defs &defs) {
    if (d.kind == Def::Alias) {
        std::string out;
        for (const std::string &line : d.about) out += "# " + line + "\n";
        const Member &m = d.members[0];
        return out + d.name + " = _StdAlias(\"" + d.name + "\", " + annotation(m.type, defs) + ")" + trailing(m) + "\n";
    }
    std::string body = docstring(about_text(d));
    if (d.kind == Def::Enum) {
        body += "    __rant_std__ = \"" + d.name + "\"\n";
        for (const Member &m : d.members)
            body += "    " + py_name(m.name) + " = " + std::to_string(m.value) + trailing(m) + "\n";
        return "class " + d.name + "(_pyenum.IntEnum):\n" + body;
    }
    for (const Member &m : d.members)
        body += "    " + py_name(m.name) + ": " + annotation(m.type, defs) + " = " + field_default(m.type, defs) + trailing(m) + "\n";
    return "@_std(\"" + d.name + "\")\nclass " + d.name + ":\n" + body;
}

static std::string runtime_source(const Model &model, const std::string &extras) {
    std::string out = "\"\"\"Generated by rant_codegen from types/stdtypes.schema and types/extras.py, do not edit.\n\n"
                      "The standard types of Rant as Python classes, with no middleware. A field annotated with one\n"
                      "spells as that type on the wire, so the same schema hashes the same in every language.\"\"\"\n\n";
    out += "import dataclasses as _dataclasses\nimport enum as _pyenum\n";
    out += "from ._vocab import _Array, _StdAlias, _std, u8, u16, u32, u64, i8, i16, i32, i64, f32, f64, string, enum\n\n";
    Defs defs;
    for (const Def &d : model.defs) {
        defs[d.name] = &d;
        out += "\n" + runtime_def(d, defs) + "\n";
    }
    out += "\n" + extras + "\n";
    out += "# The vocabulary only built the roster, so it stays out of this package's names.\n";
    out += "del _Array, _StdAlias, _std, u8, u16, u32, u64, i8, i16, i32, i64, f32, f64, string, enum\n";
    return out;
}

// The type a value of this field has, which is what a type checker should see.
static std::string value_type(const Type &t) {
    switch (t.kind) {
    case Type::Scalar:
        if (t.word == "bool") return "bool";
        return is_float(t.word) ? "float" : "int";
    case Type::String: return "str";
    case Type::Named: return t.word;
    case Type::Array: return is_bytes(t) ? "bytes" : "list[" + value_type(*t.elem) + "]";
    case Type::Map: break;
    }
    throw std::runtime_error("the Python types hold no map");
}

static std::string stub_default(const Type &t) {
    if (t.kind == Type::Scalar) {
        if (t.word == "bool") return "False";
        return is_float(t.word) ? "0.0" : "0";
    }
    return t.kind == Type::String ? "\"\"" : "...";
}

static std::string stub_source(const Model &model, const std::string &extras) {
    std::string out = "\"\"\"Generated by rant_codegen from types/stdtypes.schema and types/extras.pyi, do not edit.\n\n"
                      "The standard types of Rant as the type checker sees them: every field is the Python type it\n"
                      "decodes to.\"\"\"\n\n";
    out += "from dataclasses import dataclass\nfrom enum import IntEnum\nfrom typing import TypeAlias\n\n";
    for (const Def &d : model.defs) {
        if (d.kind == Def::Alias) {
            out += d.name + ": TypeAlias = " + value_type(d.members[0].type) + "\n\n";
        } else if (d.kind == Def::Enum) {
            out += "class " + d.name + "(IntEnum):\n";
            for (const Member &m : d.members) out += "    " + py_name(m.name) + " = " + std::to_string(m.value) + "\n";
            out += "\n";
        } else {
            out += "@dataclass\nclass " + d.name + ":\n";
            if (d.members.empty()) out += "    ...\n";
            for (const Member &m : d.members)
                out += "    " + py_name(m.name) + ": " + value_type(m.type) + " = " + stub_default(m.type) + "\n";
            out += "\n";
        }
    }
    return out + extras;
}

std::vector<Output> write_python(const Model &model, const std::string &extras, const std::string &stub_extras) {
    return { { "bindings/python/rant_types/__init__.py", runtime_source(model, extras) },
             { "bindings/python/rant_types/__init__.pyi", stub_source(model, stub_extras) } };
}

}  // namespace codegen
