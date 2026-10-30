# JavaScript

The npm package `rant-middleware` is Rant for TypeScript and JavaScript: the C library built
to wasm, the binding over it and the net objects its sockets come from. It runs in Node.js 22
and later and in browsers. Releases attach it as `rant-middleware-<version>.tgz`. The
semantics are the C ones, so docs/node.md, docs/topics.md, docs/patterns.md and
docs/tasks.md apply. This page says what is different in JavaScript.

## A node

```ts
import { openNode, struct, f64, bool, string } from "rant-middleware";

const Conveyor = struct("Conveyor", { speed: f64, running: bool, part: string(16) });
const node = await openNode("dashboard", { domain: 7 });
const belt = node.publisher("Line1/Conveyor", Conveyor, { reliable: true });
belt.send({ speed: 0.5, running: true, part: "bracket" });
node.subscriber("Line1/Conveyor", Conveyor, (c) => console.log(c.speed));
```

- `openNode(name, options)` is the one async step: it loads the wasm and opens the node.
  Every factory and call after it is synchronous, and a call returns a promise.
- `options.net` gives the sockets. In Node.js leave it out for this machine's UDP. In a page
  pass `await TunnelNet.connect(url, { passphrase })` from `rant-middleware/tunnel`, which
  borrows them from a bridge (docs/web.md).
- The other options are docs/getting-started.md's in camelCase: `domain`, `prefix`,
  `multicastInterface`, `maxTopics`, `fetchDetails`, `disableLogs`, `logLevel`,
  `disableStatus`, `disableMeta`, `disableErrorLogs`, `seedPeers` as "ip" or "ip:port", `dataPort`,
  `discoveryPort`, `multicastTtl`, `fragmentSize`, `recvBufferBytes`, `sendBufferBytes`, `announceIntervalMs`,
  `peerTimeoutMs`, `maxPeers`. In Node.js `RANT_DOMAIN`, `RANT_PREFIX` and `RANT_LOG_LEVEL`
  apply as anywhere.
- `onEvent` in the options, or `node.onEvent(handler)` later, gets every event. With none,
  errors print to the console.
- Durations are milliseconds and end in `Ms`. Timestamps are microseconds as numbers, and
  64 bit integer fields are bigints.
- The binding drives the node. Handlers run after a poll, never inside one, so a handler may
  create, call and close anything. A close from a handler happens once the handlers end.
- `await version()` is the C library's version, such as `0.0.40`.

## Schemas

- `u8` to `f64`, `bool`, `string(cap?)`, `array(elem, n?)`, `enumOf(name, { Idle: 0, Run: 1 },
  backing?)`, `map`, `struct(name, fields)` and the standard types of docs/stdtypes.md as
  `types.Pose` and the rest. A handle takes its value type from the schema, and
  `Infer<typeof S>` names it.
- An array's element may be any schema, nested: `array(array(string()))` is `string[][]`.
- Values: numbers, bigints for 64 bit integers, strings, an enum's option names, typed
  arrays for numeric arrays (`Uint8Array` for `u8`, and another element type converts),
  plain arrays for any other array, plain objects for structs and maps.
- Member names are yours: `partNumber` travels as `part_number` and decodes back into
  `partNumber`.
- A schema argument is a builder, DSL text, a `CompiledSchema`, or `null` for raw
  `Uint8Array` payloads.
- `node.schema(source)` compiles one: `name`, `hash`, `dsl`, `wire`, `fields` (the C field
  table), `enumVariants(field)`, `canRead(writer)`, `encode(value)` and `decode(bytes)`.
- A missing field encodes as zero or empty. A value that does not fit answers `"schema"`.

## Topics

- `node.publisher(name, schema, options)`: `send(value, captureUs?)` returns a SendStatus,
  `matchCount`, `ready`, `drain(timeoutMs)`, `counts`.
- `node.subscriber(name, schema, handler?, options)`: the handler gets `(value, msg)`. A
  `Message` has `value`, `data`, `publisherId`, `publisherName`, `topicName`, `recvUs`,
  `writtenUs` and `captureUs`. Without a handler the subscriber is pulled: `take()` and
  `latest()` return a message or null.
