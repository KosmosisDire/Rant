# Explorer

`RANT/Rant Explorer` is the debugger UI: a real Rant node, not a passive sniffer, with a
Nodes tab and a Topics tab, greyscale plus one teal accent, dark and light. Clay (v0.14)
does the layout, since the design is CSS flexbox and Clay is a flexbox engine. SDL3 with
SDL3_ttf renders through FreeType, whose hinting keeps text crisp. CPM builds SDL3 and
SDL3_ttf static, clay.h and nanosvg are downloaded.

## Build

`Rant Explorer/CMakeLists.txt` takes Rant through CPM at the release version it pins, so it
never reaches into this checkout. Its `windows-local` and `linux-local` presets set
`CPM_rant_SOURCE` to `../Rant` to build against the working tree. It links `rant::rant` and
`rant::rant_platform`, never `rant_host`, since `net_capture.c` compiles its own transport
only amalgamation with `RANT_NO_SHM`. It includes `tools/static_runtime.cmake` from the
fetched source before SDL3, so SDL3 and vendored freetype share the static runtime. The
SDL3_ttf release tarball omits vendored freetype, so it is fetched by git with
`GIT_SUBMODULES external/freetype` and `SDLTTF_VENDORED ON`. We own `main()`:
`SDL_MAIN_HANDLED`, `<SDL3/SDL_main.h>`, `SDL_SetMainReady()` before `SDL_Init`. After an
announce overlay version bump the explorer cannot see nodes until its pin moves to a
release that carries the bump.

## Architecture

Two translation units. `net_capture.c` is the Rant side and owns the amalgamation and
winsock. `discovery_explorer.c` is the UI. `net_capture.h` is plain types only, so a
headless test can `#include "net_capture.c"` and drive `cap_start`, `cap_subscribe` and
`cap_poll` against real nodes. Headers: `ui_render.h` (Clay to SDL3 renderer, LCD text,
texture cache and pool), `ui_theme.h`, `ui_fonts.h`, `ui_model.h` and `ui_data.h`
(CapSnapshot to Dataset), `ui_tree.h`, `ui_icons.h`, `ui_app.h`, `ui_widgets.h`,
`ui_textbox.h`, `ui_tab_*.h`, `ui_shell.h`.

Threading: `cap_start` runs `rant_node_start` and `cap_poll` calls `rant_node_dispatch`
per frame, so message callbacks land on the UI thread, but `cap_on_event` still fires on
the service thread. UI reads of shared state bracket with `rant_node_lock` and
`rant_node_unlock`. That bracket is not nestable and refuses create or set_role while
held, so bracketed code never calls another bracketing helper and never logs. Topics are
queue enabled by a `rant_topic_dispatch` right after create.

The send path match wait is disabled (`match_wait_ms = -1`, never stall the UI). The
async form is a per topic pending slot: park the payload while `rant_topic_ready` is 0,
flush on resolve or drop loudly after 3 s, with an amber SENDING chip.

The explorer crafts every pattern channel raw, never through handles. The per peer demux
binds a name to one local topic, so a handle beside raw twin channels would shadow one.

Schema re adoption: `cap_schema_poll` compares an entity's mesh generation with the one
the subscription adopted, retires the channels and re runs `cap_sub_reconcile` when it
moved. Triggered by PEER_UP, PEER_DOWN, PEER_INTEREST, SCHEMA_MISMATCH and a 1 s fallback.
Observers key cached reflection walks on the per peer interest epoch alone.

Meta: the watched node's `@rant/meta` is polled once per second with a directed call. The
poll sends a section mask: node, proc and peers every time, topics every fifth poll (the
section is about 250 B per topic). The log sidebar drains the three `@rant/log` topics,
widened to PUBSUB at `cap_start`, into a 512 line ring.

The topic list VALUE column renders the newest value per standard type (Color swatch,
Matrix heat grid, else text). A bare root shows its single value, raw bytes show their
printable head, a plain struct stays blank. Larger previews are deferred.

## Capture layer

- The node's topic reserve is a starting reserve, not a ceiling: the node grows
  `max_topics` on demand, so subscribing to thousands of topics relocates the reserve and
  never refuses. `CAP_MAX_SUBS` matches the announce topic ceiling of about 13000. A
  CapSub is about 300 B, so the fixed table is about 4.8 MB, and a feed ring is allocated
  on the first message, since 512 eager rings would reserve about 275 MB.
