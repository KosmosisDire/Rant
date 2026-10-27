/* The schema texts the fuzz targets share: every kind the DSL has, fixed and variable
 * fields, struct arrays two deep, variable elements, enums, maps, named and standard types. */
#ifndef RANT_FUZZ_SCHEMAS_H
#define RANT_FUZZ_SCHEMAS_H

static const char *const fuzz_schema_texts[] = {
    "Tagged { id: u32, name: string<12>, labels: string<8>[3], meta: { note: string<4> } }",
    "Var { id: u32, note: string, samples: f32[], labels: string<6>[], extras: map, tail: u8 }",
    "Robot { id: u32, mode: enum<u8> { Idle=0, Running=1, Charging=2, Fault=3 },"
    " step: enum<i8> { Back=-1, Hold, Fwd } }",
    "Cell { v: u16 }\nRow { cells: Cell[2], tag: string<4> }\nGrid { rows: Row[], when: Timestamp }",
    "Fleet { robots: { id: u32, name: string<8>, pose: Pose2D }[], home: GeoPoint, ok: bool }",
    "Mode = enum<u16> { Off, On = 7, Auto }\nPanel { mode: Mode, slots: { mode: Mode }[4], gain: i64 }",
    "Path { id: u32, segments: { speed: f32, pts: Float2[], tag: string }[], names: string[] }",
    "Grid { g: f32[][], t: f32[3][], f: { s: string, m: map }[2], e: enum<u8> { A, B }[] }",
    "Image",
    "JointNames",
    "Transform",
    "f64[3]",
    "{ x: f64 }",
};
#define FUZZ_SCHEMA_TEXTS (sizeof fuzz_schema_texts / sizeof fuzz_schema_texts[0])

#endif /* RANT_FUZZ_SCHEMAS_H */
