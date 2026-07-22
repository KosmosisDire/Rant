type Field = {
    path: string;
    kind: string;
    elem?: string;
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
    size?: number;
    hash?: string;
    fields?: Field[];
};
type Role = "pubsub" | "pub" | "sub" | "inactive";
type TopicOpts = {
    schema?: string;
    reliable?: boolean;
    keep_last?: number;
    catch_up?: number;
    max_message_bytes?: number;
    heartbeat_ms?: number;
    repair_delay_ms?: number;
    backpressure_wait_ms?: number;
    shm_max_bytes?: number;
};
type NodeOpts = {
    name?: string;
    domain?: number;
    max_topics?: number;
    max_peers?: number;
    interface?: string;
    seed_peers?: string[];
    fragment_size?: number;
    announce_interval_ms?: number;
    peer_timeout_ms?: number;
    match_wait_ms?: number;
    disable_shm?: boolean;
    onEvent?: (e: DartEvent) => void;
};
type DartEvent = {
    op: "event";
    event: string;
    text?: string;
} & Record<string, unknown>;
type CallStatusName = "ok" | "app_error" | "no_handler" | "timeout" | "peer_lost" | "cancelled";
type Response<Rsp = any> = {
    ok: boolean;
    status: CallStatusName;
    value: Rsp | undefined;
    data: Uint8Array;
    provider: number;
};
type RequestInfo = {
    caller: number;
    callerName: string;
};
type SignalInfo = {
    emitter: number;
    data: Uint8Array;
};
type SubscriberHandler<T> = (value: T, msg: DartMessage) => void;
type FunctionHandler<Req, Rsp> = (req: Req, info: RequestInfo) => Rsp | Promise<Rsp>;
type SignalHandler<T> = (value: T, info: SignalInfo) => void;
type VariableDefOpts<T> = {
    initial?: T;
    readOnly?: boolean;
    allowForce?: boolean;
    catch_up?: number;
    backpressure_wait_ms?: number;
};
declare class Layout {
    size: number | undefined;
    hash: string | undefined;
    fields: Map<string, Field>;
    varFields: Field[];
    constructor(r: SchemaBlock | undefined);
    get typed(): boolean;
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
    _layout: Layout;
    _view: DataView;
    constructor(layout: Layout, topic: DartTopic | null, publisher: number, data: Uint8Array);
    get(path: string): any;
    value(): any;
    text(path: string, lenField?: string): string;
}
declare class DartTopic {
    _node: DartNode;
    name: string;
    id: number;
    layout: Layout;
    onMessage: ((msg: DartMessage) => void) | null;
    matchCount: number;
    ready: boolean;
    constructor(node: DartNode, name: string, r: {
        id: number;
    } & SchemaBlock);
    get size(): number | undefined;
    get hash(): string | undefined;
    get fields(): Map<string, Field>;
    sendRaw(bytes: Uint8Array): void;
    send(values: Record<string, any>): void;
    setRole(role: Role): Promise<any>;
    drain(timeout_ms?: number): Promise<boolean>;
    _match(m: any): void;
    _deliver(publisher: number, data: Uint8Array): void;
}
declare class Publisher<T = any> {
    topic: DartTopic;
    constructor(topic: DartTopic);
    send(value: T): void;
    sendRaw(bytes: Uint8Array): void;
    get matchCount(): number;
    get ready(): boolean;
}
declare class Subscriber<T = any> {
    topic: DartTopic;
    constructor(topic: DartTopic, handler: SubscriberHandler<T>);
    get matchCount(): number;
}
declare class FunctionDefinition<Req = any, Rsp = any> {
    _node: DartNode;
    id: number;
    name: string;
    reqLayout: Layout;
    rspLayout: Layout;
    callerCount: number;
    _handler: FunctionHandler<Req, Rsp>;
    constructor(node: DartNode, name: string, r: any, handler: FunctionHandler<Req, Rsp>);
    _match(m: any): void;
    _handle(reqId: number, info: RequestInfo, payload: Uint8Array): Promise<void>;
}
type PendingCall<Rsp> = {
    resolve: (r: Response<Rsp>) => void;
    reject: (e: Error) => void;
    layout: Layout;
    timer: ReturnType<typeof setTimeout> | undefined;
};
declare class RemoteFunction<Req = any, Rsp = any> {
    _node: DartNode;
    id: number;
    name: string;
    reqLayout: Layout;
    rspLayout: Layout;
    hasDefinition: boolean;
    constructor(node: DartNode, name: string, r: any);
    _match(m: any): void;
    call(value: Req, timeoutMs?: number): Promise<Response<Rsp>>;
}
type VarWaiter = {
    res: (ok: boolean) => void;
    timer: ReturnType<typeof setTimeout> | undefined;
};
type VarChangeHandler<T> = (value: T, info: {
    forced: boolean;
}) => void;
declare class VarHandle<T = any> {
    _node: DartNode;
    id: number;
    name: string;
    layout: Layout;
    forced: boolean;
    _value: T | undefined;
    _raw: Uint8Array | undefined;
    _waiters: Set<VarWaiter>;
    _onChange: VarChangeHandler<T> | null;
    constructor(node: DartNode, name: string, r: any);
    get(): T | undefined;
    raw(): Uint8Array | undefined;
    wait(timeoutMs?: number): Promise<boolean>;
    onChange(handler: VarChangeHandler<T> | null): void;
    set(value: T): void;
    force(value: T): void;
    unforce(): void;
    _sendVar(mode: number, payload: Uint8Array): void;
    _update(payload: Uint8Array, forced: boolean): void;
    _match(_m: any): void;
}
declare class VariableDefinition<T = any> extends VarHandle<T> {
    remoteCount: number;
    constructor(node: DartNode, name: string, r: any);
    _match(m: any): void;
}
declare class RemoteVariable<T = any> extends VarHandle<T> {
    hasDefinition: boolean;
    constructor(node: DartNode, name: string, r: any);
    _match(m: any): void;
}
declare class DartSignal<T = any> {
    _node: DartNode;
    id: number;
    name: string;
    layout: Layout;
    listenerCount: number;
    _handler: SignalHandler<T> | null;
    constructor(node: DartNode, name: string, r: any, handler: SignalHandler<T> | null);
    emit(value?: T): void;
    _match(m: any): void;
    _fire(emitter: number, data: Uint8Array): void;
}
type PatternEntity = FunctionDefinition | RemoteFunction | VarHandle | DartSignal;
declare class DartNode {
    _ws: WebSocket;
    _seq: number;
    _pending: Map<number, {
        resolve: (r: any) => void;
        reject: (e: Error) => void;
    }>;
    _topics: Map<number, DartTopic>;
    _entities: Map<number, PatternEntity>;
    _reqMeta: Map<number, RequestInfo>;
    _calls: Map<number, PendingCall<any>>;
    _nextCall: number;
    _closing: boolean;
    name: string;
    onEvent: ((e: DartEvent) => void) | null;
    onClose: ((e: CloseEvent) => void) | null;
    static connect(url: string, opts?: NodeOpts): Promise<DartNode>;
    constructor(ws: WebSocket);
    _request(obj: Record<string, any>): Promise<any>;
    _onText(m: any): void;
    _onBinary(buf: ArrayBuffer): void;
    topic(name: string, role?: Role, opts?: TopicOpts): Promise<DartTopic>;
    publisher<T = any>(name: string, schema: string | null, opts?: TopicOpts): Promise<Publisher<T>>;
    subscriber<T = any>(name: string, schema: string | null, handler: SubscriberHandler<T>, opts?: TopicOpts): Promise<Subscriber<T>>;
    functionDefinition<Req = any, Rsp = any>(name: string, reqSchema: string | null, rspSchema: string | null, handler: FunctionHandler<Req, Rsp>): Promise<FunctionDefinition<Req, Rsp>>;
    remoteFunction<Req = any, Rsp = any>(name: string, reqSchema: string | null, rspSchema: string | null): Promise<RemoteFunction<Req, Rsp>>;
    variableDefinition<T = any>(name: string, schema: string | null, opts?: VariableDefOpts<T>): Promise<VariableDefinition<T>>;
    remoteVariable<T = any>(name: string, schema: string | null): Promise<RemoteVariable<T>>;
    signal<T = any>(name: string, schema: string | null, handler?: SignalHandler<T>): Promise<DartSignal<T>>;
    settle(timeoutMs?: number): Promise<boolean>;
    close(): void;
}
export { DartNode, DartTopic, DartMessage, Layout, Publisher, Subscriber, FunctionDefinition, RemoteFunction, VariableDefinition, RemoteVariable, DartSignal, type Field, type SchemaBlock, type Role, type TopicOpts, type NodeOpts, type DartEvent, type CallStatusName, type Response, type RequestInfo, type SignalInfo, type SubscriberHandler, type FunctionHandler, type SignalHandler, type VariableDefOpts, };
