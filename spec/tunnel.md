# The tunnel

A node that cannot open UDP sockets, Rant in a page, borrows them from a bridge: the bridge
binds sockets on its host and forwards datagrams both ways, so to the mesh the node is a
node of that host. The bridge reads none of them. A native node does the same to reach a
mesh it cannot join. `bindings/web/src/tunnel.ts` (`TunnelNet`) and `src/tunnel/` are the
clients. `RANT/bridge-rant` is the server, and this file is the protocol it serves.

## Connection

1. The client opens `ws://host:7480`, or `wss://` behind a TLS proxy.
2. With `allowed_origins` set, a browser whose `Origin` is not listed is closed with 4003.
3. The client asks for a challenge, then sends `open`. Nothing else is accepted before it.
4. The bridge replies, then pings at the probe interval.

Text messages are JSON. A request carries `op` and a client chosen `seq`, and its reply is
`{"op":"reply","seq":N,"ok":true,...}` or `{"op":"reply","seq":N,"ok":false,"error":"..."}`.

The passphrase never crosses the wire. `{"op":"challenge","seq":1}` answers a `nonce`, 32 hex
characters good for the next open on this connection only, and the open proves the
passphrase on it:

```json
{"op":"open","seq":2,"proto":14,"proof":"5bdc...","name":"page"}
```

| Key | Meaning |
|---|---|
| `proto` | 14. Any other is refused and the socket closed with 4001. |
| `proof` | HMAC SHA-256 of the nonce's text keyed with the passphrase, 64 lowercase hex characters. A wrong one, or an open with no challenge before it, is refused after 1 s and closed with 4001. A bridge with no passphrase reads none. |
| `resume` | The token from an earlier reply, to take that session back. |
| `name` | The session's name in the bridge's logs. |
| `probe_interval_ms` | How often the bridge pings. Default 250, minimum 10. |

```json
{"op":"reply","seq":2,"ok":true,"proto":14,"webrtc":true,"resumed":false,
 "resume_token":"9f...","grace_ms":10000,
 "interfaces":[{"addr":"192.168.1.20","mask":"255.255.255.0"}],"group_binds_any":true}
```

`interfaces` are the host's IPv4 interfaces, dotted. `group_binds_any` says the host hears a
multicast group only on a socket bound to any address (POSIX), which the net object's
`loopbackGroupBind` passes on. Absent means false.

## Finding bridges

Every bridge with `advertise` on (the default) speaks mDNS on the interfaces it listens on:

- It advertises `<host>._rant-bridge._tcp.local` through DNS-SD, with SRV to its port and TXT
  `v=<version>` and `name=<host>`. A second bridge on one host adds its port to the name.
- It answers the shared name `rant-bridges.local` with its own address, so a client reaches
  whichever bridge answers first, with no bridge chosen to serve the list.
- It browses for the others at start and every minute, keeps those heard in the last three
  minutes, and says goodbye when it stops.

`{"op":"bridges","seq":1}`, before or after `open` and with no passphrase, answers
`{"bridges":[{"name","url","version","self"}]}`: this bridge once per interface, then the
others. The list is no secret, since mDNS tells anyone on the network. `findBridges(url)`
beside `TunnelNet` asks it, `ws://rant-bridges.local:7480` by default.

## Sockets

Sockets are numbered by the client, 1 to 65535, at most 16 at once.

| Op | Keys | Reply |
|---|---|---|
| `udp_bind` | `sock`, `port` (0 for any), `if` (dotted, default any), `reuse` | `{port}`, the bound port. A bound `sock` is bound again. |
| `udp_member` | `sock`, `group`, `if`, `join` (default true) | `{}` |
| `udp_option` | `sock`, any of `ttl`, `loop`, `own_groups` | `{}` |
| `udp_close` | `sock` | `{}` |

Every op may run twice with the same result, so a client repeats what a dropped connection
left unanswered.

## Datagrams

Datagrams travel batched both ways: one binary message carries one or more, up to 60000
bytes in all.

```
[u8 9][u8 flags]  then per datagram  [u16 sock][4 ip][u16 port][4 via][u16 len][payload]
```

- `ip` is the destination toward the bridge and the source toward the client, in network
  byte order. `via` is the interface to send a group datagram out of, 0 for the default,
  and 0 toward the client. `flags` is 0.
- A side batches what it sends in one go: the client what one task sends, the bridge what
  one wake of its sockets drained.
- A batch that finds more than 4 MB waiting on its carrier is dropped, as a full socket
  drops datagrams.
