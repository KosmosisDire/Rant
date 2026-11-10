/* The schema texts the fuzz targets share: every kind the DSL has, fixed and variable
 * fields, struct arrays two deep, variable elements, enums, maps, named and standard types. */
#ifndef RANT_FUZZ_SCHEMAS_H
#define RANT_FUZZ_SCHEMAS_H

static const char *const fuzz_schema_texts[] = {
    "Meta { note: string<4> }\nTagged { id: u32, name: string<12>, labels: string<8>[3], meta: Meta }",
    "Var { id: u32, note: string, samples: f32[], labels: string<6>[], extras: map, tail: u8 }",
    "Mode = enum<u8> { Idle=0, Running=1, Charging=2, Fault=3 }\nStep = enum<i8> { Back=-1, Hold, Fwd }\n"
    "Robot { id: u32, mode: Mode, step: Step }",
    "Cell { v: u16 }\nRow { cells: Cell[2], tag: string<4> }\nBoard { rows: Row[], when: Timestamp }",
    "Bot { id: u32, name: string<8>, pose: Pose2D }\nFleet { robots: Bot[], home: GeoPoint, ok: bool }",
    "Mode = enum<u16> { Off, On = 7, Auto }\nSlot { mode: Mode }\nPanel { mode: Mode, slots: Slot[4], gain: i64 }",
    "Seg { speed: f32, pts: Float2[], tag: string }\nRoute { id: u32, segments: Seg[], names: string[] }",
    "Pair { s: string, m: map }\nAb = enum<u8> { A, B }\nNest { g: f32[][], t: f32[3][], f: Pair[2], e: Ab[] }",
    "Image",
    "JointNames",
    "Transform",
    "f64[3]",
    "Point { x: f64 }",
};
#define FUZZ_SCHEMA_TEXTS (sizeof fuzz_schema_texts / sizeof fuzz_schema_texts[0])

#endif /* RANT_FUZZ_SCHEMAS_H */
