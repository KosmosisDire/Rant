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

export type ImageFormat = "Mono8" | "Mono16" | "Rgb8" | "Rgba8" | "Bgr8" | "Yuyv" | "Nv12" | "Monof32" | "Jpeg" | "Png";
export type VideoCodec = "Unknown" | "Mjpeg" | "H264" | "H265" | "Av1";
export type VideoStreamKind = "Rtsp" | "WebrtcWhep" | "Hls" | "Srt" | "Rtp" | "HttpMjpeg" | "Other";
export type DistortionModel = "None" | "BrownConrady" | "Fisheye" | "Rational";
export type RangeKind = "Unknown" | "Ultrasound" | "Infrared" | "Laser" | "Radar";
export type GnssStatus = "NoFix" | "Fix" | "Differential" | "RtkFloat" | "RtkFixed";
export type ScalarKind = "I8" | "U8" | "I16" | "U16" | "I32" | "U32" | "F32" | "F64" | "I64" | "U64";
export interface PointFieldValue { name: string; offset: number; kind: ScalarKind; count: number }
/* The PCL layout: point_step bytes per point, little endian. width 0 when it has no rows. */
export interface PointCloudValue {
    origin: { pose: PoseT; parent: string }; width: number; fields: PointFieldValue[];
    point_step: number; dense: boolean; data: Uint8Array;
}
export interface GridFieldValue { name: string; kind: ScalarKind; data: Uint8Array }
/* origin is the outer corner of cell 0, each field one number per cell, x fastest, then y, then z. */
export interface GridValue {
    origin: { pose: PoseT; parent: string }; resolution: number;
    cells_x: number; cells_y: number; cells_z: number; fields: GridFieldValue[];
}
export interface VideoFrameValue {
    codec: VideoCodec;
    width: number; height: number; keyframe: boolean; pts: bigint; data: Uint8Array;
}
export interface ImageValue {
    width: number; height: number; stride: number; format: ImageFormat; data: Uint8Array;
}
export interface ExternalVideoStreamValue {
    kind: VideoStreamKind; codec: VideoCodec;
    width: number; height: number; url: string; name: string;
}

const named = <T>(name: string) => make<T>({ k: "named", name });
type D2 = { x: number; y: number };
type D3 = { x: number; y: number; z: number };
type D4 = { x: number; y: number; z: number; w: number };
type Seg = { a: D3; b: D3 };
type Seg2 = { a: D2; b: D2 };
type PoseT = { position: D3; orientation: D4 };
type Pose2T = { position: D2; angle: number };
type PoseCov = { pose: PoseT; covariance: Float64Array[] };
type TwistCov = { twist: { linear: D3; angular: D3 }; covariance: Float64Array[] };
type TrajectoryPoint = { time: bigint; position: Float64Array; velocity: Float64Array;
                         acceleration: Float64Array; effort: Float64Array };

