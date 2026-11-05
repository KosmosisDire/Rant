"""What the standard types are written in: the field type markers, also rant.u8 and the rest.
A marker is shared with the middleware, which reads them to reflect a class into a schema."""

import dataclasses as _dataclasses
import enum as _pyenum

# raw kind bytes, as the schema wire numbers them
_U8, _U16, _U32, _U64 = 0, 1, 2, 3
_I8, _I16, _I32, _I64 = 4, 5, 6, 7
_F32, _F64 = 8, 9


class _Type:
    """A Rant scalar field-type marker (rant.u8 ... rant.f64)."""
    def __init__(self, kind, token):
        self.kind = kind
        self.token = token

    def __getitem__(self, count):
        if not isinstance(count, int) or count <= 0:
            raise TypeError("array size must be a positive int, e.g. rant.f64[9]")
        return _Array(self, count)

    def __repr__(self):
        return "rant." + self.token


class _String:
    """A capped string field marker: rant.string(16) is string<16>. Subscript it for a
        fixed array: rant.string(8)[4]."""
    def __init__(self, cap):
        if not isinstance(cap, int) or cap <= 0:
            raise TypeError("string cap must be a positive int, e.g. rant.string(16)")
        self.cap = cap

    def __getitem__(self, count):
        if not isinstance(count, int) or count <= 0:
            raise TypeError("array size must be a positive int, e.g. rant.string(8)[4]")
        return _Array(self, count)

    def __repr__(self):
        return "rant.string(%d)" % self.cap


class _Array:
    """A fixed array marker: rant.f32[3]. Subscript it again for an array of arrays, read as
        the DSL reads: rant.f32[3][2] is two arrays of three."""
    def __init__(self, elem, count):
        self.elem = elem     # any field type: a marker, a class or a list[...] alias
        self.count = count

    def __getitem__(self, count):
        if not isinstance(count, int) or count <= 0:
            raise TypeError("array size must be a positive int, e.g. rant.f32[3][2]")
        return _Array(self, count)

    def __repr__(self):
        return "%r[%d]" % (self.elem, self.count)


u8 = _Type(_U8, "u8")
u16 = _Type(_U16, "u16")
u32 = _Type(_U32, "u32")
u64 = _Type(_U64, "u64")
i8 = _Type(_I8, "i8")
i16 = _Type(_I16, "i16")
i32 = _Type(_I32, "i32")
i64 = _Type(_I64, "i64")
f32 = _Type(_F32, "f32")
f64 = _Type(_F64, "f64")


def string(cap):
    """A capped UTF-8 string field: rant.string(16) is string<16> in the DSL. Every string
        field needs a cap, and rant.string(8)[4] is a fixed array of them."""
    return _String(cap)


# Enum fields come before the standard types below, which declare their own (the
# video family's format / codec / kind).

def _infer_enum_backing(cls):
    """Smallest scalar kind that fits every member value: unsigned when all >= 0, else
    signed. Cross-language matching wants an explicit backing (rant.enum(cls, rant.u8))."""
    vals = [int(m.value) for m in cls] or [0]
    lo, hi = min(vals), max(vals)
    if lo >= 0:
        return (_U8 if hi <= 0xFF else _U16 if hi <= 0xFFFF
                else _U32 if hi <= 0xFFFFFFFF else _U64)
    fits = lambda bits: lo >= -(1 << (bits - 1)) and hi <= (1 << (bits - 1)) - 1
    return _I8 if fits(8) else _I16 if fits(16) else _I32 if fits(32) else _I64


class _Enum:
    """An enum field marker (a named integer). Wraps an enum.Enum subclass whose member
    values ride the wire, plus the backing scalar kind."""
    def __init__(self, cls, backing=None):
        if not (isinstance(cls, type) and issubclass(cls, _pyenum.Enum)):
            raise TypeError("rant.enum expects an enum.Enum subclass")
        self.cls = cls
        if isinstance(backing, _Type):
            self.kind = backing.kind
        elif backing is None:
            self.kind = _infer_enum_backing(cls)
        else:
            raise TypeError("enum backing must be a rant scalar type, e.g. rant.u8")

    def __repr__(self):
        return "rant.enum(%s)" % self.cls.__name__


def enum(cls, backing=None):
    """A named integer field from an enum.Enum. Each member's value rides the wire. backing
        pins the wire width, else it is inferred. A bare enum annotation is the same."""
    return _Enum(cls, backing)


# The machinery behind the standard types: a field of one spells as the type name on the wire,
# so two programs that both mean a 3D point say so with the same bytes.

class _StdAlias:
    """A standard type that is an alias of one wire type: it encodes like under but spells
        as its name, so its values are ordinary Python ints, bytes or str."""
    def __init__(self, name, under):
        self.name = name
        self.under = under      # a _Type, _Array or _String

    def __repr__(self):
        return "rant.types." + self.name


def _std(name):
    """Class decorator: this class IS the standard type `name`, so a field of it spells
    as the name (and must have the canonical shape, else compiling the schema fails)."""
    def wrap(cls):
        cls.__rant_std__ = name
        return _dataclasses.dataclass(cls)
    return wrap
