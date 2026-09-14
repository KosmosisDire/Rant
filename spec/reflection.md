# Reflection

How the node builds the views behind `ramble_node_peers_next`, `ramble_node_entities_next`
and `ramble_node_mesh_next`. The API is in docs/reflection.md.

## Tables, not scans

The node fills tables as announces and details arrive. Every detail received feeds them,
and `opts.fetch_details` only widens what an observer asks for. A walk never hashes a
name, rescans an interest list or allocates. The per peer fold and the mesh table are
rebuilt lazily after a change, and `ramble_node_mesh_epoch` bumps when a walk should run
again. Each peer carries an `epoch` that bumps on every reflected change.

Entity kinds ride the announce, so the walks work with or without the patterns layer.

## Folding channels into entities

An entity is never a channel. A function's `@req` and `@rsp` pair is one function, a
task's three channels are one task, a variable's `@set` folds in as `writable`, and the
`@ramble/` builtins are hidden. Partners are found by hashing the base name plus the suffix
and verified against the fetched name when it is known, since 32 bit hashes collide. A
pattern primary missing a partner, or a partner missing its primary, is surfaced
`incomplete`. The per peer tables are hook allocated and freed when the peer goes.

## The mesh entity

The mesh entity's schemas are the PROVIDER's, because a definition owns its type. The
widest consumer declaration stands in when no provider is live. Records sort by kind, name
id and peer. The first live provider takes the slot, and a rival with a different schema
marks a conflict and wins only if it was heard more recently. `conflict` is set when
live endpoints declare schemas that cannot read each other. `generation` changes iff the
provider's uuid, any of the three schemas, or an attr changed, so "retire and re create"
is one compare. It is an FNV hash of the provider uuid, the three schema hashes, the attrs,
whether a set channel exists, and the kind.

## Adopting a schema

`reflect_from_mesh` picks per channel and per role. A reader (a subscriber, a caller's
`@rsp`, an accessor's value) takes the provider's exact declaration. A writer (a
publisher, a caller's `@req`) takes the WIDEST live declaration, since named types narrow
one way (an anonymous struct reads a `Color` writer, but a `Color` reader refuses an
anonymous writer) and the widest is what every reader accepts. Untyped when nobody
advertises one. A value you pass always wins, and a zero reliability follows the mesh.

`refresh` re reads the mesh and, when the generation moved, re types the handle in place:
same handle, same index, peers re verify, outstanding calls answered CANCELLED first. It
is never automatic because a retire is visible to peers.

## Dropped peers

A DROPPED peer stays in the peer walk and its last known entities are served as a ghost
view, but the mesh fold skips it. Its cached entities are the dead incarnation's and would
stand beside a restarted live one. The `ghost:` selftests pin it.

## Limits

There is no public subscriber side match count. `ramble_topic_match_count` is publisher
side, so a sub only topic reads 0, and the bridge's `match` push reports the same. The
entity name is a placeholder until details arrive, and `incomplete` says so. The old per
name schema ranking (`ramble_node_mesh_schema`) is gone: the provider rule plus writer side
width covers the case that forced it, a caller adopting a fellow caller's anonymous struct
over the definition's `Color`.
