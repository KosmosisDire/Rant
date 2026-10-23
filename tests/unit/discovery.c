/* The discovery core and the node core, sans IO: the record store, peer lifecycle, paths,
 * and the node's peer table over a real discovery core with no sockets. spec/testing.md
 * says what each case pins. */
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

/* craft a datagram for sender `uid` (uuid = all uid bytes) with exactly these flags, an
   empty blob when BLOB is set, into a RANT_DISCOVERY_WIRE_MAX buffer */
static size_t dc_enc(uint8_t *p, uint8_t uid, uint8_t flags, uint16_t dom, uint16_t port,
                     const char *name, uint32_t mver){
    static uint8_t uuid[16];
    i_RantDiscoveryWire w;
    memset(uuid, uid, 16);
    memset(&w, 0, sizeof w);
    w.flags = flags; w.domain = dom; w.uuid = uuid;
    w.version = mver; w.data_port = port;
    w.name = name ? rant_cstr(name) : rant_string(NULL, 0);
    return i_rant_discovery_encode(&w, p, RANT_DISCOVERY_WIRE_MAX);
}
/* the same carrying the blob. flags RANT_DISCOVERY_FLAG_BYE for a bye */
static size_t dc_wire(uint8_t *p, uint8_t uid, uint8_t flags, uint16_t dom, uint16_t port,
                      const char *name, uint32_t mver){
    return dc_enc(p, uid, (uint8_t)(flags | RANT_DISCOVERY_FLAG_BLOB), dom, port, name, mver);
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

/* the record store's hook, failing on demand to check that a refused growth leaves it whole */
static int rs_fail;
static void *rs_alloc(void *u, void *ptr, size_t size){
    if (rs_fail && size) return NULL;
    return rant_allocator_alloc(u, ptr, size);
}

static int rs_find_all(const i_RantDiscoveryRecords *r, const uint16_t *at, const uint32_t *ids, int n){
    int k;
    for (k = 0; k < n; k++){
        uint8_t uuid[16]; memset(uuid, 0, 16); uuid[0] = (uint8_t)k; uuid[1] = (uint8_t)(k >> 8);
        if (at[k] == I_RANT_REC_NONE){
            if (i_rant_records_find(r, uuid) != I_RANT_REC_NONE || i_rant_records_by_id(r, ids[k]) != I_RANT_REC_NONE)
                return 0;
        } else if (i_rant_records_find(r, uuid) != at[k] || i_rant_records_by_id(r, ids[k]) != at[k]
                   || i_rant_records_at(r, at[k])->local_id != ids[k])
            return 0;
    }
    return 1;
}

/* The due queue head to tail into q. -1 when its back links or tail disagree. */
static int rs_queue(const i_RantDiscoveryRecords *r, uint16_t *q, int max){
    uint16_t i, prev = I_RANT_REC_NONE; int n = 0;
    for (i = r->due_head; i != I_RANT_REC_NONE && n < max; i = r->recs[i].due_next){
        if (r->recs[i].due_prev != prev) return -1;
        q[n++] = i; prev = i;
    }
    return (i == I_RANT_REC_NONE && r->due_tail == prev) ? n : -1;
}

/* Records are found by uuid and by id through growth and reuse, an id is never handed out
   twice, and a store without a hook stays at its reserve. */
static void records_checks(void){
    static uint8_t mem[4096];
    RantAllocator ma = rant_allocator_heap(0);
    i_RantDiscoveryRecords r; i_RantDiscoveryRecordsBlocks blk; i_RantBump b;
    uint16_t at[300]; uint32_t ids[300], id_max = 0; uint8_t uuid[16];
    int k, ok;

    memset(&b, 0, sizeof b); b.base = mem; b.cap = sizeof mem;
    i_rant_records_layout(&b, 3, 0, &blk);
    CHECK(i_rant_records_init(&r, &blk, 3, NULL, NULL), "records: an arena store opens");
    for (k = 0; k < 3; k++){
        memset(uuid, 0, 16); uuid[0] = (uint8_t)k;
        at[k] = i_rant_records_add(&r, uuid); ids[k] = i_rant_records_at(&r, at[k])->local_id;
    }
    memset(uuid, 0, 16); uuid[0] = 3;
    CHECK(i_rant_records_add(&r, uuid) == I_RANT_REC_NONE && r.count == 3,
          "records: an arena store stays at its reserve (count %u)", (unsigned)r.count);
    i_rant_records_remove(&r, at[1]);
    at[1] = I_RANT_REC_NONE;
    memset(uuid, 0, 16); uuid[0] = 1;
    CHECK(rs_find_all(&r, at, ids, 3), "records: a removed record is gone by uuid and by id");
    at[1] = i_rant_records_add(&r, uuid); ids[1] = i_rant_records_at(&r, at[1])->local_id;
    CHECK(ids[1] == 4 && rs_find_all(&r, at, ids, 3),
          "records: the place is reused under a fresh id (id %u)", (unsigned)ids[1]);

    CHECK(i_rant_records_init(&r, &blk, 2, rs_alloc, &ma), "records: a hook store opens");
    for (k = 0; k < 300; k++){
        memset(uuid, 0, 16); uuid[0] = (uint8_t)k; uuid[1] = (uint8_t)(k >> 8);
        at[k] = i_rant_records_add(&r, uuid);
        ids[k] = at[k] != I_RANT_REC_NONE ? i_rant_records_at(&r, at[k])->local_id : 0;
    }
    CHECK(r.count == 300 && r.cap >= 300 && rs_find_all(&r, at, ids, 300),
          "records: a hook store grows and finds every record (count %u cap %u)",
          (unsigned)r.count, (unsigned)r.cap);
    for (k = 0; k < 300; k += 2){ i_rant_records_remove(&r, at[k]); at[k] = I_RANT_REC_NONE; }
    ok = rs_find_all(&r, at, ids, 300);
    for (k = 0; k < 300; k++) if (ids[k] > id_max) id_max = ids[k];
    for (k = 0; k < 300; k += 2){
        memset(uuid, 0, 16); uuid[0] = (uint8_t)k; uuid[1] = (uint8_t)(k >> 8);
        at[k] = i_rant_records_add(&r, uuid); ids[k] = i_rant_records_at(&r, at[k])->local_id;
        if (ids[k] <= id_max) ok = 0;
    }
    CHECK(ok && rs_find_all(&r, at, ids, 300) && r.count == 300,
          "records: removing half and adding them back keeps every lookup and mints new ids");

    rs_fail = 1;
    {   uint16_t cap = r.cap, got = 0;
        for (k = 300; k < 300 + cap; k++){
            memset(uuid, 0, 16); uuid[0] = (uint8_t)k; uuid[1] = (uint8_t)(k >> 8);
            if (i_rant_records_add(&r, uuid) != I_RANT_REC_NONE) got++;
        }
        CHECK(r.cap == cap && r.count == 300 + got && got == (uint16_t)(cap - 300) && rs_find_all(&r, at, ids, 300),
              "records: a refused growth fills what is there and keeps every record (%u more)", (unsigned)got);
    }
    rs_fail = 0;

    r.next_id = 0xFFFFFFFFu;
    memset(uuid, 0xA5, 16);
    i_rant_records_remove(&r, at[1]);
    {   uint16_t i1 = i_rant_records_add(&r, uuid), i2;
        uuid[0] = 0x5A; i_rant_records_remove(&r, at[3]); i2 = i_rant_records_add(&r, uuid);
        CHECK(i_rant_records_at(&r, i1)->local_id == 0xFFFFFFFFu && i_rant_records_at(&r, i2)->local_id == 1u,
              "records: the id counter wraps past 0, which is RANT_SELF");
    }
    i_rant_records_destroy(&r);
    CHECK(ma.in_use == 0, "records: destroy frees the hook memory (%u in use)",
          (unsigned)ma.in_use);
    rant_allocator_reset(&ma);

    /* The due queue holds exactly the records owed work, oldest first, and live counts the
       ACTIVE records through every change of state */
    {   uint16_t d[4], q[4];
        memset(&b, 0, sizeof b); b.base = mem; b.cap = sizeof mem;
        i_rant_records_layout(&b, 4, 0, &blk);
        CHECK(i_rant_records_init(&r, &blk, 4, NULL, NULL), "records: a store for the queue opens");
        for (k = 0; k < 4; k++){ memset(uuid, 0, 16); uuid[0] = (uint8_t)(k + 1); d[k] = i_rant_records_add(&r, uuid); }
        i_rant_records_owe(&r, d[0], I_RANT_OWE_REPLY, 500);
        i_rant_records_owe(&r, d[1], I_RANT_OWE_FETCH, 300);
        i_rant_records_owe(&r, d[2], I_RANT_OWE_REPLY, 400);
        i_rant_records_owe(&r, d[0], I_RANT_OWE_FETCH, 100);
        i_rant_records_owe(&r, d[2], I_RANT_OWE_REPLY, 450);
        CHECK(rs_queue(&r, q, 4) == 3 && q[0] == d[0] && q[1] == d[1] && q[2] == d[2]
              && r.recs[d[0]].due_us == 100 && r.recs[d[2]].due_us == 400
              && r.recs[d[0]].owed == (I_RANT_OWE_REPLY | I_RANT_OWE_FETCH),
              "records: owing joins the queue once and keeps the earliest due time");
        i_rant_records_settle(&r, d[0], I_RANT_OWE_REPLY);
        i_rant_records_requeue(&r, d[0], 900);
        i_rant_records_remove(&r, d[1]);
        i_rant_records_settle(&r, d[3], I_RANT_OWE_REPLY);
        i_rant_records_requeue(&r, d[3], 50);
        CHECK(rs_queue(&r, q, 4) == 2 && q[0] == d[2] && q[1] == d[0] && r.recs[d[0]].due_us == 900
              && r.recs[d[0]].owed == I_RANT_OWE_FETCH,
              "records: a requeue moves to the tail, a removed record leaves, one owing nothing never joins");
        i_rant_records_settle(&r, d[0], I_RANT_OWE_FETCH);
        i_rant_records_settle(&r, d[2], I_RANT_OWE_REPLY);
        CHECK(rs_queue(&r, q, 4) == 0 && r.due_head == I_RANT_REC_NONE, "records: settling all empties the queue");
        i_rant_records_set_state(&r, d[0], I_RANT_REC_DROPPED);
        i_rant_records_set_state(&r, d[0], I_RANT_REC_DROPPED);
        i_rant_records_set_state(&r, d[2], I_RANT_REC_DROPPED);
        i_rant_records_remove(&r, d[2]);
        k = r.live;
        i_rant_records_set_state(&r, d[0], I_RANT_REC_ACTIVE);
        CHECK(k == 1 && r.live == 2 && r.count == 2,
              "records: live counts the ACTIVE records through drops, returns and removes (%d then %u)",
              k, (unsigned)r.live);
    }
}

/* A grow keeps every peer with its id and scratch and takes the one it refused. Only a hook
   core migrates, and never to fewer slots. */
static void migrate_checks(void){
    static uint8_t m1[16384], m2[16384], m3[16384];
    RantAllocator ma = rant_allocator_heap(0);
    uint8_t buf[RANT_DISCOVERY_WIRE_MAX], out[RANT_DISCOVERY_WIRE_MAX];
    uint8_t sa[4]={10,0,0,1}, sb[4]={10,0,0,2}, sc[4]={10,0,0,3};
    i_RantDiscoveryCoreConfig c; i_RantDiscoveryState *st, *st2; RantAddr addr;
    uint32_t idA, idB, ups; size_t n; uint64_t *mark;
    memset(&c,0,sizeof c); memset(c.uuid,0xEE,16);
    c.domain_id=99; c.announce_interval_us=1000000; c.peer_timeout_us=4000000; c.max_peers=2;
    c.peer_user_bytes=8; c.on_peer_up=dc_up; c.on_peer_down=dc_down; c.on_event=dc_error;
    st = i_rant_discovery_init(m3, sizeof m3, &c);
    CHECK(st && i_rant_discovery_core_migrate(st, m2, sizeof m2, 4, 0, NULL, NULL) == NULL,
          "migrate: a pool core does not migrate");
    c.alloc=rant_allocator_alloc; c.alloc_user=&ma;
    st = i_rant_discovery_init(m1, sizeof m1, &c);
    CHECK(st != NULL, "migrate: init");
    if (!st) return;
    i_rant_discovery_update(st, 1000, out, sizeof out);
    dc_up_n=dc_refused_n=0;
    n=dc_wire(buf,1,0,99,5001,"alpha",1); dc_feed(st,sa,6730,buf,n,2000); idA=dc_up_id;
    n=dc_wire(buf,2,0,99,5002,"beta",1);  dc_feed(st,sb,6730,buf,n,2000); idB=dc_up_id;
    n=dc_mk(buf,3,0,99,5003,1); dc_feed(st,sc,6730,buf,n,2000);
    mark = (uint64_t*)i_rant_discovery_peer_user(st, idA);
    if (mark) *mark = 0x1234;
    CHECK(dc_up_n == 2 && dc_refused_n == 1 && mark, "migrate: two peers up, the third refused");

    st2 = i_rant_discovery_core_migrate(st, m2, sizeof m2, 4, 0, NULL, NULL);
    CHECK(st2 && i_rant_discovery_max_peers(st2) == 4 && i_rant_discovery_peer_count(st2) == 2,
          "migrate: the grown core holds both peers in 4 slots");
    if (!st2) return;
    mark = (uint64_t*)i_rant_discovery_peer_user(st2, idA);
    CHECK(mark && *mark == 0x1234 && i_rant_discovery_addr_of_id(st2, idB, &addr) && addr.port == 5002
          && i_rant_discovery_peer_name(st2, idB).len == 4,
          "migrate: ids keep their scratch, locator and name");
    ups = dc_up_n;
    n=dc_wire(buf,1,0,99,5001,"alpha",1); dc_feed(st2,sa,6730,buf,n,3000);
    CHECK(dc_up_n == ups, "migrate: a known peer is not new after the move");
    n=dc_mk(buf,3,0,99,5003,1); dc_feed(st2,sc,6730,buf,n,3000);
    CHECK(dc_up_n == ups + 1 && dc_up_id != idA && dc_up_id != idB && i_rant_discovery_peer_count(st2) == 3,
          "migrate: the refused peer takes a new slot");
    CHECK(i_rant_discovery_core_migrate(st2, m1, sizeof m1, 2, 0, NULL, NULL) == NULL,
          "migrate: never to fewer slots");
    i_rant_discovery_destroy(st2);
    CHECK(ma.in_use == 0, "migrate: destroy frees every hook block (%u in use)", (unsigned)ma.in_use);
    rant_allocator_reset(&ma);
}

/* The flags of a datagram a core built, -1 for none. */
static int dc_sent(const uint8_t *out, size_t n){
    i_RantDiscoveryWire r;
    if (!n || i_rant_discovery_decode(rant_bytes(out, n), &r) != I_RANT_DISC_OK) return -1;
    return r.flags;
}

/* Sends every targeted datagram due by now. How many went out, the last one's flags and port. */
static int dc_drain(i_RantDiscoveryState *st, uint64_t now, int *flags, uint16_t *port){
    uint8_t out[RANT_DISCOVERY_WIRE_MAX]; RantAddr to; size_t n; int k = 0;
    *flags = -1; *port = 0;
    while (k < 64 && (n = i_rant_discovery_poll_targeted(st, now, out, sizeof out, &to)) != 0){
        *flags = dc_sent(out, n); *port = to.port; k++;
    }
    return k;
}

/* 1 when the core copies its announces to the record with this data port, marked as *uni. */
static int dc_copy_of(const i_RantDiscoveryState *st, uint16_t port, int *uni){
    uint16_t i; RantAddr a;
    for (i = 0; i < i_rant_discovery_record_cap(st); i++)
        if (i_rant_discovery_copy_addr(st, i, &a, uni) && a.port == port) return 1;
    *uni = 0;
    return 0;
}

/* The work owed to peers: the opening, answers at once or spread, the match answer, a fetch
   while a version is ahead and its retries, copies by unicast, and the timeout sweep. */
static void owed_checks(void){
    static uint8_t mem[16384];
    const uint8_t F_REQ = RANT_DISCOVERY_FLAG_REQ, F_BLOB = RANT_DISCOVERY_FLAG_BLOB, F_ONE = RANT_DISCOVERY_FLAG_ONE,
                  F_UNI = RANT_DISCOVERY_FLAG_UNI, F_ALL = RANT_DISCOVERY_FLAG_ALL;
    uint8_t buf[RANT_DISCOVERY_WIRE_MAX], out[RANT_DISCOVERY_WIRE_MAX], ip[4] = {10,0,0,1};
    i_RantDiscoveryCoreConfig c; i_RantDiscoveryState *st;
    uint64_t t = 1000000u; size_t n; int k, fl, uni, sends; uint16_t port;
    memset(&c,0,sizeof c); memset(c.uuid,0xEE,16);
    c.domain_id=99; c.announce_interval_us=1000000; c.peer_timeout_us=4000000; c.max_peers=8;

    c.observer = 1;
    st = i_rant_discovery_init(mem, sizeof mem, &c);
    n = st ? i_rant_discovery_update(st, t, out, sizeof out) : 0;
    CHECK(dc_sent(out, n) == (F_REQ | F_ALL | F_BLOB), "owed: an observer opens asking every peer to answer at once");
    c.observer = 0;
    st = i_rant_discovery_init(mem, sizeof mem, &c);
    CHECK(st != NULL, "owed: init");
    if (!st) return;

    n = dc_wire(buf,1,(uint8_t)(F_REQ|F_ONE),99,5001,NULL,1); dc_feed(st,ip,6730,buf,n,t);
    CHECK(dc_drain(st,t,&fl,&port) == 0, "owed: nothing goes out before the opening");
    n = i_rant_discovery_update(st, t, out, sizeof out);
    CHECK(dc_sent(out, n) == F_BLOB, "owed: the opening carries our blob and asks nothing (flags %d)", dc_sent(out, n));
    CHECK(dc_drain(st,t,&fl,&port) == 1 && fl == (F_ONE|F_BLOB) && port == 5001,
          "owed: a targeted solicit is answered at once with BLOB|ONE (flags %d port %u)", fl, port);

    /* the match answer: a blob judged new, from a datagram not aimed at us */
    n = dc_wire(buf,2,0,99,5002,NULL,1); dc_feed(st,ip,6730,buf,n,t);
    CHECK(dc_drain(st,t,&fl,&port) == 1 && fl == (F_ONE|F_BLOB) && port == 5002,
          "owed: a new blob from the group is answered at once (flags %d)", fl);
    n = dc_wire(buf,3,F_ONE,99,5003,NULL,1); dc_feed(st,ip,6730,buf,n,t);
    sends = dc_drain(st,t,&fl,&port);
    n = dc_wire(buf,2,0,99,5002,NULL,1); dc_feed(st,ip,6730,buf,n,t);
    sends += dc_drain(st,t,&fl,&port);
    n = dc_wire(buf,2,0,99,5002,NULL,2); dc_feed(st,ip,6730,buf,n,t);
    CHECK(sends == 0 && dc_drain(st,t,&fl,&port) == 0,
          "owed: a first blob from a targeted send is not answered, nor one judged before, nor a change");

    for (k = 0; k < 3; k++){
        uint8_t how = (uint8_t)(k == 0 ? F_UNI : k == 1 ? F_ONE : F_ALL);
        n = dc_wire(buf,2,(uint8_t)(F_REQ|how),99,5002,NULL,1); dc_feed(st,ip,6730,buf,n,t);
        CHECK(dc_drain(st,t,&fl,&port) == 1 && port == 5002,
              "owed: a solicit marked %s is answered at once", k == 0 ? "UNI" : k == 1 ? "ONE" : "ALL");
    }
    for (k = 4; k <= 8; k++){ n = dc_wire(buf,(uint8_t)k,F_ONE,99,(uint16_t)(5000+k),NULL,1); dc_feed(st,ip,6730,buf,n,t); }
    for (k = 1; k <= 8; k++){ n = dc_wire(buf,(uint8_t)k,F_REQ,99,(uint16_t)(5000+k),NULL,1); dc_feed(st,ip,6730,buf,n,t); }
    {   int got = dc_drain(st,t,&fl,&port), total = got, spots = got > 0, ms;
        for (ms = 1; ms < 8; ms++){
            got = dc_drain(st, t + (uint64_t)ms*1000u, &fl, &port);
            total += got; spots += got > 0;
        }
        CHECK(i_rant_discovery_peer_count(st) == 8 && total == 8 && spots >= 3,
              "owed: a group solicit is answered within one ms per live peer, spread (%d by 7 ms over %d ms)",
              total, spots);
    }

    /* a version ahead without the blob: fetched at once, then every 250 ms, four sends */
    t += 10000u;
    n = dc_enc(buf,4,0,99,5004,NULL,2); dc_feed(st,ip,6730,buf,n,t);
    CHECK(i_rant_discovery_next_due_us(st) <= t, "owed: next due is now once a fetch is owed");
    sends = dc_drain(st,t,&fl,&port);
    CHECK(sends == 1 && fl == (F_REQ|F_ONE|F_BLOB) && port == 5004,
          "owed: a version ahead is fetched at once with REQ|ONE and our blob (flags %d)", fl);
    CHECK(dc_drain(st, t + 249000u, &fl, &port) == 0, "owed: an unanswered fetch waits 250 ms");
    for (k = 1; k < 4; k++) sends += dc_drain(st, t + (uint64_t)k*250000u, &fl, &port);
    CHECK(sends == 4 && dc_drain(st, t + 1250000u, &fl, &port) == 0,
          "owed: a fetch goes four times 250 ms apart, then stops (%d)", sends);
    t += 1500000u;
    n = dc_enc(buf,4,0,99,5004,NULL,2); dc_feed(st,ip,6730,buf,n,t);
    sends = dc_drain(st,t,&fl,&port);
    CHECK(sends == 1 && fl == (F_REQ|F_ONE|F_BLOB) && dc_drain(st, t + 250000u, &fl, &port) == 0,
          "owed: after a fetch that went unanswered, a later announce fetches once");
    n = dc_wire(buf,4,F_ONE,99,5004,NULL,2); dc_feed(st,ip,6730,buf,n,t + 260000u);
    t += 500000u;
    n = dc_enc(buf,4,0,99,5004,NULL,3); dc_feed(st,ip,6730,buf,n,t);
    sends = dc_drain(st,t,&fl,&port) + dc_drain(st, t + 250000u, &fl, &port);
    n = dc_wire(buf,4,F_ONE,99,5004,NULL,3); dc_feed(st,ip,6730,buf,n,t + 251000u);
    CHECK(sends == 2 && dc_drain(st, t + 500000u, &fl, &port) == 0,
          "owed: the blob at the advertised version ends a fetch, and the next goes four times again");

    /* a fetch carries our blob, so it settles an answer owed too */
    t += 500000u;
    n = dc_wire(buf,5,F_REQ,99,5005,NULL,1); dc_feed(st,ip,6730,buf,n,t);
    n = dc_enc(buf,5,0,99,5005,NULL,2); dc_feed(st,ip,6730,buf,n,t);
    sends = dc_drain(st,t,&fl,&port);
    CHECK(sends == 1 && fl == (F_REQ|F_ONE|F_BLOB) && dc_drain(st, t + 10000u, &fl, &port) == 0,
          "owed: a fetch settles the answer owed to the same peer (%d sends)", sends);
    n = dc_wire(buf,5,F_ONE,99,5005,NULL,2); dc_feed(st,ip,6730,buf,n,t + 20000u);

    /* an answer too big to hold ends the fetch, since a retry brings the same bytes */
    t += 500000u;
    n = dc_enc(buf,6,0,99,5006,NULL,2); dc_feed(st,ip,6730,buf,n,t);
    sends = dc_drain(st,t,&fl,&port);
    {   uint8_t big[256], ov[100], uuid6[16]; i_RantDiscoveryWire w;
        memset(uuid6, 6, 16); memset(ov, 0, sizeof ov); memset(&w, 0, sizeof w);
        w.flags = (uint8_t)(F_BLOB|F_ONE); w.domain = 99; w.uuid = uuid6; w.version = 2; w.data_port = 5006;
        w.overlay = rant_bytes(ov, sizeof ov);
        n = i_rant_discovery_encode(&w, big, sizeof big);
        dc_feed(st,ip,6730,big,n,t + 1000u);
    }
    CHECK(sends == 1 && dc_drain(st, t + 250000u, &fl, &port) == 0, "owed: an answer too big to hold ends the fetch");

    /* copies: marked UNI to a record heard by a unicast copy within the peer timeout, plain
       to a slotted one, none to a DROPPED one */
    t += 500000u;
    n = dc_wire(buf,7,F_UNI,99,5007,NULL,1); dc_feed(st,ip,6730,buf,n,t);
    k = dc_copy_of(st, 5007, &uni) && uni;
    CHECK(k && dc_copy_of(st, 5008, &uni) && !uni && dc_drain(st,t,&fl,&port) == 0,
          "owed: a record heard by a unicast copy takes copies marked UNI, a slotted one plain copies");
    n = dc_enc(buf,7,0,99,5007,NULL,1); dc_feed(st,ip,6730,buf,n,t + 4500000u);
    CHECK(dc_copy_of(st, 5007, &uni) && !uni, "owed: UNI ends a peer timeout after the last unicast copy");
    n = dc_wire(buf,1,F_REQ,99,5001,NULL,1);
    i_rant_discovery_mark_copy(buf, n, 1); k = dc_sent(buf, n);
    i_rant_discovery_mark_copy(buf, n, 0);
    CHECK(k == (F_REQ|F_BLOB|F_UNI) && dc_sent(buf, n) == (F_REQ|F_BLOB), "owed: mark_copy sets and clears UNI alone");

    /* the sweep runs a few times per interval: a peer silent past the timeout drops within a
       quarter interval, nothing owed goes to it, and the live count follows */
    t += 4500001u;
    i_rant_discovery_update(st, t, out, sizeof out);
    CHECK(i_rant_discovery_peer_count(st) == 1 && !dc_copy_of(st, 5008, &uni),
          "owed: the silent peers drop and take no copies (%u live)", (unsigned)i_rant_discovery_peer_count(st));
    {   uint64_t heard = t, at = 0, now;
        n = dc_wire(buf,7,F_REQ,99,5007,NULL,1); dc_feed(st,ip,6730,buf,n,heard);
        for (now = heard + 3900000u; now < heard + 4400000u && !at; now += 10000u){
            i_rant_discovery_update(st, now, out, sizeof out);
            if (i_rant_discovery_peer_count(st) == 0) at = now;
        }
        CHECK(at > heard + 4000000u && at <= heard + 4260000u && dc_drain(st, at, &fl, &port) == 0,
              "owed: a drop comes within a quarter interval of the timeout and settles what was owed (%u ms late)",
              (unsigned)((at - heard - 4000000u) / 1000u));
        n = dc_enc(buf,7,0,99,5007,NULL,1); dc_feed(st,ip,6730,buf,n,at + 1000u);
        CHECK(i_rant_discovery_peer_count(st) == 1, "owed: a return counts as live again");
        t = at + 1000u;
    }

    /* a change rides one announce, the next is bare again */
    i_rant_discovery_set_meta(st, rant_bytes(NULL, 0), t);
    n = i_rant_discovery_update(st, t, out, sizeof out);
    k = dc_sent(out, n);
    n = i_rant_discovery_update(st, t + c.announce_interval_us + 1u, out, sizeof out);
    CHECK(k == F_BLOB && dc_sent(out, n) == 0, "owed: a change rides one announce (flags %d then %d)", k, dc_sent(out, n));
}

static const TestCase cases[] = {
    { "records",        records_checks },
    { "migrate",        migrate_checks },
    { "owed",           owed_checks },
    { "wire",           wire_checks },
    { "change_age",     change_age_checks },
    { "discovery_core", disc_core_checks },
    { "node_core", node_core_checks },
};

TEST_MAIN(cases)
