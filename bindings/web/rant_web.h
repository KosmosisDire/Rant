/* Rant in a page or in Node.js, built to wasm with Emscripten. The node keeps every rule of the
 * C library. Its sockets are a net object the host supplies as Module.rantNet: the bridge
 * tunnel in a browser, real UDP in Node.js. spec/web.md has the contract. */
#ifndef RANT_WEB_H
#define RANT_WEB_H

#ifdef __cplusplus
extern "C" {
#endif

/* A page runs one thread and never blocks, so the app drives its nodes: fn runs now, again
 * when a datagram arrives, and again when the wait a node asked for in its last poll ends.
 * fn polls each node with a long timeout, which returns at once. Several drives may run. */
void rant_web_drive(void (*fn)(void *user), void *user);

/* The next drive comes no later than ms from now, for a timer of the app's own. */
void rant_web_wake(int ms);

#ifdef __cplusplus
}
#endif
#endif /* RANT_WEB_H */