- The bridge sends only to 239/8, to loopback and to addresses on its host's subnets, so it
  relays nothing beyond the mesh.

## Liveness and resume

- The bridge sends a PING every probe interval and the client answers each with a PONG.
  Any message from the client counts as heard. Silence longer than the bridge's
  `detach_ms` closes the socket with 4008, and the session starts its grace period.
- A dropped connection leaves the session and its sockets bound for `grace_ms`. Datagrams
  for a detached client are dropped. Reconnecting with `resume` takes the session back
  (`resumed: true`). Each reply carries a fresh token.
- When the session is gone (`resumed: false`) the new one has no sockets. The client binds
  each again to the port its node knows, with its options and groups, then binds new
  spares.

PING and PONG are binary messages, little endian:

```
PING  [u8 7][u32 seq][u32 us][u32 jitter_us][u32 samples][u32 missed]
PONG  [u8 8][u32 seq]
```

A PING carries what the bridge measures of the link: the last round trip, its jitter, the
samples so far and the probe intervals gone unanswered. A PONG answers it with its `seq`.

| Close code | Why |
|---|---|
| 1009 | A message over the bridge's `max_message_bytes`. |
| 4001 | The open was refused: wrong protocol or passphrase. |
| 4003 | `origin not allowed`. |
| 4008 | `no answer to pings`. |

## WebRTC

WebRTC is optional and off when the bridge says `webrtc: false`. The WebSocket carries
everything until it is up.

1. `{"op":"rtc"}` starts a peer connection. The reply has `ice_servers`
   (`[{urls, username, credential}]`).
2. The client sends its offer, `{"op":"rtc","sdp":"..."}`, and the reply has the answer
   `sdp`. The client always offers and the bridge answers as the DTLS client: Chrome
   fragments its ClientHello, which the bridge's DTLS server would refuse.
3. Candidates trickle both ways: `{"op":"rtc","candidate":"...","mid":"..."}` from the
   client, `rtc` pushes from the bridge.
4. State changes arrive as `{"op":"event","event":"rtc","state":"..."}`.

Channels are negotiated, so neither side announces them. Channel 0 is reliable and ordered
and carries PING and PONG. Channel 1 is unordered with no retransmits and carries batches.
Batches ride channel 1 while it is open, else the WebSocket, never both at once, so
datagrams keep their order. WebRTC does not survive a dropped connection: the client
negotiates it again.

## The client

`TunnelNet.connect(url, opts)` resolves once the WebSocket session is open and the spares
are bound, and tries WebRTC behind it, giving up after `rtcTimeoutMs`. A request with no
reply in 10 s fails. A socket op made while the link reconnects waits for it, and one the
drop left unanswered is sent again. Events report `resumed`, `restored` (a lost session set
up again), warnings and errors.

## The native client

`opts.net.bridge` runs a node of the C library on a tunnel, one per node.
`src/tunnel/core.c` is the sans IO part: SHA-1 and base64 for the handshake, frames, a
JSON reader and batch records. `src/tunnel/runtime.c` is the client. Discovery and the
node reach every socket through `i_rant_net_*`, which goes to the OS or to the node's
tunnel.

- A tunnel's socket is a handle below `RANT_SOCK_BAD`, so one poll set mixes them with the
  OS's, such as the node's waker. The poll waits on the WebSocket and the OS sockets, and
  a socket is ready when its queue holds a datagram.
- Datagrams a node sends between two flushes leave as one batch. The node flushes at the
  end of each TX drain. A backlog over 4 MB makes a send would block, as a full socket.
- The open is the one blocking step: it connects, asks for the challenge, opens the
  session and binds each socket, since a node needs its ports at once. Other socket ops are sent and not waited
  for. A refused one fires `RANT_E_BRIDGE`.
- The link is a state machine stepped by the poll: connect, handshake, challenge, open,
  open. A
  reconnect never blocks a poll, except the OS's name lookup. It backs off from 250 ms to
  5 s, resumes with the token, and binds every socket again when the session was lost.
  Six seconds without a byte from the bridge count as a drop, since it pings.
- WebSocket only, `ws://` only: no WebRTC and no TLS in the library. `src/tunnel/core.c`
  carries SHA-256 for the proof, as `bindings/web/src/proof.ts` does for a page served over
  http, which has no WebCrypto.
- `RANT_TUNNEL` builds it, on with the bundled platform layer, off under
  `RANT_PLAT_CUSTOM` or `RANT_NO_TUNNEL`. The platform adds a nonblocking TCP connect,
  send and recv, and `RANT_POLLOUT`.
