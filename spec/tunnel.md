# The tunnel

A node that cannot open UDP sockets, Rant in a page, borrows them from a bridge: the bridge
binds sockets on its host and forwards datagrams both ways, so to the mesh the node is a
node of that host. The bridge reads none of them. `bindings/web/src/tunnel.ts` (`TunnelNet`)
is the client. `RANT/bridge-rant` is the server, and this file is the protocol it serves to
a tunnel session.

## Connection

1. The client opens `ws://host:7480`, or `wss://` behind a TLS proxy.
2. With `allowed_origins` set, a browser whose `Origin` is not listed is closed with 4003.
3. The client sends `open`. Nothing else is accepted before it.
4. The bridge replies, then pings at the probe interval.

Text messages are JSON. A request carries `op` and a client chosen `seq`, and its reply is
`{"op":"reply","seq":N,"ok":true,...}` or `{"op":"reply","seq":N,"ok":false,"error":"..."}`.

```json
{"op":"open","seq":1,"proto":12,"tunnel":true,"passphrase":"...","name":"page"}
```

| Key | Meaning |
|---|---|
| `proto` | 12. Any other is refused and the socket closed with 4001. |
| `tunnel` | true, else the bridge opens a node of its own for the session. |
| `passphrase` | Must match the bridge's. A wrong one is refused after 1 s and closed with 4001. |
| `resume` | The token from an earlier reply, to take that session back. |
| `name` | The session's name in the bridge's logs. |
| `probe_interval_ms` | How often the bridge pings. Default 250, minimum 10. |

```json
{"op":"reply","seq":1,"ok":true,"proto":12,"webrtc":true,"resumed":false,
 "resume_token":"9f...","grace_ms":10000,
 "tunnel":{"interfaces":[{"addr":"192.168.1.20","mask":"255.255.255.0"}],"group_binds_any":true}}
```

`interfaces` are the host's IPv4 interfaces, dotted. `group_binds_any` says the host hears a
multicast group only on a socket bound to any address (POSIX), which the net object's
`loopbackGroupBind` passes on. Absent means false.

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

PING and PONG are binary messages with a 29 byte header, the frame of the bridge's other
sessions:

```
[u8 op][u8 flags][u16 id][u32 seq][u32 peer][u64 written_us][u64 capture_us][u8 text_len][text]
[u16 msg][u32 total][u32 offset]     only when flags bit 7 is set
[payload]
```

A PING is op 7, its payload CBOR link stats `{us, jitter_us, samples, missed}` (the round
trip the bridge measures, its jitter, and probe intervals gone unanswered). A PONG is op 8:
the ping's header and text with the op changed, bit 7 cleared and no payload.

| Close code | Why |
|---|---|
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
