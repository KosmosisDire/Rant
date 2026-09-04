# Explorer

`explore/` is the debugger UI: a real DART node, not a passive sniffer, with a Nodes tab
and a Topics tab, greyscale plus one teal accent, dark and light. Clay (v0.14) does the
layout, since the design is CSS flexbox and Clay is a flexbox engine. SDL3 with SDL3_ttf
renders through FreeType, whose hinting keeps text crisp. FetchContent builds SDL3 and
SDL3_ttf static, clay.h and nanosvg are downloaded.

## Build

`explore/CMakeLists.txt` is dual mode, detected by `if(NOT TARGET dart)`: standalone, or
from the root with `-DDART_BUILD_EXPLORER=ON` (what the presets do), where it links the
root `dart` target and depends on `dist`. Build it from the root, since the root build is
MSVC and MSVC has no VLAs. The SDL3_ttf release tarball omits vendored freetype, so it is
fetched by git with `GIT_SUBMODULES external/freetype` and `SDLTTF_VENDORED ON`. We own
`main()`: `SDL_MAIN_HANDLED`, `<SDL3/SDL_main.h>`, `SDL_SetMainReady()` before `SDL_Init`.
Rebuild the explorer after any announce overlay version bump or it cannot see nodes.

## Architecture

Two translation units. `net_capture.c` is the DART side and owns the amalgamation and
winsock. `discovery_explorer.c` is the UI. `net_capture.h` is plain types only, so a
headless test can `#include "net_capture.c"` and drive `cap_start`, `cap_subscribe` and
`cap_poll` against real nodes. Headers: `ui_render.h` (Clay to SDL3 renderer, LCD text,
texture cache and pool), `ui_theme.h`, `ui_fonts.h`, `ui_model.h` and `ui_data.h`
(CapSnapshot to Dataset), `ui_tree.h`, `ui_icons.h`, `ui_app.h`, `ui_widgets.h`,
`ui_textbox.h`, `ui_tab_*.h`, `ui_shell.h`.

Threading: `cap_start` runs `dart_node_start` and `cap_poll` calls `dart_node_dispatch`
per frame, so message callbacks land on the UI thread, but `cap_on_event` still fires on
the service thread. UI reads of shared state bracket with `dart_node_lock` and
`dart_node_unlock`. That bracket is not nestable and refuses create or set_role while
held, so bracketed code never calls another bracketing helper and never logs. Topics are
queue enabled by a `dart_topic_dispatch` right after create.

The send path match wait is disabled (`match_wait_ms = -1`, never stall the UI). The
async form is a per topic pending slot: park the payload while `dart_topic_ready` is 0,
flush on resolve or drop loudly after 3 s, with an amber SENDING chip.

The explorer crafts every pattern channel raw, never through handles. The per peer demux
binds a name to one local topic, so a handle beside raw twin channels would shadow one.

Schema re adoption: `cap_schema_poll` compares an entity's mesh generation with the one
the subscription adopted, retires the channels and re runs `cap_sub_reconcile` when it
moved. Triggered by PEER_UP, PEER_DOWN, PEER_INTEREST, SCHEMA_MISMATCH and a 1 s fallback.
Observers key cached reflection walks on the per peer interest epoch alone.

Meta: the watched node's `@dart/meta` is polled once per second with a directed call. The
poll sends a section mask: node, proc and peers every time, topics every fifth poll (the
section is about 250 B per topic). The log sidebar drains the three `@dart/log` topics,
widened to PUBSUB at `cap_start`, into a 512 line ring.

The topic list VALUE column renders the newest value per standard type (Color swatch,
Matrix heat grid, else text). A bare root shows its single value, raw bytes show their
printable head, a plain struct stays blank. Larger previews are deferred.

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
  inspector. `DART_UI_TAB` and `DART_UI_TOPIC` deep link. `DART_UI_FPS=1` prints fps.
- `demo_scene.c` spins up the sample mesh, one node per process, since several discovery
  participants in one process share the port and come up nameless. `run_demo.ps1` drives
  it. Pace a test publisher by wall clock, since `dart_node_poll` returns early under
  traffic.
