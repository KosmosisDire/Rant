// The Emscripten library under platform.c: its socket imports go to the net object the host
// sets as Module.rantNet before opening a node, and the driver runs the app's drives.
// spec/web.md says what a net object must do.

mergeInto(LibraryManager.library, {
    $RantWeb__deps: ['malloc', 'rant_web_deliver'],
    $RantWeb: {
        drives: [],
        sink: null,          // this instance's, so one net object can serve several
        running: false,
        // -1 for none in both, since Emscripten writes this object out and Infinity as null
        wait: -1,            // the shortest wait asked for during the current run
        due: -1,             // when the next run is scheduled, performance.now() time
        token: 0,
        port: null,          // a MessageChannel for runs that should not wait on a timer
        later: [],
        micro: false,        // a run is queued as a microtask

        net() {
            const net = Module['rantNet'];
            if (!net) throw new Error('rant: set Module.rantNet before opening a node');
            return net;
        },

        // A datagram from the net object. The bytes are copied into the heap, where the
        // platform takes them over.
        deliver(id, naddr, port, bytes) {
            const p = _malloc(bytes.length || 1);
            HEAPU8.set(bytes, p);
            _rant_web_deliver(id, naddr, port, p, bytes.length);
            RantWeb.soon();
        },

        // A run right after the task that delivered, before any other task, so a datagram is
        // handled with no hop through the event loop. One run serves a whole batch.
        soon() {
            if (RantWeb.micro) return;
            RantWeb.micro = true;
            queueMicrotask(() => {
                RantWeb.micro = false;
                RantWeb.token++;      // a run now makes the scheduled one moot
                RantWeb.run();
            });
        },

        // Runs the drives no later than ms from now. Zero yields once, with no timer clamp.
        kick(ms) {
            const at = performance.now() + ms;
            if (RantWeb.due >= 0 && at >= RantWeb.due) return;
            RantWeb.due = at;
            const token = ++RantWeb.token;
            const run = () => { if (token === RantWeb.token) RantWeb.run(); };
            if (ms > 0) { setTimeout(run, ms); return; }
            if (!RantWeb.port) {
                const ch = new MessageChannel();
                ch.port1.onmessage = () => RantWeb.later.shift()?.();
                RantWeb.port = ch.port2;
            }
            RantWeb.later.push(run);
            RantWeb.port.postMessage(0);
        },

        waited(ms) {
            if (RantWeb.wait < 0 || ms < RantWeb.wait) RantWeb.wait = ms;
        },

        run() {
            RantWeb.due = -1;
            RantWeb.wait = -1;
            RantWeb.running = true;
            try {
                for (const d of RantWeb.drives) {{{ makeDynCall('vp', 'd.fn') }}}(d.user);
            } finally {
                RantWeb.running = false;
            }
            if (RantWeb.wait >= 0) RantWeb.kick(RantWeb.wait);
        },
    },

    rant_web_drive__deps: ['$RantWeb'],
    rant_web_drive: (fn, user) => {
        RantWeb.drives.push({ fn, user });
        RantWeb.kick(0);
    },

    rant_web_wake__deps: ['$RantWeb'],
    rant_web_wake: (ms) => {
        if (RantWeb.running) RantWeb.waited(ms);
        else RantWeb.kick(ms);
    },

    rant_net_wait__deps: ['$RantWeb'],
    rant_net_wait: (ms) => RantWeb.waited(ms),

    rant_net_open__deps: ['$RantWeb'],
    rant_net_open: () => {
        RantWeb.sink ??= { deliver: RantWeb.deliver };
        try { return RantWeb.net().open(RantWeb.sink); }
        catch (e) { err(String(e?.message ?? e)); return -1; }
    },

    rant_net_bind__deps: ['$RantWeb'],
    rant_net_bind: (id, ifNaddr, port, reuse) => RantWeb.net().bind(id, ifNaddr >>> 0, port, !!reuse),

    rant_net_option__deps: ['$RantWeb'],
    rant_net_option: (id, what, value) => {
        const net = RantWeb.net();
        switch (what) {
        case 1: net.setMulticastInterface(id, value >>> 0); break;
        case 2: net.setMulticastTTL(id, value); break;
        case 3: net.setMulticastLoopback(id, !!value); break;
        case 4: net.ownGroupsOnly(id); break;
        case 5: net.setReceiveBufferSize?.(id, value); break;
        case 6: net.setSendBufferSize?.(id, value); break;
        }
    },

    rant_net_membership__deps: ['$RantWeb'],
    rant_net_membership: (id, group, ifNaddr, join) =>
        RantWeb.net().membership(id, group >>> 0, ifNaddr >>> 0, !!join) ? 1 : 0,

    rant_net_send__deps: ['$RantWeb'],
    rant_net_send: (id, buf, len, naddr, port) =>
        RantWeb.net().send(id, HEAPU8.subarray(buf, buf + len), naddr >>> 0, port),

    rant_net_close__deps: ['$RantWeb'],
    rant_net_close: (id) => RantWeb.net().close(id),

    rant_net_interfaces__deps: ['$RantWeb'],
    rant_net_interfaces: (out, max) => {
        const list = RantWeb.net().interfaces().slice(0, max);
        list.forEach((f, i) => {
            HEAPU32[(out >> 2) + 2 * i] = f.addr >>> 0;
            HEAPU32[(out >> 2) + 2 * i + 1] = f.mask >>> 0;
        });
        return list.length;
    },

    rant_net_loopback_group_bind__deps: ['$RantWeb'],
    rant_net_loopback_group_bind: (naddr) => {
        const net = RantWeb.net();
        return (net.loopbackGroupBind ? net.loopbackGroupBind(naddr >>> 0) : naddr) >>> 0;
    },

    rant_net_hostname__deps: ['$RantWeb', '$stringToUTF8'],
    rant_net_hostname: (buf, cap) => stringToUTF8(RantWeb.net().hostname(), buf, cap),
});