- Options: `reliable`, `keepLast`, `catchUp`, `maxMessageBytes`, `heartbeatMs`,
  `repairDelayMs`, `backpressureWaitMs`, `queueBytes`, `maxRateHz`, `noTimestamp`,
  `reflectFromMesh`.
- `schema` is the schema in use, and `refresh()` takes the mesh's again for a
  `reflectFromMesh` handle. Same name handles on one node share one topic.
- SendStatus: `"ok"`, `"no_topic"`, `"too_big"`, `"role"`, `"oom"`, `"state"`, `"nosys"`,
  `"schema"`.

## Functions and tasks

```ts
const Req = struct("AddReq", { a: i32, b: i32 }), Rsp = struct("AddRsp", { sum: i32 });
node.functionDefinition("Add", (v) => ({ sum: v.a + v.b }), Req, Rsp);
const add = node.remoteFunction("Add", Req, Rsp);
const r = await add.call({ a: 2, b: 3 });     // r.status "ok", r.value { sum: 5 }
```

- A handler returns the response or a promise of it, and gets the `Request` second. A throw
  answers `"app_error"` with its message, and `new AppError(message, value)` sends data too.
- `call(value, { timeoutMs, provider })` never rejects. A `Response` has `ok`, `status`,
  `sendStatus`, `value`, `hasValue`, `data`, `message`, `provider` and `writtenUs`.
- Status: `"ok"`, `"app_error"`, `"no_handler"`, `"timeout"`, `"peer_lost"`,
  `"cancelled"`, `"no_provider"`.
- Options: `timeoutMs`, `backpressureWaitMs`, `keepLast`, `multi`, `reflectFromMesh`.
- `node.taskDefinition(name, handler, req, prg, rsp, options)`: the handler gets
  `(value, ctx)` with `ctx.progress(value)`, `ctx.cancelled` and `ctx.signal`. Throw
  `new CancelledError(message, partial)` to answer cancelled.
- `node.remoteTask(name, req, prg, rsp, options).call(value, { onProgress, provider })`
  returns a `TaskCall`: `callId`, `result` and `cancel()`, which answers `"ok"`,
  `"no_cancel"` or `"not_pending"`. `onProgress` gets null first, for RUNNING.
- Task options add `progressBestEffort`, `progressKeepLast`, `noCancel` and `exclusive`.

## Variables

- `node.variableDefinition(name, schema, options)` and `node.remoteVariable(name, schema,
  options)`. Options: `initial`, `readOnly`, `allowForce`, `catchUp`, `keepLast`,
  `backpressureWaitMs`, `reflectFromMesh`.
- `get()` is undefined before a value. `set(v)`, `force(v)` and `unforce()` return a
  SendStatus. `forced`, `wait(timeoutMs)`, `matchCount`.
- `onChange(handler)` fires with the current value, then on each change. `onWrite(handler)`
  fires on every applied write. Both get `(value, update)`, and null stops them.

## The rest

- `settle(timeoutMs?)` resolves once discovery and matching settle.
- `log(level, text)` publishes a line at `"debug"`, `"info"`, `"warn"` or `"error"`.
  `onLog(handler)` gets every other node's as a `LogLine`: `level`, `origin`, `node`,
  `nodeId`, `text`, `wallUs`, `recvUs`.
- `setStatus(key, level, text, { clearable })` raises or updates a condition,
  `clearStatus(key)` clears it. `onStatus(handler)` gets each node's whole set as a
  `StatusUpdate` (`node`, `nodeId`, `conditions`) on every change, and an empty set when a
  node goes away. `clearPeerStatus(nodeId, key, sinceUs)` asks another node to clear a
  clearable condition, whose app then sees a `"status_cleared"` event.
- `node.reflection`: `peers()`, `entities(peer = 0)`, `mesh()`, `find(kind, name)`,
  `epoch`, `meta(peer, { sections, timeoutMs })`.
- `stats`, `lastError`, `close(sendBye?)`. Every handle has `name`, `closed`, `close()` and
  `[Symbol.dispose]`, and a closed one answers `"no_topic"`.
- A failed open or factory throws `RantError` with the event behind it. `take()` throws
  `SchemaError` for a message that does not decode, and a handler's does not run but an
  error event names the topic.
