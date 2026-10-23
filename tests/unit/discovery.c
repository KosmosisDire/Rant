/* The discovery core and the node core, sans IO: peer lifecycle, paths, and the node's
 * peer table over a real discovery core with no sockets. spec/testing.md says what each
 * case pins. */
#include "support/test.h"

/* the discovery core's callbacks, recorded for the lifecycle checks */
static uint32_t dc_up_id, dc_up_n, dc_down_id, dc_down_n, dc_refused_n, dc_wire_n;
static int      dc_down_reason;
static void dc_up(void *user, uint32_t peer, const RantAddr *addr, RantBytes meta){
    (void)user; (void)addr; (void)meta; dc_up_id=peer; dc_up_n++;
}
static void dc_down(void *user, uint32_t peer, i_RantDiscoveryDownReason reason){
    (void)user; dc_down_id=peer; dc_down_reason=(int)reason; dc_down_n++;
}
static void dc_error(const RantEvent *ev){
    if (ev->error==RANT_E_PEER_REFUSED) dc_refused_n++;
    if (ev->error==RANT_E_WIRE_VERSION) dc_wire_n++;
}

/* craft a datagram for sender `uid` (uuid = all uid bytes) carrying an empty blob into a
   RANT_DISCOVERY_WIRE_MAX buffer. flags RANT_DISCOVERY_FLAG_BYE for a bye */
static size_t dc_wire(uint8_t *p, uint8_t uid, uint8_t flags, uint16_t dom, uint16_t port,
                      const char *name, uint32_t mver){
    static uint8_t uuid[16];
    i_RantDiscoveryWire w;
    memset(uuid, uid, 16);
    memset(&w, 0, sizeof w);
    w.flags = (uint8_t)(flags | RANT_DISCOVERY_FLAG_BLOB); w.domain = dom; w.uuid = uuid;
    w.version = mver; w.data_port = port;
    w.name = name ? rant_cstr(name) : rant_string(NULL, 0);
    return i_rant_discovery_encode(&w, p, RANT_DISCOVERY_WIRE_MAX);
}
static size_t dc_mk(uint8_t *p, uint8_t uid, uint8_t flags, uint16_t dom, uint16_t port, uint32_t mver){
    return dc_wire(p, uid, flags, dom, port, NULL, mver);
}

/* feed one crafted datagram to a core as if received from (ip, sport) */
static void dc_feed(i_RantDiscoveryState *st, const uint8_t ip[4], uint16_t sport,
                    const uint8_t *buf, size_t n, uint64_t now){
    RantAddr src;
    memset(&src, 0, sizeof src);
    memcpy(src.ip, ip, 4); src.ip_len = 4; src.port = sport;
    i_rant_discovery_on_datagram(st, &src, rant_bytes(buf, n), now);
}

