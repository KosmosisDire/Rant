// The language neutral model of a schema text, which every backend writes from. Rant's own
// compiler reads the text, so the model always agrees with the wire.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace codegen {

// A type as a field, an alias or an array element spells it.
struct Type {
    enum Kind { Scalar, String, Map, Array, Named };
    Kind kind = Scalar;
    std::string word;            // Scalar: u8 to f64 or bool. Named: the type's wire name
    uint16_t cap = 0;            // String: the most bytes, 0 when unbounded
    uint16_t count = 0;          // Array: the elements, 0 when variable
    std::shared_ptr<Type> elem;  // Array
};

// A struct's field, an enum's option, or the one type of an alias.
struct Member {
    std::string name;            // the wire spelling
    Type type;                   // not for an option
    int64_t value = 0;           // an option only
    std::string unit, note;      // its comment, split at the [unit]
};

struct Def {
    enum Kind { Struct, Enum, Alias };
    Kind kind = Struct;
    std::string name;
    std::vector<std::string> about;   // the description, one entry per line
    std::vector<Member> members;
    std::string backing;              // Enum: its integer kind
    bool fixed = true;                // no variable part anywhere inside
    uint32_t packed = 0;              // the fixed bytes on the wire
    uint64_t hash = 0;
};

struct Model {
    std::vector<Def> defs;
    uint32_t revision = 0;            // changes with any type's name or wire, never with a comment
};

// Reads a schema text. standalone resolves only the names it defines. Throws a runtime_error
// that says why and at which line and column.
Model load_model(const std::string &text, bool standalone);

// The wire spelling of a type name as a field name, Pose2D to pose2_d.
std::string snake(const std::string &name);

// The type as schema text, such as string<30>, Double3[] or f32[3][3].
std::string spell(const Type &type);
// A member's comment as schema text, [unit] then note, with lines apart by newlines.
std::string comment_of(const Member &member);
// The definition's body as schema text with its comments, without the name or the about lines.
std::string body_of(const Def &def);
// The whole definition as schema text.
std::string definition_of(const Def &def);

}  // namespace codegen
