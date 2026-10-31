// What the schema cannot say in C#: the attributes the structs below and a user's own types carry.
using System;

namespace Rant
{
    /// <summary>Name the wire type of a message struct, class or enum. Without it the type's
    /// own name is the wire name. A type nested in another is defined once under its name and
    /// every use, as a field or an array element, takes the name, so peers spell the same name.</summary>
    [AttributeUsage(AttributeTargets.Struct | AttributeTargets.Class | AttributeTargets.Enum)]
    public sealed class RantSchemaAttribute : Attribute
    {
        /// <summary>The wire type name, in any spelling. The wire keeps PascalCase.</summary>
        public string Name;
        /// <summary>Name the type on the wire.</summary>
        public RantSchemaAttribute(string name) { Name = name; }
    }

    /// <summary>Fixed length arrays: the element count of each array level, outermost first.
    /// 0 leaves a level variable, and so does every level past the list. Without it every
    /// level is a variable array whose length rides the message.</summary>
    [AttributeUsage(AttributeTargets.Field | AttributeTargets.Property)]
    public sealed class RantArrayAttribute : Attribute
    {
        /// <summary>The counts, outermost first. A shorter array is zero filled, a longer one is cut.</summary>
        public int[] Counts;
        /// <summary>Fix the array levels at these counts, outermost first.</summary>
        public RantArrayAttribute(params int[] counts) { Counts = counts; }
    }

    /// <summary>A capped string field, the max UTF-8 byte length. Without it a string is
    /// unbounded. On an array of strings it caps each string.</summary>
    [AttributeUsage(AttributeTargets.Field | AttributeTargets.Property)]
    public sealed class RantStringAttribute : Attribute
    {
        /// <summary>The most UTF-8 bytes the string holds. A longer one is refused at send.</summary>
        public int Cap;
        /// <summary>Cap the string at cap UTF-8 bytes.</summary>
        public RantStringAttribute(int cap) { Cap = cap; }
    }

    /// <summary>Name a plain field's type with a standard type (<see href="https://github.com/KosmosisDire/Rant/blob/main/docs/stdtypes.md">docs/stdtypes.md</see>), such as a
    /// long as Timestamp, so the name narrows matching. The shape must be the canonical one or
    /// compiling fails. A struct names itself with [RantSchema].</summary>
    [AttributeUsage(AttributeTargets.Field | AttributeTargets.Property)]
    public sealed class RantTypeNameAttribute : Attribute
    {
        /// <summary>The standard type's name, such as Timestamp or Uuid.</summary>
        public string Name;
        /// <summary>Spell the member's type as the standard type name.</summary>
        public RantTypeNameAttribute(string name) { Name = name; }
    }

    /// <summary>Name a member on the wire when its own name will not do. Any spelling works,
    /// the wire keeps snake_case (<see href="https://github.com/KosmosisDire/Rant/blob/main/docs/stdtypes.md">docs/stdtypes.md</see>).</summary>
    [AttributeUsage(AttributeTargets.Field | AttributeTargets.Property)]
    public sealed class RantFieldAttribute : Attribute
    {
        /// <summary>The wire name, in any spelling. The wire keeps snake_case.</summary>
        public string Name;
        /// <summary>Name the member on the wire.</summary>
        public RantFieldAttribute(string name) { Name = name; }
    }
}
