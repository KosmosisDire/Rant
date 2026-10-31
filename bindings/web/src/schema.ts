/* Schemas as values: each one spells its DSL text and carries the TS type of its values, so a
 * handle made from one is typed without generics written by hand. */

export type Scalar = "u8" | "u16" | "u32" | "u64" | "i8" | "i16" | "i32" | "i64" | "f32" | "f64" | "bool";

export type Shape =
    | { k: "scalar"; t: Scalar }
    | { k: "string"; cap?: number }
    | { k: "array"; elem: Shape; n?: number }
    | { k: "enum"; name: string; backing: Scalar; options: Record<string, number> }
    | { k: "map" }
    | { k: "struct"; name: string; fields: Record<string, Shape> }
    | { k: "named"; name: string };            /* a standard type, in scope by name */

declare const typeTag: unique symbol;

/* A schema: its shape, and the TS type of its values as a phantom. */
export interface Schema<T = unknown> {
    readonly shape: Shape;
    readonly [typeTag]?: T;
}

/* The TS type of a schema's values. */
export type Infer<S> = S extends Schema<infer T> ? T : never;

const make = <T>(shape: Shape): Schema<T> => ({ shape }) as Schema<T>;

/* A scalar keeps its kind in its type, so an array of it knows which typed array it is. */
type ScalarSchema<T, K extends Scalar> = Schema<T> & { readonly shape: { k: "scalar"; t: K } };
const scalar = <T, K extends Scalar>(t: K) => make<T>({ k: "scalar", t }) as ScalarSchema<T, K>;

export const u8 = scalar<number, "u8">("u8");
export const u16 = scalar<number, "u16">("u16");
export const u32 = scalar<number, "u32">("u32");
export const u64 = scalar<bigint, "u64">("u64");
export const i8 = scalar<number, "i8">("i8");
export const i16 = scalar<number, "i16">("i16");
export const i32 = scalar<number, "i32">("i32");
export const i64 = scalar<bigint, "i64">("i64");
export const f32 = scalar<number, "f32">("f32");
export const f64 = scalar<number, "f64">("f64");
export const bool = scalar<boolean, "bool">("bool");
export const map = make<Record<string, unknown>>({ k: "map" });

/* A string of at most cap bytes, or of any length without one. */
export function string(cap?: number): Schema<string> { return make<string>({ k: "string", cap }); }

/* The typed array each numeric element kind arrives as. Booleans arrive as boolean[]. */
type TypedFor = {
    u8: Uint8Array; u16: Uint16Array; u32: Uint32Array; u64: BigUint64Array; i8: Int8Array;
    i16: Int16Array; i32: Int32Array; i64: BigInt64Array; f32: Float32Array; f64: Float64Array;
};

/* An array of n elements, or of any length without n. Numeric elements arrive as typed arrays. */
export function array<S extends Schema<any>>(elem: S, n?: number):
    Schema<S extends { shape: { k: "scalar"; t: infer K extends keyof TypedFor } } ? TypedFor[K] : Infer<S>[]> {
    return make({ k: "array", elem: elem.shape, n });
}

/* A named integer with named values, read and written as the names. */
export function enumOf<const O extends Record<string, number>>(name: string, options: O, backing: Schema<number> = u8):
    Schema<keyof O & string> {
    const s = backing.shape;
    if (s.k !== "scalar") throw new TypeError("an enum is backed by an integer");
    return make({ k: "enum", name, backing: s.t, options });
}

/* A named struct. Member names are yours, the wire spells them in snake_case. */
export function struct<const F extends Record<string, Schema<any>>>(name: string, fields: F):
    Schema<{ -readonly [K in keyof F]: Infer<F[K]> }> {
    const shapes: Record<string, Shape> = {};
    for (const [k, v] of Object.entries(fields)) shapes[k] = v.shape;
    return make({ k: "struct", name, fields: shapes });
}

/* DSL text for a schema: named structs and enums as leading definitions, the root last. */
export function dsl(s: Schema<any> | string): string {
    if (typeof s === "string") return s;
    const defs = new Map<string, string>();
    const spell = (sh: Shape): string => {
        switch (sh.k) {
        case "scalar": return sh.t;
        case "string": return sh.cap === undefined ? "string" : `string<${sh.cap}>`;
        case "array": return `${spell(sh.elem)}[${sh.n ?? ""}]`;
        case "enum":
            if (!defs.has(sh.name)) {
                const body = Object.entries(sh.options).map(([k, v]) => `${k} = ${v}`).join(", ");
                defs.set(sh.name, `${sh.name} = enum<${sh.backing}> { ${body} }`);
            }
            return sh.name;
        case "map": return "map";
        case "named": return sh.name;
        case "struct": {
            if (!defs.has(sh.name)) {
                defs.set(sh.name, "");            /* reserve the slot before members, so order stays dependency first */
                const body = Object.entries(sh.fields).map(([k, f]) => `${k}: ${spell(f)}`).join(", ");
                defs.delete(sh.name);
                defs.set(sh.name, `${sh.name} { ${body} }`);
            }
            return sh.name;
        }
        }
    };
    const root = spell(s.shape);
    if (s.shape.k === "struct") return [...defs.values()].join("\n");
    return [...defs.values(), root].join("\n");
}

/* A standard type by its name alone, which is in scope in every schema. */
export const named = <T>(name: string) => make<T>({ k: "named", name });