static void disc_core_checks(void){
    static uint8_t mem[8192];
    uint8_t buf[RANT_DISCOVERY_WIRE_MAX], out[RANT_DISCOVERY_WIRE_MAX];
    uint8_t sa[4]={10,0,0,1}, sb[4]={10,0,0,2}, sc[4]={10,0,0,3};
    i_RantDiscoveryCoreConfig c; i_RantDiscoveryState *st;
    uint32_t idA, idB; size_t n;
    memset(&c,0,sizeof c);
    memset(c.uuid,0xEE,16);                        /* receiver uuid, distinct from senders */
    c.domain_id=99; c.announce_interval_us=1000000; c.peer_timeout_us=1000000; c.max_peers=2;
    c.on_peer_up=dc_up; c.on_peer_down=dc_down; c.on_event=dc_error;
    st = i_rant_discovery_init(mem,sizeof mem,&c);
    CHECK(st!=NULL, "disc-core: init");
    if (!st) return;
    i_rant_discovery_update(st, 1000, out, sizeof out);     /* start */

    /* Two peers announce: two ups, both ACTIVE */
    dc_up_n=dc_down_n=dc_refused_n=0;
    n=dc_mk(buf,1,0,99,5001,1); dc_feed(st,sa,6730,buf,n,2000); idA=dc_up_id;
    n=dc_mk(buf,2,0,99,5002,1); dc_feed(st,sb,6730,buf,n,2000); idB=dc_up_id;
    CHECK(dc_up_n==2 && i_rant_discovery_peer_count(st)==2,
             "disc-core: two peers up (ups=%u count=%u)", dc_up_n, i_rant_discovery_peer_count(st));

    /* A new peer is REFUSED when the table is full of ACTIVE peers */
    dc_up_n=dc_refused_n=0;
    n=dc_mk(buf,3,0,99,5003,1); dc_feed(st,sc,6730,buf,n,2000);
    CHECK(dc_refused_n==1 && dc_up_n==0 && i_rant_discovery_peer_count(st)==2,
             "disc-core: refuse new peer when full of active (refused=%u up=%u count=%u)",
             dc_refused_n, dc_up_n, i_rant_discovery_peer_count(st));

    /* Silence past the timeout drops both, kept but excluded from the count */
    dc_down_n=0;
    i_rant_discovery_update(st, 2002000, out, sizeof out);
    CHECK(dc_down_n==2 && dc_down_reason==(int)RANT_DISCOVERY_DROP && i_rant_discovery_peer_count(st)==0,
             "disc-core: timeout drops both (downs=%u reason=%d count=%u)",
             dc_down_n, dc_down_reason, i_rant_discovery_peer_count(st));

    /* The same uuid returns: RESUME under the same local_id */
    dc_up_n=0;
    n=dc_mk(buf,1,0,99,5001,1); dc_feed(st,sa,6730,buf,n,2100000);
    CHECK(dc_up_n==1 && dc_up_id==idA && i_rant_discovery_peer_count(st)==1,
             "disc-core: same uuid resumes same id (up=%u sameid=%d count=%u)",
             dc_up_n, dc_up_id==idA, i_rant_discovery_peer_count(st));

    /* A new peer now evicts the oldest DROPPED peer (B) as GONE */
    dc_down_n=dc_up_n=0;
    n=dc_mk(buf,3,0,99,5003,1); dc_feed(st,sc,6730,buf,n,2100000);
    CHECK(dc_down_n==1 && dc_down_id==idB && dc_down_reason==(int)RANT_DISCOVERY_GONE && dc_up_n==1,
             "disc-core: new peer evicts oldest dropped as GONE (downs=%u sameid=%d reason=%d up=%u)",
             dc_down_n, dc_down_id==idB, dc_down_reason, dc_up_n);

    /* BYE is GONE */
    dc_down_n=0;
    n=dc_mk(buf,1,0x01,99,5001,1); dc_feed(st,sa,6730,buf,n,2100000);
    CHECK(dc_down_n==1 && dc_down_id==idA && dc_down_reason==(int)RANT_DISCOVERY_GONE
             && i_rant_discovery_peer_count(st)==1,
             "disc-core: BYE is GONE (downs=%u sameid=%d reason=%d count=%u)",
             dc_down_n, dc_down_id==idA, dc_down_reason, i_rant_discovery_peer_count(st));

    /* A new uuid announcing from an (ip, port) we already hold: that process restarted,
       so the predecessor is evicted as GONE rather than shadowing the newcomer's data */
    { uint32_t idP;
      st = i_rant_discovery_init(mem,sizeof mem,&c);              /* fresh receiver */
      i_rant_discovery_update(st, 1000, out, sizeof out);
      dc_up_n=dc_down_n=0;
      n=dc_mk(buf,7,0,99,6001,1); dc_feed(st,sa,6730,buf,n,3000); idP=dc_up_id;
      dc_up_n=dc_down_n=0;
      n=dc_mk(buf,8,0,99,6001,1); dc_feed(st,sa,6730,buf,n,3100);  /* new uuid, same ip:port */
      CHECK(dc_down_n==1 && dc_down_id==idP && dc_down_reason==(int)RANT_DISCOVERY_GONE
               && dc_up_n==1 && i_rant_discovery_peer_count(st)==1,
               "disc-core: new uuid at a held ip:port evicts the predecessor (downs=%u sameid=%d reason=%d up=%u count=%u)",
               dc_down_n, dc_down_id==idP, dc_down_reason, dc_up_n, i_rant_discovery_peer_count(st));
    }

    /* A node of this domain on another wire version is never a peer and is reported once per
       source, from its first eight bytes alone. Another domain is ignored. */
    st = i_rant_discovery_init(mem,sizeof mem,&c);
    i_rant_discovery_update(st, 1000, out, sizeof out);
    dc_up_n=dc_wire_n=0;
    n=dc_mk(buf,9,0,99,6002,1); buf[4]=(uint8_t)(RANT_WIRE_VERSION+1);
    dc_feed(st,sa,6730,buf,n,4000); dc_feed(st,sa,6730,buf,n,5000);
    dc_feed(st,sb,6730,buf,8,6000);
    n=dc_mk(buf,9,0,98,6002,1); buf[4]=(uint8_t)(RANT_WIRE_VERSION+1); dc_feed(st,sc,6730,buf,n,7000);
    CHECK(dc_wire_n==2 && dc_up_n==0 && i_rant_discovery_peer_count(st)==0,
             "disc-core: another wire version is reported once per source, never a peer (reports=%u ups=%u)",
             dc_wire_n, dc_up_n);
    {   /* each such source is held as a trouble for the node's condition, until it goes quiet */
        i_RantDiscoveryTrouble tr[RANT_DISCOVERY_TROUBLE_MAX]; int nt;
        nt = i_rant_discovery_troubles(st, tr, RANT_DISCOVERY_TROUBLE_MAX);
        CHECK(nt==2 && tr[0].kind==RANT_E_WIRE_VERSION && tr[1].kind==RANT_E_WIRE_VERSION
                 && tr[0].value==(uint32_t)(RANT_WIRE_VERSION+1) && tr[0].heard_us==5000,
                 "disc-core: each source on another wire is a trouble, refreshed when heard (n=%d)", nt);
        i_rant_discovery_update(st, 6000 + 3u*c.announce_interval_us + 1u, out, sizeof out);
        nt = i_rant_discovery_troubles(st, tr, RANT_DISCOVERY_TROUBLE_MAX);
        CHECK(nt==0, "disc-core: a trouble quiet for three announce intervals ages out (n=%d)", nt);
    }

    /* Twenty such sources: the table keeps the first it can hold while they are heard, never
       evicting a live one, and counts the rest as more. Each tracked source is reported once
       and the rest once, so a big mesh on another wire is no event flood. */
    {   i_RantDiscoveryTrouble tr[RANT_DISCOVERY_TROUBLE_MAX]; int nt, k, round;
        uint8_t ip[4] = {10,0,1,0};
        st = i_rant_discovery_init(mem,sizeof mem,&c);
        i_rant_discovery_update(st, 1000, out, sizeof out);
        dc_wire_n = 0;
        for (round = 0; round < 3; round++)
            for (k = 0; k < RANT_DISCOVERY_TROUBLE_MAX + 4; k++){
                ip[3] = (uint8_t)(k + 1);
                n = dc_mk(buf,(uint8_t)(20 + k),0,99,6002,1); buf[4]=(uint8_t)(RANT_WIRE_VERSION+1);
                dc_feed(st,ip,6730,buf,n,(uint64_t)2000 + (uint64_t)round*500000u + (uint64_t)k);
            }
        nt = i_rant_discovery_troubles(st, tr, RANT_DISCOVERY_TROUBLE_MAX);
        CHECK(nt==RANT_DISCOVERY_TROUBLE_MAX && tr[0].addr.ip[3]==1
                 && i_rant_discovery_trouble_more(st, RANT_E_WIRE_VERSION)
                 && dc_wire_n==RANT_DISCOVERY_TROUBLE_MAX + 1u,
                 "disc-core: a full table keeps its live sources and counts the rest once (n=%d reports=%u)",
                 nt, dc_wire_n);
        i_rant_discovery_update(st, 2000 + 2u*500000u + 100u + 3u*c.announce_interval_us, out, sizeof out);
        CHECK(i_rant_discovery_troubles(st, tr, RANT_DISCOVERY_TROUBLE_MAX)==0
                 && !i_rant_discovery_trouble_more(st, RANT_E_WIRE_VERSION),
                 "disc-core: the table and its count of more age out together");
    }

    /* A peer heard on two paths is known from either, as a multihomed host sends from
       whichever its route picks. A path two peers share is neither's, and a path quiet for
       three announce intervals stops counting. */
    { uint32_t idM, idN, got = 0;
      st = i_rant_discovery_init(mem,sizeof mem,&c);
      i_rant_discovery_update(st, 1000, out, sizeof out);
      n=dc_mk(buf,10,0,99,6100,1); dc_feed(st,sa,6730,buf,n,2000); idM=dc_up_id;
      dc_feed(st,sb,6730,buf,n,2000);
      n=dc_mk(buf,11,0,99,6100,1); dc_feed(st,sc,6730,buf,n,2000); idN=dc_up_id;
      CHECK(i_rant_discovery_id_for_addr(st,sa,4,6100,&got) && got==idM
               && i_rant_discovery_id_for_addr(st,sb,4,6100,&got) && got==idM
               && !i_rant_discovery_id_for_addr(st,sb,4,6101,&got),
               "disc-core: a peer heard on two paths is known from either");
      dc_feed(st,sb,6730,buf,n,2500);
      CHECK(!i_rant_discovery_id_for_addr(st,sb,4,6100,&got)
               && i_rant_discovery_id_for_addr(st,sc,4,6100,&got) && got==idN,
               "disc-core: a path two peers share is neither's");
      n=dc_mk(buf,10,0,99,6100,1); dc_feed(st,sa,6730,buf,n,3500000);
      CHECK(i_rant_discovery_id_for_addr(st,sa,4,6100,&got) && got==idM
               && !i_rant_discovery_id_for_addr(st,sb,4,6100,&got),
               "disc-core: a path quiet for three intervals stops counting");
    }
}

