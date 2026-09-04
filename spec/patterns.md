# Patterns

Functions, tasks and variables are built over plain topics with distinct topic kinds. The
user facing API is in docs/patterns.md and docs/tasks.md. Every decision here was argued
and settled. Do not relitigate them.

## Settled decisions

- Three kinds: FUNCTION (request and response, always exactly one response, one
  definition), TASK (a function with progress and cancel on the same handle), VARIABLE
  (replicated state, dumb writes, one owner). SIGNALS were removed on purpose (Aug 2026).
  Do not re add them: best effort or reliable topics and tasks cover their niche.
- Pattern headers are a fixed byte prefix in front of the payload, never schema fields,
  since schema composition would give one logical type two hashes. Payload bytes after the
  prefix are byte identical to the same value on a plain topic.
- Kind is part of the match predicate, not the name identity: same name plus different
  kind are disjoint entities that coexist, and a refused pairing fires a once per (peer,
  index) diagnostic. Same name different kind twins on one node are refused at create.
- Variable sets have no response. All set failures are local and synchronous. `v@set` is
  a plain reliable topic, not a function. Code that needs completion wants a function.
- Force is opaque to writers: writes to a forced variable are absorbed silently into the
  shadow value and unforce restores the latest absorbed write. The forced flag on the
  value prefix is the observable.
- The remote cache is authoritative only: no optimistic echo, because multi writer and
  force both make optimism wrong. `get()` answers what the system's value is.
- Multi writer stays legal on the wire (an HMI, the explorer and failover need it). The
  one authority contract is watched, not refused.
- Backpressure is on by default for every pattern channel (`DART_PATTERN_BP_WAIT_US`).
  There is no "off" spelling for it on a pattern channel, an open design question.
  Reliability and latching stay sealed. `catch_up` and `keep_last` are exposed.
- Vocabulary: DEFINITION is where the body or storage lives, REMOTE is a reference to one
  on another node. You CALL a function, the definition handles a REQUEST and sends a
  RESPONSE, the outcome is a `DartCallStatus`. No ROS vocabulary (RUNNING, never ACCEPTED,
  progress never feedback).
- The layer depends on `node/runtime` only, through kind agnostic seams: create a topic
  with a kind and a fixed payload prefix, route that topic's deliveries to a pattern
  handler, send with a header or directed to one peer, observe events and a per poll tick.
  `DART_NO_PATTERNS` strips it.
- `DartTaskOpts` uses discrete zero default progress fields, not an embedded `DartQos`,
  because a zeroed `DartQos` means best effort and would contradict the reliable default.

## Kinds

`DartTopicKind` rides bits 3 to 6 of the announce interest flags. A kind mismatch is
refused with `DART_E_KIND_MISMATCH`.

| kind | channel | who publishes |
|---|---|---|
| `TOPIC` (0) | the bare name | anyone |
| `FUNC_REQ` (1) | `f@req` | callers |
| `FUNC_RSP` (2) | `f@rsp` | the definition, directed |
| `VARIABLE` (3) | the bare name | the owner |
| `VAR_SET` (4) | `v@set` | writers |
| `TASK_REQ` (5) | `t@req` | callers, requests and cancel ops |
| `TASK_PRG` (6) | `t@prg` | the definition, broadcast |
| `TASK_RSP` (7) | `t@rsp` | the definition, directed |

## Headers

A delivered message splits into `DartMsg.header` (the pattern prefix) and
`DartMsg.data` (the user payload the schema validates). A zero length payload on a prefix
carrying channel is an op only message, exempt from schema validation, which is what lets
a remote unforce a typed variable.

- `@req`: `[u32 call_id][u8 op]`. Op 0 is CALL, op 1 is CANCEL (schema exempt). A CANCEL
  with a 4 byte `[u32 caller_lo]` payload names another caller's call.
- `@rsp`: `[u32 call_id][u8 status][u8 msg_len][msg]`. The message text is capped at
  `DART_CALL_MSG_MAX` (255) and truncated, never refused. A non OK outcome with no text
  gets default status text, so `message` is always displayable. The text rides the header
  side so the payload stays schema validated alone.
- `t@prg`: `[u32 caller_lo][u32 call_id]`. Call ids are per caller counters and peer ids
  are node local, so `call_id` alone would blend two callers' same numbered calls on the
  shared broadcast channel. `caller_lo` is the little endian u32 of the requester's uuid's
  first 4 bytes. It also gives any observer requester attribution, which is why there is
  no `@dart/meta` tasks section.

Statuses `OK`, `APP_ERROR`, `NO_HANDLER`, `CANCELLED` and `RUNNING` are wire carried.
`TIMEOUT` and `PEER_LOST` are synthesized on the caller. `RUNNING` is the one non terminal
status. Descriptive facts (cancellable, exclusive, multi, forceable, no timestamp) ride
the detail attrs byte, not the announce.

## One outcome per call

Every call gets exactly one terminal response at every exit. A returning handler with no
reply auto acks OK for a function, and answers APP_ERROR "handler returned no result" for
a task. Provider retire or close answers live deferred calls CANCELLED on the wire,
flushed before teardown. A severed destination lane synthesizes CANCELLED at the caller.
A provider crash is the PEER_LOST reap. The per call timeout covers only the window until
the first response of any kind (RUNNING or progress, whichever lands first, since they
ride different lanes).

Defer tokens live in a per handle registry. Every token verb validates membership first,
so a stale token is `DART_ERR_STATE`, never undefined behavior. `dart_function_progress`,
`dart_function_cancelled` and `dart_function_complete` are thread safe. Cancellation is
cooperative and never acked: the terminal status is the answer. A CANCELLED completion may
carry a partial result.