- An observed topic owns up to two Rant topics, one best effort and one reliable, created
  on demand with exactly one live at a time, and the live one's role is the union of what
  the explorer wants, since two live topics of one identity would misroute.
  `cap_sub_reconcile` keeps it in sync. Publishers offer reliable by default, because a
  reliable offer satisfies every subscriber and reliability arms retention so a first
  publish racing the match replays. An existing subscription pins the choice.
- A typed topic adopts the schema its advertisers carry, so publishes reach typed readers.
  The topic then matches only that schema, and the drawer flags disagreeing publishers.
- A variable's set channel has no retention. It is multi writer and replay preserves only
  per writer order, so retained ops from several writers would reach a rematching owner
  in arbitrary order and a stale write could win. An op racing the owner match parks in
  the pending send slot instead.
- Function replies arrive on the service thread with the node lock held and park in a per
  topic slot. `cap_poll` drains them into the feed ring under the same lock. Task
  progress off the raw tap updates the call state and the run table, bracketed with the
  node lock, and never enters the feed ring, which stays the call log. Run rows age out
  30 s after their last update, and an own call still waiting for its first response
  times out after 5 s.
- The feed's timestamp is the writer's `written_us`, taken at the commit point, so a
  repaired, replayed or queued message reads as when it was written, shown as a plain
  time of day so a replayed message legitimately reads older than the observer and a
  skewed host reads shifted. Recency and jitter use the arrival stamp on the node's
  monotonic clock, which never runs backwards. A message with no stamp shows our own
  arrival mapped through two clock origins taken from the node at `cap_start`, marked
  as arrived rather than written.
- Rate is counted over an adaptive wall clock window on the performance counter, since
  GetTickCount64 quantizes to about 15.6 ms and snapped a steady 250 Hz to two values.
  The window closes after enough messages or a maximum time, so a silent topic decays to
  0. Jitter tracks the whole lifetime: an EWMA of the inter arrival interval is the
  expected spacing, and each deviation feeds an online p90 estimator on the pinball loss
  (up by step times 0.9 when a sample meets the estimate, down by step times 0.1
  otherwise) with the step scaled by the mean, so no sample history is kept.
- The snapshot is never memset. Its per node entity buffers are high water heap growth
  reused across frames, every used slot is fully overwritten, and counts delimit the
  reader. The reflection walk per peer is O(topics), so it runs only when the peer's
  interest epoch moved. Names are copied with memcpy, not snprintf, since thousands of
  snprintf calls per frame were slow.
- The publish form parses each value against the advertised schema through the same
  setter the send uses, so validation is the authoritative accept test with no side
  effects. A variable's form is seeded from its live value, re spelled in form syntax by
  the exact inverse of the parser, and a field that cannot round trip stays empty rather
  than sending junk. The seed retries every frame until the value lands and stops on a
  user edit.
- The meta poll asks for the node, proc and peers sections every time and the topics
  section every fifth poll, since that section is about 250 B per topic and was hundreds
  of KB per second on a node advertising thousands of topics.
- The pattern interop block is the one place that knows the wire constants, mirroring the
  channel kinds and prefixes of src/patterns/core.c.

## UI details

- The CMake step patches Clay: its scroll container table is hard coded to 10 entries and
  every clip container counts, so a form with a few text boxes overflowed it, and
  `Clay_UpdateScrollContainers` dereferenced a null clip config for a transient container
  that vanished this frame. Both patches are idempotent.
- Clay's arena is sized to an element ceiling of 256k, about 215 MB, because the topic tree
  and the feed are virtualized and the one large consumer is a selected node's endpoint
  list at about 5 elements per row. One million elements would cost about 800 MB.
- The feed is a table: columns are the flattened fields, rows are samples, and the header
  is a sibling above the scroll body sharing the column widths. A function feed
  interleaves calls and replies with different schemas, so the columns are the union of
  both field sets tagged by group and prefixed req. and rsp., and a row fills only its
  own group. Column templates come from a sample matching the advertised schema, not the
  newest sample, so a subset publisher never narrows the columns. Cells truncate rather
  than clip, since a per cell clip would exhaust Clay's scroll container slots. Column
  widths stretch to a width derived from stable references, since measuring the feed's
  own laid out width pins it wide and it could never shrink.