/* node core peer lifecycle, sans IO: drive the peer up, down and refused hooks directly
   over a transport with no sockets, clock or platform anywhere. */
static uint32_t nc_up_n, nc_down_n, nc_refused_n, nc_up_id, nc_down_id;
static void nc_event(const RantEvent *ev){
    if      (ev->kind==RANT_PEER_UP)       { nc_up_n++;   nc_up_id=ev->peer; }
    else if (ev->kind==RANT_PEER_DOWN)     { nc_down_n++; nc_down_id=ev->peer; }
    else if (ev->kind==RANT_ERROR && ev->error==RANT_E_PEER_REFUSED){ nc_refused_n++; }
}
/* node core peer lifecycle, fully sans IO: a transport plus a real discovery core, the
   node core delegating its peer table to discovery. */
static void node_core_checks(void){
    static uint8_t tmem[1<<18], cmem[4096], dmem[8192], amem[1<<16];
    uint8_t buf[256], out[RANT_DISCOVERY_WIRE_MAX];
    uint8_t sa[4]={10,0,0,1}, sb[4]={10,0,0,2}, sc[4]={10,0,0,3};
    i_RantTransportConfig tc; i_RantTransportState *tr; i_RantTopicDef ch[1];
    i_RantDiscoveryCoreConfig dcfg; i_RantDiscoveryState *st;
    i_RantNodeCoreConfig cc; i_RantNodeCore *nc;
    RantAddr d; uint32_t id, idA, idB; size_t n;
    /* STATIC allocator over a caller buffer: the embedded no-heap contract */
    static RantAllocator A; A = rant_allocator_static(amem, sizeof amem);

    memset(ch,0,sizeof ch); ch[0].name="nc/topic";
    memset(&tc,0,sizeof tc); tc.topics=ch; tc.n_topics=1; tc.max_peers=2;
    tc.allocator=rant_allocator_alloc; tc.user=&A;
    tr = i_rant_transport_init(tmem, sizeof tmem, &tc);
    CHECK(tr!=NULL, "node-core: transport init");
    if (!tr) return;

    /* node core first (discovery bound once it exists, exactly like the runtime) */
    memset(&cc,0,sizeof cc);
    cc.transport=tr; cc.n_topics=1; cc.frag_size=1200; cc.on_event=nc_event;
    cc.alloc=rant_allocator_alloc; cc.alloc_user=&A;
    nc = i_rant_node_core_init(cmem, sizeof cmem, &cc);
    CHECK(nc!=NULL, "node-core: init");
    if (!nc) return;

    /* the discovery core whose peer table the node core delegates to: reserve the node's
       per peer scratch via peer_user_bytes and wire its events into the node core */
    memset(&dcfg,0,sizeof dcfg); memset(dcfg.uuid,0xEE,16);
    dcfg.domain_id=99; dcfg.announce_interval_us=1000000; dcfg.peer_timeout_us=1000000; dcfg.max_peers=2;
    dcfg.peer_user_bytes=i_rant_node_core_peer_user_bytes();
    i_rant_node_core_discovery_hooks(nc, &dcfg);
    st = i_rant_discovery_init(dmem, sizeof dmem, &dcfg);
    CHECK(st!=NULL, "node-core: discovery init");
    if (!st) return;
    i_rant_node_core_bind_discovery(nc, st);
    i_rant_discovery_update(st, 1000, out, sizeof out);     /* start */

    /* build-meta still works (uses the transport, not any peer table) */
    {   RantBytes mb;
        i_rant_node_core_build_meta(nc);
        mb = i_rant_node_core_meta(nc);
        CHECK(mb.len>=5 && i_rant_meta_frag(mb)==1200,
                 "node-core: builds overlay (frag=%u)", i_rant_meta_frag(mb)); }

    /* Two peers announce: two ups, discovery assigns the ids, resolve each way */
    nc_up_n=nc_down_n=nc_refused_n=0;
    n=dc_wire(buf,1,0,99,5001,"nc-self",1); dc_feed(st,sa,6730,buf,n,2000); idA=nc_up_id;
    n=dc_wire(buf,2,0,99,5002,"nc-self",1); dc_feed(st,sb,6730,buf,n,2000); idB=nc_up_id;
    CHECK(nc_up_n==2, "node-core: two peers up (ups=%u)", nc_up_n);
    {   RantString pn = i_rant_node_core_peer_name(nc, idA);
        CHECK(pn.data && pn.len==7 && memcmp(pn.data,"nc-self",7)==0,
                 "node-core: peer name learned from announce (%.*s)", (int)pn.len, pn.data?pn.data:"?"); }
    CHECK(i_rant_node_core_resolve(nc,idA,&d) && d.port==5001 && d.ip[3]==1,
             "node-core: peer id resolves to addr (port=%u ip3=%u)", d.port, d.ip[3]);
    CHECK(i_rant_node_core_id_for_addr(nc, sb, 5002, &id) && id==idB,
             "node-core: addr resolves to id (id=%u)", id);

    /* a nameless announce falls back to "unknown-peer", never empty or NULL */
    {   RantString pn;
        n=dc_wire(buf,1,0,99,5001,NULL,2);     dc_feed(st,sa,6730,buf,n,2001);
        pn = i_rant_node_core_peer_name(nc, idA);
        CHECK(pn.data && pn.len==12 && memcmp(pn.data,"unknown-peer",12)==0,
                 "node-core: nameless announce -> unknown-peer (%.*s)", (int)pn.len, pn.data?pn.data:"(null)");
        n=dc_wire(buf,1,0,99,5001,"nc-self",3); dc_feed(st,sa,6730,buf,n,2002); }

    /* A third peer is REFUSED while the table is full of ACTIVE peers, the node forwards it */
    nc_refused_n=0;
    n=dc_wire(buf,3,0,99,5003,"three",1); dc_feed(st,sc,6730,buf,n,2003);
    CHECK(nc_refused_n==1, "node-core: refused forwarded (refused=%u)", nc_refused_n);

    /* Silence past the timeout DROPS both: a PEER_DOWN each, but the slots are kept (resolve) */
    nc_down_n=0;
    i_rant_discovery_update(st, 1003000, out, sizeof out);
    CHECK(nc_down_n==2, "node-core: timeout drops both (downs=%u)", nc_down_n);
    CHECK(i_rant_node_core_resolve(nc,idA,&d)==1, "node-core: dropped peer kept (resolves)");

    /* The same uuid returns: RESUME re fires PEER_UP under the same id */
    nc_up_n=0;
    n=dc_wire(buf,1,0,99,5001,"nc-self",4); dc_feed(st,sa,6730,buf,n,1100000);
    CHECK(nc_up_n==1 && nc_up_id==idA, "node-core: resume re-ups same id (ups=%u id=%u)", nc_up_n, nc_up_id);

    /* GONE (BYE) on the active peer: one PEER_DOWN, and it does not resolve any more */
    nc_down_n=0;
    n=dc_wire(buf,1,0x01,99,5001,NULL,1); dc_feed(st,sa,6730,buf,n,1100001);
    CHECK(nc_down_n==1, "node-core: GONE on active fires down (downs=%u)", nc_down_n);
    CHECK(i_rant_node_core_resolve(nc,idA,&d)==0, "node-core: GONE peer freed (no resolve)");
}