Traps pinned by the `task:` and `taskx:` selftests:

- A send only commits. A poll pass transmits. Retire and close drain the CANCELLED
  replies through `i_dart_node_flush_tx` (a transmit only drain) before tearing lanes down,
  or the reply stays in history forever.
- The retire announce can sever the demux before the queued reply drains (discovery is
  serviced before the data socket), so the caller side severed lane backstop on
  PEER_INTEREST is load bearing.
- Directed sends once leaked on non directed topics because the lane advance no oped
  without the directed flag. Request channels are directed in every mode now.

## The reply ring

An inline reply is a reentrant send. The handler runs under the node lock, so it can
never pump or wait for a TX pass, and the request and response rings (`keep_last`,
default 10) are the only thing holding a batch of replies. A provider whose poll pass
drains more requests than the depth answers only the last `keep_last` of them, and the
rest reach their callers as a bare TIMEOUT, with nothing logged at the provider. Size it
above the biggest batch one pass can drain, on both sides (a caller firing a burst at an
unpolled provider needs it on its request ring too). Slots allocate as used. Every
pattern channel once had a ring sized by habit and each one lost data.

A pattern layer must not hold the node lock across topic creation: `create_topic` reads a
held lock as "called from a callback" and refuses. Allocate the handle, release, create,
re lock to append.

## Directed requests

Task requests are always directed at one provider (`DartCallOpts.provider`, 0 = the
oldest matched at send time, or at flush for a call queued before any match). A function
call with no provider reaches every definition and the first answer wins. A directed send
shares the topic's seqno line and every other reliable lane skips past it. See
spec/transport.md.

## Variables

The owner publishes the value channel (`catch_up` 1, so a late remote gets the latest).
Writers push on `v@set` with no response. A read only definition creates no set channel,
and remote sets get `DART_ERR_ROLE`. Both channels keep the reliable history depth (10).
That depth is the repair window, distinct from `catch_up`, the replay window. They were
once the same line, which gave every reliable variable a one slot history: the next write
overwrote the sample a lagging accessor was about to NACK. A burst written from inside a
callback cannot wait for a TX pass, so writes that do not fit the ring are evicted before
a remote can NACK them.

Force overrides the value with a shadow source until unforce. `on_change` fires only when
the observed state actually changes (first value, different bytes, a forced flip) and
replays the current value once at registration, which kills the create to register race.
`on_write` fires on every applied write with no replay, because writes are events, not
state. Both fire inline under the node lock on the thread that applied the write. Writes
absorbed while forced fire nothing. Edge predicates would hook in at
`i_dart_var_would_change`.

A fresh remote's first write rides the send path match wait, so `DART_ERR_NO_TOPIC`
means the owner is genuinely absent. A node younger than the post open gather window pays
the gather once even for an orphan set. The `varwait:` selftests pin it.

A successor definition seeds its `write_seq` from the slot's continuing seqno line so its
first write orders above the predecessor's last at every accessor. The accessor's stale
order guard is disarmed on PEER_INTEREST when the source match count hits 0, or a same
node successor definition's values were dropped forever.

## Duplicate authority

Two authorities never form a lane (pub and pub, sub and sub), so the layer checks the
announce interest instead: at authority create against known peers, and on every
interest apply. It fires `DART_E_DUPLICATE_AUTHORITY` once per (entity, peer) on both
rivals. This is a diagnostic, not a refusal: calls take the first response and remotes
converge on the last write. The per apply check is one walk of the peer's entities
against a sorted (kind, hash) index of the local authorities, rebuilt lazily after a
create or retire. The old per authority walk made an explorer subscribing to everything
cost a 602 ms poll pass at 800 owned variables. `.multi` suppresses it.

## Retire and shadowing

Retire parks the channels (`DART_INACTIVE`, re advertised), answers every outstanding
call CANCELLED (a definition answers its live deferred calls on the wire first), silences
callbacks, unlinks the handle and frees it. It is refused with `DART_ERR_STATE` from
inside a callback and the handle then stays valid. The builtin meta handle refuses retire.

The per peer index maps bind a name to one local topic, preferring the oldest active one,
so a second same name handle created while the first lives is silently SHADOWED: it
receives nothing and its writes collide with the twin's shared reader cursor. Retire parks
the predecessor, peers re pend its verdicts and re verify the name against the successor.
The channel slots are released through the topic retire and reuse machinery
(spec/interest.md), so a re created entity takes its old slots back. The same rule means a
tool must go fully raw or fully through handles, never both. The `retire:` selftests pin
it.

## Progress reliability

`t@prg` reliability is configurable per side with `DartTaskOpts.progress_best_effort`
(default reliable). The RxO rule composes the mixed case: a reliable caller gets every
update with backpressure, and a best effort observer is out of flow control and can never
stall the task.

## The meta function

`@dart/meta` rides this layer as one handle carrying both sides (mode 2 of the function
constructor). Its request channel is reliable, so a caller's immediate in order ack from
the provider samples the round trip. See spec/node.md for the snapshot.

## Deferred, designed but not built

Variable external storage (owner get and set hooks, with `on_force` and `on_unforce`),
the owner side `on_set` transform, `exclusive_write`, publish side on change dedup, the
dispatch collapsed consumer thread delivery mode and rising or falling edge triggers (one
slot per event kind, the same `DartVariableUpdate` payload). `dart_variable_wait` cannot
wait under a service thread because the wait seam was part of that deferred design. Name
directed calls, a `call_all` keyed response list and a fast reboot challenge supersede
(which needs the host id unconditionally in the announce) were argued through and parked.
