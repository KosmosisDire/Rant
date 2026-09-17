# Reflection

Three walks over one zero initialized `RantIter` answer what a tool asks about the mesh,
with or without the patterns layer.

```c
RantIter it = {0};
RantPeerInfo peer;
while (rant_node_peers_next(n, &it, &peer)) ...

RantIter it2 = {0};
RantEntityInfo e;
while (rant_node_entities_next(n, RANT_SELF, &it2, &e)) ...         /* what this node offers */

RantIter it3 = {0};
while (rant_node_mesh_next(n, &it3, &e)) ...                      /* the whole mesh folded */
```

- `rant_node_peers_next` lists who is here as a `RantPeerInfo`: id, uuid, name, address,
  liveness, last heard time, an `epoch` that bumps on every change, fragment size, and
  the measured round trip. A dropped peer is still listed, so gate on `.liveness`.
- `rant_node_entities_next(n, peer, ...)` lists what one node offers, `RANT_SELF` for this
  one. A dropped peer's last known view is served as a ghost.
- `rant_node_mesh_next` and `rant_node_mesh_find(n, kind, name, &e)` fold the whole mesh:
  one `RantEntityInfo` per kind and name across every active peer and this node.

An entity is never a channel. A function's request and response pair is one function, a
task's three channels are one task, a variable's set channel folds in as `writable`, and
the `@rant/` builtins are hidden.

`RantEntityInfo` carries the kind, name and hash, who provides and consumes it and how
many, reliability, the attrs (`writable`, `forceable`, `cancellable`, `exclusive`,
`multi`), the schemas (`schema`, `rsp_schema`, `progress_schema` with their hashes),
`provider` and `from` (the providing peer), `incomplete` (details still arriving),
`conflict` (live endpoints declare schemas that cannot read each other) and `generation`.

The schemas are the provider's, since a definition owns its type. The widest consumer
declaration stands in when no provider is live. `generation` changes exactly when the
provider's identity, any schema or an attr changed, so "retire and re create" is one
compare. `rant_node_mesh_epoch` says when to walk again.

Bracket the views with `rant_node_lock` and `rant_node_unlock` when a poller runs on
another thread. `opts.fetch_details` makes the node fetch names and schemas for every
topic every peer advertises, which observer tools need.

## Adopting the mesh's schema

A tool that carries no type of its own (the explorer, the bridge, a CLI publisher, an
HMI) passes `.reflect_from_mesh = 1` in any create's opts. Every NULL schema then takes
the entity's from the mesh and a zero reliability follows it. A value you pass always
wins. A reader takes the provider's exact declaration. A writer takes the widest live
declaration, which every reader accepts. Untyped when nobody advertises one.

`rant_topic_refresh`, `rant_function_refresh` and `rant_variable_refresh` re read the mesh
and, when the entity's generation moved, re type the handle in place: same handle, same
index, peers re verify, outstanding calls answered CANCELLED first. It is never
automatic, because a retire is visible to peers, so the app picks the moment.
`rant_schema_copy` gives an owned copy of any node owned schema.

The same walks are in every binding: C++ `Peer` and `Entity`, C# `RantPeer` and `RantEntity`
(docs/csharp.md), Python `Peer` and `Entity` on `node.reflection` (docs/python.md), and the
bridge's `peers` op.