- The tree panel width is measured across the whole tree, ranking candidates by label
  length plus indent and measuring only the few longest, since TTF measuring thousands of
  labels is too slow. Rows are virtualized: only rows near the viewport become elements
  and fixed height spacers stand in for the rest, with ids keyed to the global index.
- Reliability rollups come from the announced interest: `reliable_recommend` is the
  reliability to subscribe as, reliable only if every publisher offers it, and the
  displayed badge takes the publishers' value when present and the subscribers' otherwise.
  The explorer's own publish counts as a publisher.
- The text cache is keyed by font, color and string bytes with open addressing and no
  tombstones, since the sweep rebuilds the table wholesale. The pool's free list must
  absorb a whole sweep's releases, so it is sized to message rate times idle window. A
  geometry scratch pair serves every rounded rect and arc, since the fillers never nest.
- The scrollbar runs in two phases: a pre pass at the top of the frame over last frame's
  bars handles drag and paging and consumes the press before any row sees it, then each
  container site registers its bar and draws it.
- The text box coalesces same kind edits at the caret within 900 ms into one undo step,
  typing, backspace and delete separately, and a paste never coalesces. Its right click
  menu runs before the box lays out its lines, since Clay keeps views into the buffer. The
  dropdown turns its box into a search field while open, and one shared search buffer
  serves every dropdown since one is open at a time.
- The logo's D glyph was outlined to a path with fontTools because nanosvg has no text
  engine. Icons rasterize at a supersample and SDL shrinks them, since shrinking always
  anti aliases.

## Gotchas

- Never `return` out of a `CLAY({...}) {...}` block. The macro is a for loop that closes
  the element in its increment clause, so an early return segfaults in `Clay_EndLayout`.
- Never use `Clay_GetScrollOffset()` for a clip's `childOffset`. It matches by element
  pointer, which shifts when an earlier element appears or disappears, so the container
  flickers to the top. `ui_scroll_offset(id)` matches by id.
- Selection is keyed by identity, never a bare index. The Dataset is rebuilt and may
  reorder every frame. AppState keeps `sel_topic_path` and `sel_node_id` and re resolves.
- The topic tree is collapsed by default. AppState stores an expanded set and
  `app_expand_to(path)` reveals a by name selection.
- Clay does not copy strings. `ui_fmt()` into a per frame pool, `ui_str()` for persistent.
- Never add a per frame texture creation path. `SDL_CreateProperties` takes ids from a
  global 32 bit counter that is never recycled and spins forever at saturation (still true
  in SDL 3.2.16). A texture per text draw exhausted it in about a day. The fix needs both a
  cache keyed by (font, color, string) for text that repeats and a size classed pool that
  overwrites borrowed textures with `SDL_UpdateTexture` for text that does not (a message
  feed mints unique strings). The free list must absorb message rate times idle window:
  keep 1024 per class, 8 MB per class, idle 90 frames, sweep every 30. Watch `g_pool_made`
  plateau. Call `ui_text_cache_clear()` on a DPI reload and `ui_render_shutdown()` before
  `SDL_DestroyRenderer`.
- LCD text goes through the surface API drawn 1:1 integer aligned. The background per run
  is the composite of every painted rect under the text centre, translucent ones included.
- Sizes are the mockup's CSS px times `ui_scale` times `ui_dpi`. One TTF_Font per (family,
  weight, size), never `TTF_SetFontSize` in the measure callback. Fonts are OS system fonts
  by absolute path.
- Icons are Lucide SVGs rasterized once by nanosvg. Fetch the real svg, never reconstruct
  a path from memory.
- `ui_textbox.h` is the text input widget with one global focus slot. Code that rewrites a
  field's buffer must call `ui_tb_reset`.
- Screenshots from a shell need the window foregrounded first. `F12` toggles the Clay
  inspector. `RANT_UI_TAB` and `RANT_UI_TOPIC` deep link. `RANT_UI_FPS=1` prints fps.
- `demo_scene.c` spins up the sample mesh, one node per process, since several discovery
  participants in one process share the port and come up nameless. `run_demo.ps1` drives
  it. Pace a test publisher by wall clock, since `rant_node_poll` returns early under
  traffic.