/* The codec: decode reads back what encode wrote, and each way a datagram is refused. */
static void wire_checks(void){
    static const uint8_t uuid[16] = { 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16 };
    static const uint8_t ov[5] = { 9, 8, 7, 6, 5 };
    uint8_t buf[256]; size_t n; i_RantDiscoveryWire w, r;
    char longname[RANT_NODE_NAME_MAX + 8];
    memset(&w, 0, sizeof w);
    w.flags = RANT_DISCOVERY_FLAG_REQ | RANT_DISCOVERY_FLAG_BLOB; w.domain = 77; w.uuid = uuid;
    w.version = 0x01020304u; w.changed_ago_ms = 0xA0B0C0D0u; w.data_port = 6801;
    w.name = rant_cstr("wire-node"); w.overlay = rant_bytes(ov, sizeof ov);
    n = i_rant_discovery_encode(&w, buf, sizeof buf);
    CHECK(n == RANT_DISCOVERY_FIXED_LEN + 9u + 5u,
          "wire: encode writes the fixed part, the name and the overlay (%u)", (unsigned)n);
    CHECK(i_rant_discovery_decode(rant_bytes(buf, n), &r) == I_RANT_DISC_OK
          && r.flags == w.flags && r.domain == 77 && memcmp(r.uuid, uuid, 16) == 0
          && r.version == w.version && r.changed_ago_ms == w.changed_ago_ms && r.data_port == 6801
          && r.name.len == 9 && memcmp(r.name.data, "wire-node", 9) == 0
          && r.overlay.len == 5 && memcmp(r.overlay.data, ov, 5) == 0 && r.overlay_len == 5,
          "wire: decode reads back what encode wrote");
    CHECK(i_rant_discovery_encode(&w, buf, n - 1) == 0, "wire: encode refuses a buffer too small");
    w.flags = 0;
    n = i_rant_discovery_encode(&w, buf, sizeof buf);
    CHECK(n == RANT_DISCOVERY_FIXED_LEN + 9u && i_rant_discovery_decode(rant_bytes(buf, n), &r) == I_RANT_DISC_OK
          && r.overlay.len == 0 && r.overlay_len == 0, "wire: the overlay rides only with BLOB");
    memset(longname, 'n', sizeof longname - 1); longname[sizeof longname - 1] = '\0';
    w.name = rant_cstr(longname);
    n = i_rant_discovery_encode(&w, buf, sizeof buf);
    CHECK(i_rant_discovery_decode(rant_bytes(buf, n), &r) == I_RANT_DISC_OK && r.name.len == RANT_NODE_NAME_MAX,
          "wire: a long name is cut to RANT_NODE_NAME_MAX (%u)", (unsigned)r.name.len);

    w.name = rant_cstr("x"); w.flags = RANT_DISCOVERY_FLAG_BLOB;
    n = i_rant_discovery_encode(&w, buf, sizeof buf);
    CHECK(i_rant_discovery_decode(rant_bytes(buf, 7), &r) == I_RANT_DISC_FOREIGN, "wire: under 8 bytes is foreign");
    buf[0] = 'x';
    CHECK(i_rant_discovery_decode(rant_bytes(buf, n), &r) == I_RANT_DISC_FOREIGN, "wire: another magic is foreign");
    buf[0] = 'u'; buf[4] = (uint8_t)(RANT_WIRE_VERSION + 1);
    CHECK(i_rant_discovery_decode(rant_bytes(buf, n), &r) == I_RANT_DISC_WIRE
          && r.wire == (uint8_t)(RANT_WIRE_VERSION + 1) && r.domain == 77 && r.flags == RANT_DISCOVERY_FLAG_BLOB,
          "wire: another version is known by its first eight bytes");
    buf[4] = (uint8_t)RANT_WIRE_VERSION;
    CHECK(i_rant_discovery_decode(rant_bytes(buf, RANT_DISCOVERY_FIXED_LEN - 1), &r) == I_RANT_DISC_MALFORMED
          && i_rant_discovery_decode(rant_bytes(buf, RANT_DISCOVERY_FIXED_LEN), &r) == I_RANT_DISC_MALFORMED,
          "wire: a cut fixed part or name is malformed");
    CHECK(i_rant_discovery_decode(rant_bytes(buf, n - 2), &r) == I_RANT_DISC_CUT && r.overlay_len == 5
          && r.data_port == 6801, "wire: a cut overlay says how much was meant");
    buf[n] = 0;
    CHECK(i_rant_discovery_decode(rant_bytes(buf, n + 1), &r) == I_RANT_DISC_MALFORMED,
          "wire: a trailing byte is malformed");
    buf[36] = RANT_NODE_NAME_MAX + 1;
    CHECK(i_rant_discovery_decode(rant_bytes(buf, sizeof buf), &r) == I_RANT_DISC_MALFORMED,
          "wire: a name past the cap is malformed");
    buf[36] = 1; buf[5] = 0;
    CHECK(i_rant_discovery_decode(rant_bytes(buf, n), &r) == I_RANT_DISC_MALFORMED,
          "wire: an overlay without BLOB is malformed");
}