/* The standard types of docs/stdtypes.md, by name. */
export const types = {
    Float2: named<D2>("Float2"), Float3: named<D3>("Float3"), Float4: named<D4>("Float4"),
    Double2: named<D2>("Double2"), Double3: named<D3>("Double3"), Double4: named<D4>("Double4"),
    Int2: named<D2>("Int2"), Int3: named<D3>("Int3"), Int4: named<D4>("Int4"),
    Quaternion: named<D4>("Quaternion"),
    Pose: named<PoseT>("Pose"), Pose2D: named<Pose2T>("Pose2D"),
    Transform: named<{ pose: PoseT; parent: string }>("Transform"),
    Transform2D: named<{ pose: Pose2T; parent: string }>("Transform2D"),
    Twist: named<{ linear: D3; angular: D3 }>("Twist"), Twist2D: named<{ linear: D2; angular: number }>("Twist2D"),
    Wrench: named<{ force: D3; torque: D3 }>("Wrench"), Wrench2D: named<{ force: D2; torque: number }>("Wrench2D"),
    Accel: named<{ linear: D3; angular: D3 }>("Accel"), Accel2D: named<{ linear: D2; angular: number }>("Accel2D"),
    PoseWithCovariance: named<PoseCov>("PoseWithCovariance"),
    TwistWithCovariance: named<TwistCov>("TwistWithCovariance"),
    Odometry: named<{ pose: PoseCov; twist: TwistCov; parent: string }>("Odometry"),
    GeoPoint: named<{ lat: number; lon: number; alt: number }>("GeoPoint"),
    Matrix3x3: named<Float32Array[]>("Matrix3x3"), Matrix4x4: named<Float32Array[]>("Matrix4x4"),
    Covariance3: named<Float64Array[]>("Covariance3"), Covariance6: named<Float64Array[]>("Covariance6"),
    Timestamp: named<bigint>("Timestamp"), Duration: named<bigint>("Duration"), Uuid: named<Uint8Array>("Uuid"),
    AlignedBox: named<{ min: D3; max: D3 }>("AlignedBox"), OrientedBox: named<{ pose: PoseT; size: D3 }>("OrientedBox"),
    Plane: named<{ position: D3; normal: D3 }>("Plane"), Segment: named<Seg>("Segment"),
    Ray: named<{ origin: D3; direction: D3 }>("Ray"), Ray2D: named<{ origin: D2; direction: D2 }>("Ray2D"),
    Sphere: named<{ center: D3; radius: number }>("Sphere"), Capsule: named<{ axis: Seg; radius: number }>("Capsule"),
    Cylinder: named<{ axis: Seg; radius: number }>("Cylinder"), Cone: named<{ base: D3; tip: D3; radius: number }>("Cone"),
    Polygon: named<{ points: D3[] }>("Polygon"),
    AlignedBox2D: named<{ min: D2; max: D2 }>("AlignedBox2D"), OrientedBox2D: named<{ pose: Pose2T; size: D2 }>("OrientedBox2D"),
    Plane2D: named<{ position: D2; normal: D2 }>("Plane2D"), Segment2D: named<Seg2>("Segment2D"),
    Circle: named<{ center: D2; radius: number }>("Circle"), Capsule2D: named<{ axis: Seg2; radius: number }>("Capsule2D"),
    Polygon2D: named<{ points: D2[] }>("Polygon2D"),
    Prism: named<{ pose: PoseT; base: { points: D2[] }; height: number }>("Prism"),
    Polyline: named<{ points: D3[] }>("Polyline"), Polyline2D: named<{ points: D2[] }>("Polyline2D"),
    Path: named<{ poses: PoseT[] }>("Path"), Path2D: named<{ poses: Pose2T[] }>("Path2D"),
    Inertia: named<{ mass: number; center: D3; ixx: number; ixy: number; ixz: number;
                     iyy: number; iyz: number; izz: number }>("Inertia"),
    DistortionModel: named<DistortionModel>("DistortionModel"),
    CameraIntrinsics: named<{ width: number; height: number; fx: number; fy: number; cx: number; cy: number;
                              model: DistortionModel; coeffs: Float64Array }>("CameraIntrinsics"),
    JointState: named<{ position: Float64Array; velocity: Float64Array; effort: Float64Array }>("JointState"),
    JointNames: named<{ name: string[] }>("JointNames"),
    JointTrajectoryPoint: named<TrajectoryPoint>("JointTrajectoryPoint"),
    JointTrajectory: named<{ points: TrajectoryPoint[] }>("JointTrajectory"),
    Imu: named<{ orientation: D4; orientation_covariance: Float64Array[];
                 angular_velocity: D3; angular_velocity_covariance: Float64Array[];
                 linear_acceleration: D3; linear_acceleration_covariance: Float64Array[] }>("Imu"),
    LaserScan: named<{ angle_min: number; angle_step: number; time_step: number; range_min: number;
                       range_max: number; echoes: number; ranges: Float32Array; intensities: Float32Array }>("LaserScan"),
    RangeKind: named<RangeKind>("RangeKind"),
    RangeReading: named<{ distance: number; min: number; max: number; field_of_view: number;
                          kind: RangeKind }>("RangeReading"),
    GnssStatus: named<GnssStatus>("GnssStatus"),
    GnssFix: named<{ position: { lat: number; lon: number; alt: number }; covariance: Float64Array[];
                     status: GnssStatus }>("GnssFix"),
    ScalarKind: named<ScalarKind>("ScalarKind"),
    PointField: named<PointFieldValue>("PointField"), PointCloud: named<PointCloudValue>("PointCloud"),
    Mesh: named<{ vertices: PointCloudValue; triangles: Uint32Array }>("Mesh"),
    GridField: named<GridFieldValue>("GridField"), Grid: named<GridValue>("Grid"),
    Tensor: named<{ kind: ScalarKind; shape: Uint32Array; data: Uint8Array }>("Tensor"),
    File: named<{ name: string; media_type: string; data: Uint8Array }>("File"),
    Detection2D: named<{ bounds: { pose: Pose2T; size: D2 }; label: string; score: number; track: number }>("Detection2D"),
    Detection3D: named<{ bounds: { pose: PoseT; size: D3 }; label: string; score: number; track: number }>("Detection3D"),
    Color: named<{ r: number; g: number; b: number; a: number }>("Color"),
    Uri: named<string>("Uri"), Empty: named<Record<string, never>>("Empty"),
    ImageFormat: named<ImageFormat>("ImageFormat"), VideoCodec: named<VideoCodec>("VideoCodec"),
    VideoStreamKind: named<VideoStreamKind>("VideoStreamKind"),
    Image: named<ImageValue>("Image"), VideoFrame: named<VideoFrameValue>("VideoFrame"),
    ExternalVideoStream: named<ExternalVideoStreamValue>("ExternalVideoStream"),
} as const;
