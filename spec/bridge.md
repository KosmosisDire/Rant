# WebSocket bridge

`bridge/` builds `rant_bridge`, a C++17 server where one WebSocket connection is one full
Rant node, built on `dist/rant.hpp`. `bridge/PROTOCOL.md` is the wire spec (v11) and must
stay in sync with `bridge.cpp`. Scope is firm: pub/sub, the patterns and pull only
introspection (peers, entities, mesh, meta). No retire op, no auth, no TLS, not a mesh
debugger.

## Shape

- One `create` op with a `kind`. The entity id is client chosen (1 to 65534) and doubles
  as the WebRTC data channel id, so both ends open the channel with no round trip. Frames
  for an in flight create are held client side and replayed. Schema blocks come back under
  the request key (`schema`, `req`, `prg`, `rsp`), each `{name, size, hash, fields}`.
- One 29 byte frame header for every op both ways:
  `[op][flags][u16 id][u32 seq][u32 peer][u64 written_us][u64 capture_us][u8 text_len][text][payload]`,
  ops DATA, VAR, CALL, RESULT, PROGRESS, CANCEL. The caller name and the result message
  ride the text slot. The `match` push is `{id, count, ready}`. A topic's count is the
  publisher side matched readers, so a sub only topic reads 0.
- WebRTC: the WebSocket opens first and carries control and signaling. Data channels carry
  frames, unordered with no retransmits for best effort and reliable ordered otherwise.
  The WebSocket is the per frame fallback. Best effort frames drop past 1 MB buffered and
  are reported as `msg_lost`. A reliable backlog past `--max-buffered` closes the client.
- Video tracks: H264, H265 and AV1 only. Payload types are the browser's from its offer.
  The keyframe gate is the flag or an IDR NAL. The RTP timestamp is pts us times 9/100.
  MJPEG, Image and no track stay frames. Tracks go bridge to browser only.
- Media is not an entity kind. A VideoFrame or Image may sit at a topic root, nested in a
  sample, in a variable, in task progress or a call result. The client's `VideoView` is a
  sink: `push(value)` from anywhere, or `attach(source, path)` binds it to an entity's
  stream by dotted field path. Over WebRTC the offer's `tracks` map is
  `{mid: {id, path, call, keep_data}}`. The bridge validates the field is a VideoFrame,
  stamps its SSRC, and `media_route` packetizes that field and forwards the message with
  the field's tail frame emptied. An `ExternalVideoStream` value makes the view follow
  the URL (WHEP, MJPEG img, HLS). The view's MediaStream carries exactly one track at a
  time, so a bound but silent track never stays in it.
- Reflection: `reflect: true` on any create takes schemas from the mesh, plus `refresh`,
  `mesh` and `mesh_find` ops.
- Variable updates are pushed off the `on_change` hook, no polling.
- `log` and `log_subscribe` ops exist. Meta is deliberately not exposed, though the bridge
  node still hosts `@rant/meta`.

## The DTLS role trap

Chrome's DTLS ClientHello is fragmented (its post quantum key share) and MbedTLS as a DTLS
server refuses fragmented ClientHellos. So the client always offers and the bridge answers
`active`, the DTLS client. Signaling: a bare `{op:"rtc"}` returns `{ice_servers}`, and
`{op:"rtc", sdp, tracks}` returns `{sdp, tracks}` with per mid verdicts. Candidates trickle
both ways. The client creates negotiated channel 0 as the SCTP anchor. Do not move the
offer back to the bridge.

## Build

libdatachannel v0.23.2 with its submodules plus MbedTLS 3.6.4 via FetchContent, behind
`RANT_BRIDGE_WEBRTC`. `bridge/cmake/FindMbedTLS.cmake` shadows the finders the fetched
projects ship. `mbedtls_user_config.h` enables DTLS SRTP. Flags: `--ice`, `--rtc-ports`,
`--mtu` (default 1200, since libdatachannel's 1280 failed on a VPN path), `--no-webrtc`,
`--webrtc-debug`. No bridge lock or poll thread: node handlers send on the service thread,
libdatachannel callbacks reach the connection through a weak pointer, and close tears
WebRTC down first, then the node. A background `rant_bridge.exe` blocks the link of a
rebuild. Tests run `example.mjs` over real data channels with the `node-datachannel`
polyfill, or under headless Chrome.