/* Every datagram says how long ago our blob changed: from the start, then from each change,
   saturating rather than wrapping. */
static void change_age_checks(void){
    static uint8_t mem[8192];
    uint8_t out[RANT_DISCOVERY_WIRE_MAX]; size_t n;
    i_RantDiscoveryCoreConfig c; i_RantDiscoveryState *st; i_RantDiscoveryWire r;
    uint64_t t0 = 5000000u;
    memset(&c,0,sizeof c); memset(c.uuid,0xEE,16);
    c.domain_id=99; c.announce_interval_us=1000000; c.peer_timeout_us=4000000; c.max_peers=2;
    st = i_rant_discovery_init(mem, sizeof mem, &c);
    CHECK(st != NULL, "change age: init");
    if (!st) return;
    n = i_rant_discovery_update(st, t0, out, sizeof out);
    CHECK(n && i_rant_discovery_decode(rant_bytes(out, n), &r) == I_RANT_DISC_OK && r.changed_ago_ms == 0,
          "change age: the start counts as the first change (%u ms)", (unsigned)r.changed_ago_ms);
    i_rant_discovery_solicit(st);
    n = i_rant_discovery_update(st, t0 + 2500000u, out, sizeof out);
    CHECK(n && i_rant_discovery_decode(rant_bytes(out, n), &r) == I_RANT_DISC_OK && r.changed_ago_ms == 2500,
          "change age: it grows with the clock (%u ms)", (unsigned)r.changed_ago_ms);
    i_rant_discovery_set_meta(st, rant_bytes(NULL, 0), t0 + 3000000u);
    n = i_rant_discovery_update(st, t0 + 3250000u, out, sizeof out);
    CHECK(n && i_rant_discovery_decode(rant_bytes(out, n), &r) == I_RANT_DISC_OK && r.changed_ago_ms == 250
          && r.version == 2, "change age: a change restarts it (%u ms, version %u)",
          (unsigned)r.changed_ago_ms, (unsigned)r.version);
    i_rant_discovery_solicit(st);
    n = i_rant_discovery_update(st, t0 + 3000000u + 0x100000000ull * 1000u, out, sizeof out);
    CHECK(n && i_rant_discovery_decode(rant_bytes(out, n), &r) == I_RANT_DISC_OK && r.changed_ago_ms == 0xFFFFFFFFu,
          "change age: past 49 days it stays at the most it can say");
}

static const TestCase cases[] = {
    { "wire",           wire_checks },
    { "change_age",     change_age_checks },
    { "discovery_core", disc_core_checks },
    { "node_core", node_core_checks },
};

TEST_MAIN(cases)
