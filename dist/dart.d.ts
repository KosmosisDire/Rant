type Frame = {
    op: number;
    flags: number;
    id: number;
    seq: number;
    peer: number;
    writtenUs: number;
    captureUs: number;
    text: string;
    payload: Uint8Array;
};
type Field = {
    path: string;
    kind: string;
    named?: string;
    elem?: string;
    elem_named?: string;
    elem_size?: number;
    elem_struct?: boolean;
    in_array?: number;
    count?: number;
    cap?: number;
    backing?: string;
    variants?: {
        name: string;
        value: number;
    }[];
    offset: number;
    size: number;
    varOrdinal?: number;
};
type SchemaBlock = {
    name?: string;
    size?: number;
    hash?: string;
    fields?: Field[];
};
type MediaTypeName = "VideoFrame" | "Image" | "ExternalVideoStream";
type MediaField = {
    path: string;
    type: MediaTypeName;
};
type Role = "pubsub" | "pub" | "sub" | "inactive";
type TopicOpts = {
    schema?: string;
    reflect?: boolean;
    reliable?: boolean;
    keep_last?: number;
    catch_up?: number;
    max_message_bytes?: number;
    heartbeat_ms?: number;
    repair_delay_ms?: number;
    backpressure_wait_ms?: number;
    shm_max_bytes?: number;
    max_rate_hz?: number;
};
type TransportName = "websocket" | "webrtc";
type NodeOpts = {
    name?: string;
    domain?: number;
    max_topics?: number;
    max_peers?: number;
    interface?: string;
    seed_peers?: string[];
    unicast_only?: boolean;
    self_ip?: string;
    advertise_port?: number;
    fragment_size?: number;
    announce_interval_ms?: number;
    peer_timeout_ms?: number;
    match_wait_ms?: number;
    disable_shm?: boolean;
    disable_logs?: boolean;
    disable_meta?: boolean;
    disable_error_logs?: boolean;
    fetch_details?: boolean;
    transport?: "auto" | "websocket";
    rtcTimeoutMs?: number;
    iceServers?: RTCIceServer[];
    onEvent?: (e: DartEvent) => void;
};
type DartEvent = {
    op: "event";
    event: string;
    text?: string;
} & Record<string, unknown>;
type LogLevelName = "error" | "warn" | "info";
type LogLine = {
    level: LogLevelName;
    node: string;
    wallUs: number;
    monoUs: number;
    recvUs: number;
    writtenUs: number;
    text: string;
};
type CallStatusName = "ok" | "app_error" | "no_handler" | "timeout" | "peer_lost" | "cancelled";
type Peer = {
    id: number;
    name: string;
    address: string;
    active: boolean;
    fragmentSize: number;
    rttUs: number;
    rttJitterUs: number;
    rttMinUs: number;
    rttSamples: number;
};
type EntityKindName = "topic" | "function" | "task" | "variable";
type Entity = {
    kind: EntityKindName;
    name: string;
    provides: boolean;
    consumes: boolean;
    reliable: boolean;
    providers?: number;
    consumers?: number;
    provider?: number;
    from?: string;
    conflict?: boolean;
    generation?: string;
    writable?: boolean;
    forceable?: boolean;
    cancellable?: boolean;
    exclusive?: boolean;
    multi?: boolean;
    incomplete?: boolean;
    index: number;
    hash: number;
    schemaHash?: string;
    rspSchemaHash?: string;
    progressSchemaHash?: string;
    schema?: SchemaBlock;
    rspSchema?: SchemaBlock;
    progressSchema?: SchemaBlock;
};
declare const MetaSection: {
    readonly Node: 1;
    readonly Proc: 2;
    readonly Topics: 4;
    readonly Peers: 8;
    readonly All: 0;
};
type MetaSnapshot = {
    valid: boolean;
    status: CallStatusName;
    provider: number;
    info: Record<string, any>;
};
type Response<Rsp = any> = {
    ok: boolean;
    status: CallStatusName;
    value: Rsp | undefined;
    data: Uint8Array;
    provider: number;
    writtenUs: number;
    message: string;
};
type RequestInfo = {
    caller: number;
    callerName: string;
    writtenUs: number;
};
type SubscriberHandler<T> = (value: T, msg: DartMessage) => void;
type FunctionHandler<Req, Rsp> = (req: Req, info: RequestInfo) => Rsp | Promise<Rsp>;
type TaskContext<Prg = any> = {
    progress: (value: Prg) => void;
    signal: AbortSignal;
    cancelled: boolean;
    caller: number;
    callerName: string;
    writtenUs: number;
};
type TaskHandler<Req, Prg, Rsp> = (req: Req, ctx: TaskContext<Prg>) => Rsp | Promise<Rsp>;
type TaskProgressHandler<Prg> = (value: Prg | null, info: {
    provider: number;
    writtenUs: number;
}) => void;
type CancelStatus = "ok" | "no_cancel" | "not_pending" | "error";
type FunctionOpts = {
    timeout_ms?: number;
    backpressure_wait_ms?: number;
    keep_last?: number;
    reflect?: boolean;
};
type TaskOpts = {
    reflect?: boolean;
    progress_best_effort?: boolean;
    progress_keep_last?: number;
    no_cancel?: boolean;
    exclusive?: boolean;
    multi?: boolean;
    timeout_ms?: number;
    backpressure_wait_ms?: number;
    keep_last?: number;
};
type VariableDefOpts<T> = {
    initial?: T;
    readOnly?: boolean;
    allowForce?: boolean;
    catch_up?: number;
    keep_last?: number;
    backpressure_wait_ms?: number;
    onWrite?: boolean;
    reflect?: boolean;
};
type RemoteVarOpts = {
    onWrite?: boolean;
    reflect?: boolean;
};
declare class Layout {
    name: string | undefined;
    size: number | undefined;
    hash: string | undefined;
    fields: Map<string, Field>;
    varFields: Field[];
    valueRoot: boolean;
    constructor(r: SchemaBlock | undefined);
    get typed(): boolean;
    mediaFields(): MediaField[];
    getField(data: Uint8Array, view: DataView, path: string): any;
    enumName(path: string, value: number | bigint): string;
    enumValue(path: string, name: string): number | undefined;
    decode(data: Uint8Array): any;
    encode(value: any): Uint8Array;
}
declare class DartMessage {
    topic: DartTopic | null;
    publisher: number;
    data: Uint8Array;
    writtenUs: number;
    captureUs: number;
    _layout: Layout;
    _view: DataView;
    constructor(layout: Layout, topic: DartTopic | null, publisher: number, data: Uint8Array, writtenUs?: number, captureUs?: number);
    get(path: string): any;
    value(): any;
    _value: any;
    text(path: string, lenField?: string): string;
}
declare class DartEntity {
    _node: DartNode;
    id: number;
    name: string;
    reliable: boolean;
    reflected: boolean;
    matchCount: number;
    ready: boolean;
    _dc: RTCDataChannel | null;
    _taps: ((value: any) => void)[];
    constructor(node: DartNode, name: string, r: any, dc: RTCDataChannel | null);
    refresh(): Promise<boolean>;
    _match(m: any): void;
    _frame(_f: Frame): void;
    _retype(_r: any): void;
    _tap(value: any): void;
    _send(op: number, flags: number, seq: number, text: string, payload: Uint8Array, captureUs?: number): void;
}
declare class DartTopic extends DartEntity {
    layout: Layout;
    onMessage: ((msg: DartMessage) => void) | null;
    constructor(node: DartNode, name: string, r: any, dc: RTCDataChannel | null);
    _retype(r: any): void;
    get size(): number | undefined;
    get hash(): string | undefined;
    get fields(): Map<string, Field>;
    sendRaw(bytes: Uint8Array, captureUs?: number): void;
    send(value: any, captureUs?: number): void;
    setRole(role: Role): Promise<any>;
    drain(timeout_ms?: number): Promise<boolean>;
    _frame(f: Frame): void;
}
declare class Publisher<T = any> {
    topic: DartTopic;
    constructor(topic: DartTopic);
    send(value: T, captureUs?: number): void;
    sendRaw(bytes: Uint8Array, captureUs?: number): void;
    get matchCount(): number;
    get ready(): boolean;
}
declare class Subscriber<T = any> {
    topic: DartTopic;
    constructor(topic: DartTopic, handler: SubscriberHandler<T>);
    get matchCount(): number;
}
declare class FunctionDefinition<Req = any, Rsp = any> extends DartEntity {
    reqLayout: Layout;
    rspLayout: Layout;
    _handler: FunctionHandler<Req, Rsp>;
    constructor(node: DartNode, name: string, r: any, dc: RTCDataChannel | null, handler: FunctionHandler<Req, Rsp>);
    get callerCount(): number;
    _retype(r: any): void;
    _frame(f: Frame): void;
    _handle(reqId: number, info: RequestInfo, payload: Uint8Array): Promise<void>;
}
type PendingCall<Rsp> = {
    resolve: (r: Response<Rsp>) => void;
    reject: (e: Error) => void;
    layout: Layout;
    timer: ReturnType<typeof setTimeout> | undefined;
    progress?: (data: Uint8Array, provider: number, writtenUs: number) => void;
};
declare class RemoteFunction<Req = any, Rsp = any> extends DartEntity {
    reqLayout: Layout;
    rspLayout: Layout;
    constructor(node: DartNode, name: string, r: any, dc: RTCDataChannel | null);
    get hasDefinition(): boolean;
    _retype(r: any): void;
    call(value: Req, timeoutMs?: number): Promise<Response<Rsp>>;
}
declare class TaskDefinition<Req = any, Prg = any, Rsp = any> extends DartEntity {
    reqLayout: Layout;
    prgLayout: Layout;
    rspLayout: Layout;
    _handler: TaskHandler<Req, Prg, Rsp>;
    _aborts: Map<number, AbortController>;
    constructor(node: DartNode, name: string, r: any, dc: RTCDataChannel | null, handler: TaskHandler<Req, Prg, Rsp>);
    get callerCount(): number;
    _retype(r: any): void;
    _frame(f: Frame): void;
    _handle(reqId: number, info: RequestInfo, payload: Uint8Array): Promise<void>;
}
declare class TaskRun<Prg = any, Rsp = any> {
    _node: DartNode;
    _prgLayout: Layout;
    id: number;
    callId: number;
    result: Promise<Response<Rsp>>;
    _onProgress: TaskProgressHandler<Prg> | null;
    _buffered: {
        value: Prg | null;
        info: {
            provider: number;
            writtenUs: number;
        };
    }[];
    _taps: ((value: any) => void)[];
    constructor(node: DartNode, id: number, prgLayout: Layout, callId: number, result: Promise<Response<Rsp>>);
    onProgress(handler: TaskProgressHandler<Prg> | null): this;
    cancel(): Promise<CancelStatus>;
    _push(data: Uint8Array, provider: number, writtenUs: number): void;
}
declare class RemoteTask<Req = any, Prg = any, Rsp = any> extends DartEntity {
    reqLayout: Layout;
    prgLayout: Layout;
    rspLayout: Layout;
    constructor(node: DartNode, name: string, r: any, dc: RTCDataChannel | null);
    get hasDefinition(): boolean;
    _retype(r: any): void;
    call(value: Req): TaskRun<Prg, Rsp>;
}
type VarWaiter = {
    res: (ok: boolean) => void;
    timer: ReturnType<typeof setTimeout> | undefined;
};
type VarChangeHandler<T> = (value: T, info: {
    forced: boolean;
    writtenUs: number;
    source: number;
}) => void;
declare class VarHandle<T = any> extends DartEntity {
    layout: Layout;
    forced: boolean;
    writtenUs: number;
    _value: T | undefined;
    _raw: Uint8Array | undefined;
    _waiters: Set<VarWaiter>;
    _onChange: VarChangeHandler<T> | null;
    _onWrite: VarChangeHandler<T> | null;
    constructor(node: DartNode, name: string, r: any, dc: RTCDataChannel | null);
    get(): T | undefined;
    getRaw(): Uint8Array | undefined;
    wait(timeoutMs?: number): Promise<boolean>;
    onChange(handler: VarChangeHandler<T> | null): void;
    onWrite(handler: VarChangeHandler<T> | null): void;
    set(value: T): void;
    force(value: T): void;
    unforce(): void;
    _retype(r: any): void;
    _frame(f: Frame): void;
}
declare class VariableDefinition<T = any> extends VarHandle<T> {
    get remoteCount(): number;
}
declare class RemoteVariable<T = any> extends VarHandle<T> {
    get hasDefinition(): boolean;
}
declare const VIDEO_FRAME = "VideoFrame";
declare const IMAGE = "Image";
declare const EXTERNAL_VIDEO_STREAM = "ExternalVideoStream";
declare const VideoCodec: {
    readonly Unknown: 0;
    readonly Mjpeg: 1;
    readonly H264: 2;
    readonly H265: 3;
    readonly Av1: 4;
};
declare const ImageFormat: {
    readonly Mono8: 0;
    readonly Mono16: 1;
    readonly Rgb8: 2;
    readonly Rgba8: 3;
    readonly Bgr8: 4;
    readonly Yuyv: 5;
    readonly Nv12: 6;
    readonly Jpeg: 16;
    readonly Png: 17;
};
declare const StreamKind: {
    readonly Rtsp: 0;
    readonly WebrtcWhep: 1;
    readonly Hls: 2;
    readonly Srt: 3;
    readonly Rtp: 4;
    readonly HttpMjpeg: 5;
    readonly Other: 15;
};
type VideoFrameValue = {
    codec: number;
    width: number;
    height: number;
    keyframe: boolean;
    pts: bigint;
    data: Uint8Array;
};
type ImageValue = {
    width: number;
    height: number;
    stride: number;
    format: number;
    data: Uint8Array;
};
type ExternalVideoStreamValue = {
    kind: number;
    codec: number;
    width: number;
    height: number;
    url: string;
    name: string;
};
type MediaValue = VideoFrameValue | ImageValue | ExternalVideoStreamValue;
type VideoPath = "none" | "track" | "decoder";
type MediaSource = DartTopic | Subscriber | VarHandle | TaskRun;
type MediaAttachOpts = {
    path?: string;
    keepData?: boolean;
};
declare class VideoView {
    _node: DartNode;
    stream: MediaStream | null;
    canvas: HTMLCanvasElement | null;
    path: VideoPath;
    frames: number;
    width: number;
    height: number;
    onFrame: ((value: MediaValue) => void) | null;
    source: DartTopic | VarHandle | TaskRun | null;
    sourcePath: string;
    sourceType: MediaTypeName | "";
    trackState: string;
    decodeState: string;
    external: ExternalVideoStreamValue | null;
    externalState: string;
    _extPc: RTCPeerConnection | null;
    _extResource: string;
    _extImg: HTMLImageElement | null;
    _extVideo: HTMLVideoElement | null;
    _extTimer: number;
    _tap: ((value: any) => void) | null;
    _transceiver: RTCRtpTransceiver | null;
    _ctx: CanvasRenderingContext2D | null;
    _canvasTrack: MediaStreamTrack | null;
    _rtcTrack: MediaStreamTrack | null;
    _decoder: any;
    _decoderCodec: string;
    _pendingBlob: {
        bytes: Uint8Array;
        mime: string;
    } | null;
    _blobBusy: boolean;
    _closed: boolean;
    constructor(node: DartNode);
    attach(target: MediaSource, opts?: MediaAttachOpts | string): Promise<this>;
    _keepData: boolean;
    detach(): void;
    close(): void;
    push(value: MediaValue): void;
    _trackReq(): {
        id: number;
        path: string;
        call: number;
        keep_data: boolean;
    } | null;
    _follow(desc: ExternalVideoStreamValue): void;
    _stopExternal(): void;
    _whep(url: string): Promise<void>;
    _paintElement(el: HTMLImageElement | HTMLVideoElement): void;
    _attachTrack(track: MediaStreamTrack): void;
    _showTrack(): void;
    _showCanvas(): void;
    _detachTrack(): void;
    _surface(w: number, h: number): boolean;
    _painted(): void;
    _paintBlob(bytes: Uint8Array, mime: string): void;
    _decode(f: VideoFrameValue): void;
}
type AnyEntity = DartTopic | FunctionDefinition | RemoteFunction | TaskDefinition | RemoteTask | VarHandle;
declare class DartNode {
    static MetaSection: {
        readonly Node: 1;
        readonly Proc: 2;
        readonly Topics: 4;
        readonly Peers: 8;
        readonly All: 0;
    };
    static VideoCodec: {
        readonly Unknown: 0;
        readonly Mjpeg: 1;
        readonly H264: 2;
        readonly H265: 3;
        readonly Av1: 4;
    };
    static ImageFormat: {
        readonly Mono8: 0;
        readonly Mono16: 1;
        readonly Rgb8: 2;
        readonly Rgba8: 3;
        readonly Bgr8: 4;
        readonly Yuyv: 5;
        readonly Nv12: 6;
        readonly Jpeg: 16;
        readonly Png: 17;
    };
    static StreamKind: {
        readonly Rtsp: 0;
        readonly WebrtcWhep: 1;
        readonly Hls: 2;
        readonly Srt: 3;
        readonly Rtp: 4;
        readonly HttpMjpeg: 5;
        readonly Other: 15;
    };
    _ws: WebSocket;
    _seq: number;
    _pending: Map<number, {
        resolve: (r: any) => void;
        reject: (e: Error) => void;
    }>;
    _entities: Map<number, AnyEntity>;
    _early: Map<number, Uint8Array[]>;
    _nextId: number;
    _calls: Map<number, PendingCall<any>>;
    _nextCall: number;
    _closing: boolean;
    _views: Set<VideoView>;
    _pc: RTCPeerConnection | null;
    _rtcUp: boolean;
    _rtcMax: number;
    _iceQueue: RTCIceCandidateInit[];
    _rtcChain: Promise<void>;
    _rtcFail: ((why: string) => void) | null;
    _anchor: RTCDataChannel | null;
    name: string;
    transport: TransportName;
    onEvent: ((e: DartEvent) => void) | null;
    onClose: ((e: CloseEvent) => void) | null;
    _onLog: ((l: LogLine) => void) | null;
    static connect(url: string, opts?: NodeOpts): Promise<DartNode>;
    constructor(ws: WebSocket);
    _request(obj: Record<string, any>): Promise<any>;
    _rtcConnect(timeoutMs: number, extraIce: RTCIceServer[]): Promise<void>;
    _rtcOffer(): Promise<void>;
    _addVideoLine(v: VideoView): void;
    _rtcState(): void;
    _rtcDrop(): void;
    _makeChannel(id: number, reliable: boolean): RTCDataChannel | null;
    _openChannel(e: DartEntity): void;
    _sendFrame(e: DartEntity, bytes: Uint8Array): void;
    _onText(m: any): void;
    _onFrame(b: Uint8Array): void;
    _onCallFrame(f: Frame): void;
    _create<E extends AnyEntity>(kind: string, name: string, reliable: boolean, fields: Record<string, any>, make: (r: any, dc: RTCDataChannel | null) => E): Promise<E>;
    topic(name: string, role?: Role, opts?: TopicOpts): Promise<DartTopic>;
    publisher<T = any>(name: string, schema: string | null, opts?: TopicOpts): Promise<Publisher<T>>;
    subscriber<T = any>(name: string, schema: string | null, handler: SubscriberHandler<T>, opts?: TopicOpts): Promise<Subscriber<T>>;
    videoView(): VideoView;
    video(name: string, opts?: TopicOpts): Promise<VideoView>;
    image(name: string, opts?: TopicOpts): Promise<VideoView>;
    functionDefinition<Req = any, Rsp = any>(name: string, reqSchema: string | null, rspSchema: string | null, handler: FunctionHandler<Req, Rsp>, opts?: FunctionOpts): Promise<FunctionDefinition<Req, Rsp>>;
    remoteFunction<Req = any, Rsp = any>(name: string, reqSchema: string | null, rspSchema: string | null, opts?: FunctionOpts): Promise<RemoteFunction<Req, Rsp>>;
    taskDefinition<Req = any, Prg = any, Rsp = any>(name: string, reqSchema: string | null, prgSchema: string | null, rspSchema: string | null, handler: TaskHandler<Req, Prg, Rsp>, opts?: TaskOpts): Promise<TaskDefinition<Req, Prg, Rsp>>;
    remoteTask<Req = any, Prg = any, Rsp = any>(name: string, reqSchema: string | null, prgSchema: string | null, rspSchema: string | null, opts?: TaskOpts): Promise<RemoteTask<Req, Prg, Rsp>>;
    variableDefinition<T = any>(name: string, schema: string | null, opts?: VariableDefOpts<T>): Promise<VariableDefinition<T>>;
    remoteVariable<T = any>(name: string, schema: string | null, opts?: RemoteVarOpts): Promise<RemoteVariable<T>>;
    settle(timeoutMs?: number): Promise<boolean>;
    log(level: LogLevelName, text: string): Promise<void>;
    logError(text: string): Promise<void>;
    logWarn(text: string): Promise<void>;
    logInfo(text: string): Promise<void>;
    onLog(handler: (line: LogLine) => void, levels?: LogLevelName[]): Promise<void>;
    peers(): Promise<Peer[]>;
    entities(): Promise<Entity[]>;
    peerEntities(peerId: number, includeDropped?: boolean): Promise<Entity[]>;
    mesh(): Promise<{
        entities: Entity[];
        epoch: number;
    }>;
    meshFind(kind: EntityKindName, name: string): Promise<Entity | null>;
    meta(peerId: number, sections?: number): Promise<MetaSnapshot>;
    close(): void;
}
export { DartNode, DartEntity, DartTopic, DartMessage, Layout, Publisher, Subscriber, FunctionDefinition, RemoteFunction, TaskDefinition, RemoteTask, TaskRun, VariableDefinition, RemoteVariable, VideoView, VideoCodec, ImageFormat, StreamKind, VIDEO_FRAME, IMAGE, EXTERNAL_VIDEO_STREAM, MetaSection, type Field, type SchemaBlock, type Role, type TopicOpts, type NodeOpts, type DartEvent, type TransportName, type CallStatusName, type Response, type RequestInfo, type SubscriberHandler, type FunctionHandler, type TaskContext, type TaskHandler, type TaskProgressHandler, type TaskOpts, type CancelStatus, type VariableDefOpts, type RemoteVarOpts, type VarChangeHandler, type VideoFrameValue, type ImageValue, type ExternalVideoStreamValue, type MediaValue, type MediaTypeName, type VideoPath, type MediaSource, type MediaAttachOpts, type MediaField, type FunctionOpts, type LogLevelName, type LogLine, type Peer, type Entity, type EntityKindName, type MetaSnapshot, };
