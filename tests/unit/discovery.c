/* The discovery core and the node core, sans IO: peer lifecycle, relaying, stated and
 * observed locators, and the node's peer table over a real discovery core with no sockets.
 * spec/testing.md says what each case pins. */
#include "support/test.h"

/* the discovery core's callbacks, recorded for the lifecycle checks */
static uint32_t dc_up_id, dc_up_n, dc_down_id, dc_down_n, dc_refused_n;
static int      dc_down_reason;
static void dc_up(void *user, uint32_t peer, const RantAddr *addr, RantBytes meta){
    (void)user; (void)addr; (void)meta; dc_up_id=peer; dc_up_n++;
}
static void dc_down(void *user, uint32_t peer, i_RantDiscoveryDownReason reason){
    (void)user; dc_down_id=peer; dc_down_reason=(int)reason; dc_down_n++;
}
static void dc_error(const RantEvent *ev){ if (ev->error==RANT_E_PEER_REFUSED) dc_refused_n++; }

/* craft a v3 announce for sender `uid` (uuid = all uid bytes), meta_len 0 */
static size_t dc_mk(uint8_t *p, uint8_t uid, uint8_t flags, uint16_t dom, uint16_t port, uint32_t mver){
    size_t off = RANT_DISCOVERY_META_OFF;
    uint8_t *b = p + off;
    memset(p, 0, off);
    p[0]='u';p[1]='D';p[2]='S';p[3]='C';
    p[4]=(uint8_t)RANT_DISCOVERY_PROTO_VERSION;
    p[5]=flags;                                   /* 0x01 = BYE (internal flag) */
    p[6]=(uint8_t)dom; p[7]=(uint8_t)(dom>>8);
    memset(p+8, uid, 16);                         /* a distinct uuid per uid */
    p[off-6]=(uint8_t)mver; p[off-5]=(uint8_t)(mver>>8);
    p[off-4]=(uint8_t)(mver>>16); p[off-3]=(uint8_t)(mver>>24);
    /* blob = the discovery section [u16 data_port][u8 self_ip_len 0][u8 name_len 0], no
       overlay. self_ip_len 0 means the receiver uses the datagram source ip. */
    b[0]=(uint8_t)port; b[1]=(uint8_t)(port>>8); b[2]=0; b[3]=0;
    p[off-2]=4; p[off-1]=0;                        /* meta_len = 4 (the discovery section) */
    return off + 4;
}

/* craft an announce that states its locator: [u16 port][u8 ip_len 4][ip][u8 name_len 0].
   flags 0x04 = RELAY_ME, 0x08 = PROXIED (a relay speaking for the origin). */
static size_t dc_mk_ip(uint8_t *p, uint8_t uid, uint8_t flags, uint16_t dom, uint16_t port,
                       uint32_t mver, const uint8_t ip[4]){
    size_t off = RANT_DISCOVERY_META_OFF;
    uint8_t *b = p + off;
    memset(p, 0, off);
    p[0]='u';p[1]='D';p[2]='S';p[3]='C';
    p[4]=(uint8_t)RANT_DISCOVERY_PROTO_VERSION;
    p[5]=flags;
    p[6]=(uint8_t)dom; p[7]=(uint8_t)(dom>>8);
    memset(p+8, uid, 16);
    p[off-6]=(uint8_t)mver; p[off-5]=(uint8_t)(mver>>8);
    p[off-4]=(uint8_t)(mver>>16); p[off-3]=(uint8_t)(mver>>24);
    b[0]=(uint8_t)port; b[1]=(uint8_t)(port>>8); b[2]=4;
    memcpy(b+3, ip, 4); b[7]=0;                    /* name_len 0, no overlay */
    p[off-2]=8; p[off-1]=0;                        /* meta_len = 8 */
    return off + 8;
}

/* feed one crafted datagram to a core as if received from (ip, sport). via 0 = the
   discovery socket, 1 = the data port socket */
static void dc_feed(i_RantDiscoveryState *st, const uint8_t ip[4], uint16_t sport, int via,
                    const uint8_t *buf, size_t n, uint64_t now){
    RantAddr src;
    memset(&src, 0, sizeof src);
    memcpy(src.ip, ip, 4); src.ip_len = 4; src.port = sport;
    i_rant_discovery_on_datagram(st, &src,
                               via ? RANT_DISCOVERY_VIA_DATA : RANT_DISCOVERY_VIA_DISCOVERY,
                               rant_bytes(buf, n), now);
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
    n=dc_mk(buf,1,0,99,5001,1); dc_feed(st,sa,7400,0,buf,n,2000); idA=dc_up_id;
    n=dc_mk(buf,2,0,99,5002,1); dc_feed(st,sb,7400,0,buf,n,2000); idB=dc_up_id;
    CHECK(dc_up_n==2 && i_rant_discovery_peer_count(st)==2,
             "disc-core: two peers up (ups=%u count=%u)", dc_up_n, i_rant_discovery_peer_count(st));

    /* A new peer is REFUSED when the table is full of ACTIVE peers */
    dc_up_n=dc_refused_n=0;
    n=dc_mk(buf,3,0,99,5003,1); dc_feed(st,sc,7400,0,buf,n,2000);
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
    n=dc_mk(buf,1,0,99,5001,1); dc_feed(st,sa,7400,0,buf,n,2100000);
    CHECK(dc_up_n==1 && dc_up_id==idA && i_rant_discovery_peer_count(st)==1,
             "disc-core: same uuid resumes same id (up=%u sameid=%d count=%u)",
             dc_up_n, dc_up_id==idA, i_rant_discovery_peer_count(st));

    /* A new peer now evicts the oldest DROPPED peer (B) as GONE */
    dc_down_n=dc_up_n=0;
    n=dc_mk(buf,3,0,99,5003,1); dc_feed(st,sc,7400,0,buf,n,2100000);
    CHECK(dc_down_n==1 && dc_down_id==idB && dc_down_reason==(int)RANT_DISCOVERY_GONE && dc_up_n==1,
             "disc-core: new peer evicts oldest dropped as GONE (downs=%u sameid=%d reason=%d up=%u)",
             dc_down_n, dc_down_id==idB, dc_down_reason, dc_up_n);

    /* BYE is GONE */
    dc_down_n=0;
    n=dc_mk(buf,1,0x01,99,5001,1); dc_feed(st,sa,7400,0,buf,n,2100000);
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
      n=dc_mk(buf,7,0,99,6001,1); dc_feed(st,sa,7400,0,buf,n,3000); idP=dc_up_id;
      dc_up_n=dc_down_n=0;
      n=dc_mk(buf,8,0,99,6001,1); dc_feed(st,sa,7400,0,buf,n,3100);  /* new uuid, same ip:port */
      CHECK(dc_down_n==1 && dc_down_id==idP && dc_down_reason==(int)RANT_DISCOVERY_GONE
               && dc_up_n==1 && i_rant_discovery_peer_count(st)==1,
               "disc-core: new uuid at a held ip:port evicts the predecessor (downs=%u sameid=%d reason=%d up=%u count=%u)",
               dc_down_n, dc_down_id==idP, dc_down_reason, dc_up_n, i_rant_discovery_peer_count(st));
    }

    /* RELAY, the rules the loopback phase cannot see: who enlists us, one hop only, and
       a proxied locator losing to a direct path we still hear */
    { uint32_t id9; RantAddr a; size_t pn; uint16_t pport;
      st = i_rant_discovery_init(mem,sizeof mem,&c);              /* fresh receiver */
      i_rant_discovery_update(st, 1000, out, sizeof out);
      /* (a) a DIRECT relay-me announce enlists us: we owe a proxied announce for it */
      n=dc_mk(buf,9,0x04,99,7001,1); dc_feed(st,sa,7400,0,buf,n,3000);
      id9=dc_up_id;
      pn = i_rant_discovery_poll_relay(st, out, sizeof out);
      /* PROXIED is the loop stop, 8b proves it defeats enlistment even with RELAY_ME set.
         RELAY_ME rides along as origin info, so a stated locator is no endpoint identity */
      CHECK(pn > 0 && (out[5] & 0x08) && (out[5] & 0x04),
               "disc-core: relay-me peer is proxied PROXIED, origin's RELAY_ME carried (n=%u flags=0x%02X)",
               (unsigned)pn, (unsigned)(pn ? out[5] : 0));
      pport = pn ? (uint16_t)(out[RANT_DISCOVERY_META_OFF] |
                             ((uint16_t)out[RANT_DISCOVERY_META_OFF+1] << 8)) : 0;
      CHECK(pn > 0 && out[8]==9 && out[RANT_DISCOVERY_META_OFF+2]==4 &&
               memcmp(out+RANT_DISCOVERY_META_OFF+3, sa, 4)==0 && pport==7001,
               "disc-core: the proxy carries the ORIGIN's uuid + the locator we hold (uuid=%u port=%u)",
               (unsigned)(pn ? out[8] : 0), (unsigned)pport);
      CHECK(i_rant_discovery_poll_relay(st, out, sizeof out)==0,
               "disc-core: one proxied announce per peer per interval");
      /* (b) a PROXIED announce never enlists a SECOND hop, even carrying RELAY_ME: a
             peer we know only second-hand is not ours to introduce (the loop stop) */
      n=dc_mk_ip(buf,11,(uint8_t)(0x08|0x04),99,7011,1,sc);
      dc_feed(st,sb,7400,0,buf,n,3100);
      CHECK(i_rant_discovery_peer_count(st)==2 && i_rant_discovery_poll_relay(st,out,sizeof out)==0,
               "disc-core: a proxied announce never enlists a second relay hop");
      /* (c) a proxied locator is a CANDIDATE: the direct path we are still hearing wins,
             so a relay's view cannot flap the address a peer's data is unicast to */
      n=dc_mk_ip(buf,9,0x08,99,7001,2,sb);        /* the relay says peer 9 is at 10.0.0.2 */
      dc_feed(st,sc,7400,0,buf,n,3200);
      memset(&a,0,sizeof a); i_rant_discovery_addr_of_id(st, id9, &a);
      CHECK(memcmp(a.ip, sa, 4)==0,
               "disc-core: a proxied locator never stomps a live direct one (%u.%u.%u.%u)",
               a.ip[0], a.ip[1], a.ip[2], a.ip[3]);
      /* (d) contrast: the same stated address WITHOUT the proxied flag is the peer
             speaking for itself, which stays authoritative and does move it */
      n=dc_mk_ip(buf,9,0,99,7001,3,sb);
      dc_feed(st,sc,7400,0,buf,n,3300);
      memset(&a,0,sizeof a); i_rant_discovery_addr_of_id(st, id9, &a);
      CHECK(memcmp(a.ip, sb, 4)==0,
               "disc-core: ...but a peer's OWN stated locator still is (%u.%u.%u.%u)",
               a.ip[0], a.ip[1], a.ip[2], a.ip[3]);
    }

    /* SELF-IP: a node states its locator outright. It must ride our own announce, and a
       relay must propagate the stated address, not the source it saw. */
    { i_RantDiscoveryCoreConfig sc2; i_RantDiscoveryState *s2; RantAddr a;
      uint8_t pub_ip[4]={203,0,113,7}; uint32_t id12; size_t pn;
      sc2 = c;                                     /* same domain/timing, our own uuid */
      memcpy(sc2.self_ip, pub_ip, 4); sc2.self_ip_len = 4; sc2.data_port = 7400;
      s2 = i_rant_discovery_init(mem,sizeof mem,&sc2);
      CHECK(s2!=NULL, "disc-core: self-ip core init");
      if (s2){
          n = i_rant_discovery_update(s2, 1000, out, sizeof out);          /* start: solicit + blob */
          CHECK(n > (size_t)RANT_DISCOVERY_META_OFF + 3 &&
                   out[RANT_DISCOVERY_META_OFF+2]==4 &&
                   memcmp(out+RANT_DISCOVERY_META_OFF+3, pub_ip, 4)==0,
                   "disc-core: our announce states the configured self ip");
      }
      /* the relay side: peer 12 announces RELAY_ME FROM sa while STATING pub_ip. The relay
         must hold (and hand on) the stated address, so the override survives the hop. */
      st = i_rant_discovery_init(mem+4096,sizeof mem-4096,&c);
      i_rant_discovery_update(st, 1000, out, sizeof out);
      n=dc_mk_ip(buf,12,0x04,99,7400,1,pub_ip);
      dc_feed(st,sa,7400,0,buf,n,4000);      /* source sa, states pub_ip */
      id12=dc_up_id;
      memset(&a,0,sizeof a); i_rant_discovery_addr_of_id(st, id12, &a);
      CHECK(memcmp(a.ip, pub_ip, 4)==0,
               "disc-core: a stated locator beats the source it arrived from (%u.%u.%u.%u)",
               a.ip[0], a.ip[1], a.ip[2], a.ip[3]);
      pn = i_rant_discovery_poll_relay(st, out, sizeof out);
      CHECK(pn > 0 && memcmp(out+RANT_DISCOVERY_META_OFF+3, pub_ip, 4)==0,
               "disc-core: the relay proxies the STATED locator, not the source it saw");
    }

    /* OBSERVED SOURCES: a unicast only peer behind a NAT advertises a fiction, so the
       receiver binds one observed source per local channel and routes everything there. */
    { i_RantDiscoveryCoreConfig c4; RantAddr a; uint32_t id20, id21, id22, idX, idU;
      uint8_t na[4]={192,168,1,50}, sx[4]={192,168,1,200}, su[4]={192,168,1,51};
      size_t pn; int ex;
      c4 = c; c4.max_peers = 4;
      st = i_rant_discovery_init(mem,sizeof mem,&c4);
      i_rant_discovery_update(st, 1000, out, sizeof out);
      /* (a) bind and route: a RELAY_ME announce arrives on our data channel from (na, 33333).
         Data goes to the observed source, attribution refuses the phantom locator. */
      dc_up_n=0;
      n=dc_mk(buf,20,0x04,99,6000,1); dc_feed(st,na,33333,1,buf,n,5000); id20=dc_up_id;
      memset(&a,0,sizeof a); i_rant_discovery_addr_of_id(st, id20, &a);
      CHECK(dc_up_n==1 && a.port==33333 && memcmp(a.ip,na,4)==0,
               "disc-core: data routes to the observed source, not the locator (port=%u)", a.port);
      CHECK(i_rant_discovery_id_for_addr(st,na,4,33333,NULL) &&
               !i_rant_discovery_id_for_addr(st,na,4,6000,NULL),
               "disc-core: attribution accepts the observed source, refuses the phantom locator");
      /* (b) the discovery channel binds independently: the same announce heard on our
         discovery socket from another flow, so discovery TX hits exactly that endpoint */
      n=dc_mk(buf,20,0x04,99,6000,1); dc_feed(st,na,44444,0,buf,n,5001);
      { int k = i_rant_discovery_peer_addr(st, 0, &a);
        CHECK(k==2 && a.port==44444,
                 "disc-core: discovery TX targets the discovery-channel source exactly (k=%d port=%u)",
                 k, a.port); }
      /* (c) quiet rebind: the NAT expired and the next announce arrives from a new flow.
         Sends follow at once and no peer_up refires. */
      dc_up_n=0;
      n=dc_mk(buf,20,0x04,99,6000,1); dc_feed(st,na,55555,1,buf,n,5002);
      memset(&a,0,sizeof a); i_rant_discovery_addr_of_id(st, id20, &a);
      CHECK(a.port==55555 && dc_up_n==0,
               "disc-core: a rebound mapping redirects sends quietly (port=%u ups=%u)", a.port, dc_up_n);
      /* (d) two NAT'd containers on ONE device advertise the SAME phantom locator: they
         must coexist (neither is that endpoint), distinguished by their observed sources */
      dc_down_n=0; dc_up_n=0;
      n=dc_mk(buf,21,0x04,99,6000,1); dc_feed(st,na,55666,1,buf,n,5003); id21=dc_up_id;
      CHECK(dc_up_n==1 && dc_down_n==0 && i_rant_discovery_peer_count(st)==2,
               "disc-core: same phantom locator never evicts a NAT'd sibling (downs=%u count=%u)",
               dc_down_n, i_rant_discovery_peer_count(st));
      { uint32_t got=0;
        CHECK(i_rant_discovery_id_for_addr(st,na,4,55666,&got) && got==id21 &&
                 i_rant_discovery_id_for_addr(st,na,4,55555,&got) && got==id20,
                 "disc-core: observed sources tell the siblings apart"); }
      /* (e) ...but a NEW uuid arriving from an endpoint held as some peer's OBSERVED
         source is that socket reused by a new process: the old occupant is evicted */
      dc_down_n=0;
      n=dc_mk(buf,22,0x04,99,6000,1); dc_feed(st,na,55666,1,buf,n,5004); id22=dc_up_id;
      CHECK(dc_down_n==1 && dc_down_id==id21 && dc_down_reason==(int)RANT_DISCOVERY_GONE,
               "disc-core: a reused observed endpoint evicts its old occupant (downs=%u sameid=%d)",
               dc_down_n, dc_down_id==id21);
      (void)id22;
      /* (f) a stated self_ip is authoritative: observed never binds over the operator's
         asserted locator, even for a RELAY_ME sender */
      { uint8_t pub2[4]={203,0,113,9};
        n=dc_mk_ip(buf,23,0x04,99,6001,1,pub2); dc_feed(st,su,60123,1,buf,n,5005);
        memset(&a,0,sizeof a); i_rant_discovery_addr_of_id(st, dc_up_id, &a);
        CHECK(memcmp(a.ip,pub2,4)==0 && a.port==6001,
                 "disc-core: a stated self_ip beats the observed source (%u.%u.%u.%u:%u)",
                 a.ip[0],a.ip[1],a.ip[2],a.ip[3],a.port); }
      /* (g) INTRODUCTIONS: the inbound half of relaying. A relay-me peer is told who we
         hear directly, at its observed source, and re-told when someone new appears. */
      c4.max_peers = 6;
      st = i_rant_discovery_init(mem,sizeof mem,&c4);          /* fresh relayer */
      i_rant_discovery_update(st, 1000, out, sizeof out);
      n=dc_mk(buf,30,0,99,7000,1); dc_feed(st,sx,7400,0,buf,n,6000); idX=dc_up_id;   /* peer X */
      n=dc_mk(buf,31,0x04,99,6000,1); dc_feed(st,su,50001,1,buf,n,6001); idU=dc_up_id; /* relay U */
      pn = i_rant_discovery_poll_introduce(st, out, sizeof out, &a, &ex);
      CHECK(pn > 0 && out[8]==30 && ex==1 && a.port==50001 && memcmp(a.ip,su,4)==0,
               "disc-core: a new relay-me peer is introduced to everyone we hear (uuid=%u to=%u)",
               (unsigned)(pn?out[8]:0), a.port);
      CHECK(i_rant_discovery_poll_introduce(st, out, sizeof out, &a, &ex)==0,
               "disc-core: ...exactly once (change-triggered, not periodic)");
      /* a newcomer re-triggers a walk to every relay-me peer */
      n=dc_mk(buf,32,0,99,7002,1); dc_feed(st,sx,7401,0,buf,n,6002);
      { int saw32=0, k2;
        for (k2=0;k2<8;k2++){
            pn = i_rant_discovery_poll_introduce(st, out, sizeof out, &a, &ex);
            if (!pn) break;
            if (out[8]==32) saw32=1;
        }
        CHECK(saw32, "disc-core: a newcomer is introduced to existing relay-me peers"); }
      /* a peer known only from a PROXY is not ours to introduce (the one-hop rule) */
      n=dc_mk_ip(buf,33,0x08,99,7003,1,sx); dc_feed(st,sx,7400,0,buf,n,6003);
      { int saw33=0, k2;
        n=dc_mk(buf,34,0,99,7004,1); dc_feed(st,sx,7402,0,buf,n,6004);   /* re-trigger a walk */
        for (k2=0;k2<8;k2++){
            pn = i_rant_discovery_poll_introduce(st, out, sizeof out, &a, &ex);
            if (!pn) break;
            if (out[8]==33) saw33=1;
        }
        CHECK(!saw33, "disc-core: a proxy-heard peer is never introduced onward"); }
      (void)idX; (void)idU;
    }

    /* GHOST GATE: relaying is sustained by direct liveness only. Proxies still refresh
       plain liveness, so the entry outlives the origin by one timeout, then dies. */
    { i_RantDiscoveryCoreConfig c5; uint64_t t; size_t pn; int round, relayed;
      c5 = c; c5.max_peers = 4;
      st = i_rant_discovery_init(mem,sizeof mem,&c5);
      i_rant_discovery_update(st, 1000, out, sizeof out);
      n=dc_mk(buf,40,0x04,99,8000,1); dc_feed(st,sa,7400,0,buf,n,1000);     /* direct relay-me */
      n=dc_mk(buf,40,0x04,99,8000,1); dc_feed(st,sa,7400,0,buf,n,900000);   /* still direct */
      t = 1001000;                       /* first announce tick: direct heard 101ms ago */
      i_rant_discovery_update(st, t, out, sizeof out);
      pn = i_rant_discovery_poll_relay(st, out, sizeof out);
      CHECK(pn > 0 && out[8]==40, "disc-core: a directly heard relay-me peer is relayed");
      /* the origin dies while other relays' PROXIED announces keep arriving. They keep the
         entry alive but must never keep us relaying it. */
      relayed = 0;
      for (round = 0; round < 3; round++){
          t += 1000000;
          n=dc_mk_ip(buf,40,(uint8_t)(0x08|0x04),99,8000,1,sa);
          dc_feed(st,sb,7400,0,buf,n,t);
          i_rant_discovery_update(st, t + 1000, out, sizeof out);
          if (i_rant_discovery_poll_relay(st, out, sizeof out)) relayed = 1;
      }
      CHECK(!relayed && i_rant_discovery_peer_count(st)==1,
               "disc-core: proxies keep a peer alive but never sustain relaying (count=%u)",
               i_rant_discovery_peer_count(st));
      /* nor is a direct-stale origin introduced to a newly appearing relay-me peer */
      n=dc_mk(buf,41,0x04,99,8001,1); dc_feed(st,sb,50002,1,buf,n,t + 2000);
      { int saw40=0, k2; RantAddr a2; int ex2;
        for (k2=0;k2<8;k2++){
            pn = i_rant_discovery_poll_introduce(st, out, sizeof out, &a2, &ex2);
            if (!pn) break;
            if (out[8]==40) saw40=1;
        }
        CHECK(!saw40, "disc-core: a direct-stale peer is not ours to introduce"); }
      /* the proxies stop (every relay's own direct went stale): the ghost now times out */
      dc_down_n=0;
      i_rant_discovery_update(st, t + 2200000, out, sizeof out);
      CHECK(dc_down_n>=1, "disc-core: the ghost dies once proxies cease (downs=%u)", dc_down_n);
    }

    /* A UNICAST-ONLY RECEIVER: a published port forward rewrites the source to the
       gateway on our own subnet, so a source never overrides a held locator here. */
    { i_RantDiscoveryCoreConfig c6; i_RantDiscoverySubnet net; RantAddr a;
      i_RantDiscoveryPeerView v; uint32_t id50, got;
      static const uint8_t gw[4]={10,0,2,2}, real_ip[4]={192,168,1,106};
      static const uint8_t net_ip[4]={10,0,2,0}, net_mask[4]={255,255,255,0};
      c6 = c; c6.max_peers = 4; c6.relay_me = 1;         /* we are the NAT'd node */
      st = i_rant_discovery_init(mem,sizeof mem,&c6);
      memcpy(net.ip, net_ip, 4); memcpy(net.mask, net_mask, 4);
      i_rant_discovery_set_local_subnets(st, &net, 1);       /* the container's own subnet */
      i_rant_discovery_update(st, 1000, out, sizeof out);
      /* (a) an introduction states the peer's true routed locator */
      n=dc_mk_ip(buf,50,0x08,99,5555,1,real_ip); dc_feed(st,sa,7400,0,buf,n,9000);
      id50=dc_up_id;
      /* (b) its direct announce arrives through our port forward: the source is the gateway
         on our own subnet, rank 2 against the real address's rank 1. The locator must not move. */
      n=dc_mk(buf,50,0,99,5555,2); dc_feed(st,gw,33445,1,buf,n,9001);
      { int have = i_rant_discovery_peer_at(st, 0, &v);
        CHECK(have && memcmp(v.addr.ip,real_ip,4)==0 && v.addr.port==5555,
                 "disc-core: a forwarded source never overrides the held locator (%u.%u.%u.%u:%u)",
                 v.addr.ip[0],v.addr.ip[1],v.addr.ip[2],v.addr.ip[3],v.addr.port); }
      /* (c) ...but it IS the peer's return path: bound as the observed source even for
         an ordinary (non-relay-me) peer, routing data + attribution through the flow */
      memset(&a,0,sizeof a);
      CHECK(i_rant_discovery_addr_of_id(st, id50, &a) && a.port==33445 && memcmp(a.ip,gw,4)==0,
               "disc-core: a unicast-only node routes every peer via its observed return path (port=%u)",
               a.port);
      got=0;
      CHECK(i_rant_discovery_id_for_addr(st,gw,4,33445,&got) && got==id50,
               "disc-core: ...and attributes arrivals from that flow to the peer");
    }

    /* DUPLICATE SUPPRESSION: two foreign proxies within one announce interval suppress
       our own emission, our looped echo never counts, and a changed origin re arms it. */
    { i_RantDiscoveryCoreConfig c7; uint8_t mine[RANT_DISCOVERY_WIRE_MAX];
      size_t mn, pn2;
      c7 = c; c7.max_peers = 4;
      st = i_rant_discovery_init(mem,sizeof mem,&c7);
      i_rant_discovery_update(st, 1000, out, sizeof out);
      n=dc_mk(buf,60,0x04,99,8000,1); dc_feed(st,sa,7400,0,buf,n,1000);   /* direct relay-me */
      mn = i_rant_discovery_poll_relay(st, mine, sizeof mine);
      CHECK(mn > 16 && mine[8]==60 && memcmp(mine + mn - 16, c7.uuid, 16)==0,
               "disc-core: our proxy carries our relayer trailer (n=%u)", (unsigned)mn);
      /* our own echo comes back, plus ONE foreign proxy (old-style, trailer-less) */
      dc_feed(st,sc,7400,0,mine,mn,2000);                                  /* self echo */
      n=dc_mk_ip(buf,60,(uint8_t)(0x08|0x04),99,8000,1,sa);
      dc_feed(st,sb,7400,0,buf,n,2001);                                    /* 1 foreign */
      n=dc_mk(buf,60,0x04,99,8000,1); dc_feed(st,sa,7400,0,buf,n,900000);  /* stay direct-fresh */
      i_rant_discovery_update(st, 1001000, out, sizeof out);
      CHECK(i_rant_discovery_poll_relay(st, out, sizeof out) > 0,
               "disc-core: one foreign proxy does not suppress; a self echo never counts");
      /* two foreign proxies within the interval: quorum exists, we sit this one out */
      n=dc_mk_ip(buf,60,(uint8_t)(0x08|0x04),99,8000,1,sa);
      dc_feed(st,sb,7400,0,buf,n,1500000);
      dc_feed(st,sc,7400,0,buf,n,1600000);
      n=dc_mk(buf,60,0x04,99,8000,1); dc_feed(st,sa,7400,0,buf,n,1900000);
      i_rant_discovery_update(st, 2001000, out, sizeof out);
      CHECK(i_rant_discovery_poll_relay(st, out, sizeof out) == 0,
               "disc-core: a relay quorum suppresses our periodic emission");
      /* the quorum went quiet: the next tick re-enlists us */
      n=dc_mk(buf,60,0x04,99,8000,1); dc_feed(st,sa,7400,0,buf,n,2900000);
      i_rant_discovery_update(st, 3001000, out, sizeof out);
      pn2 = i_rant_discovery_poll_relay(st, out, sizeof out);
      CHECK(pn2 > 0, "disc-core: relay silence re-enlists us at the next tick");
      /* a CHANGED origin bypasses suppression: news beats steady-state thinning */
      n=dc_mk_ip(buf,60,(uint8_t)(0x08|0x04),99,8000,1,sa);
      dc_feed(st,sb,7400,0,buf,n,3200000);
      dc_feed(st,sc,7400,0,buf,n,3200001);
      n=dc_mk(buf,60,0x04,99,8000,2); dc_feed(st,sa,7400,0,buf,n,3300000);  /* blob v2 */
      CHECK(i_rant_discovery_poll_relay(st, out, sizeof out) > 0,
               "disc-core: a changed origin is relayed at once despite a quorum");
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
/* feed the node core through a real sans IO discovery core: build an announce datagram
   with no overlay so discovery fires events into the node core. flags 0x01 = BYE. */
static size_t nc_dgram(uint8_t *p, uint8_t uid, uint8_t flags, uint16_t dom, uint16_t port,
                       const char *name, uint32_t mver){
    size_t off = RANT_DISCOVERY_META_OFF;
    uint8_t *b = p + off;
    uint8_t nl = name ? (uint8_t)strlen(name) : 0;
    uint16_t ml = (uint16_t)(4u + nl);
    memset(p, 0, off);
    p[0]='u';p[1]='D';p[2]='S';p[3]='C';
    p[4]=(uint8_t)RANT_DISCOVERY_PROTO_VERSION;
    p[5]=flags;
    p[6]=(uint8_t)dom; p[7]=(uint8_t)(dom>>8);
    memset(p+8, uid, 16);                          /* a distinct uuid per uid */
    p[off-6]=(uint8_t)mver; p[off-5]=(uint8_t)(mver>>8);
    p[off-4]=(uint8_t)(mver>>16); p[off-3]=(uint8_t)(mver>>24);
    b[0]=(uint8_t)port; b[1]=(uint8_t)(port>>8); b[2]=0; b[3]=nl;   /* port, no self ip, name_len */
    if (nl) memcpy(b+4, name, nl);
    p[off-2]=(uint8_t)ml; p[off-1]=(uint8_t)(ml>>8);
    return off + ml;
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
    n=nc_dgram(buf,1,0,99,5001,"nc-self",1); dc_feed(st,sa,7400,0,buf,n,2000); idA=nc_up_id;
    n=nc_dgram(buf,2,0,99,5002,"nc-self",1); dc_feed(st,sb,7400,0,buf,n,2000); idB=nc_up_id;
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
        n=nc_dgram(buf,1,0,99,5001,NULL,2);     dc_feed(st,sa,7400,0,buf,n,2001);
        pn = i_rant_node_core_peer_name(nc, idA);
        CHECK(pn.data && pn.len==12 && memcmp(pn.data,"unknown-peer",12)==0,
                 "node-core: nameless announce -> unknown-peer (%.*s)", (int)pn.len, pn.data?pn.data:"(null)");
        n=nc_dgram(buf,1,0,99,5001,"nc-self",3); dc_feed(st,sa,7400,0,buf,n,2002); }

    /* A third peer is REFUSED while the table is full of ACTIVE peers, the node forwards it */
    nc_refused_n=0;
    n=nc_dgram(buf,3,0,99,5003,"three",1); dc_feed(st,sc,7400,0,buf,n,2003);
    CHECK(nc_refused_n==1, "node-core: refused forwarded (refused=%u)", nc_refused_n);

    /* Silence past the timeout DROPS both: a PEER_DOWN each, but the slots are kept (resolve) */
    nc_down_n=0;
    i_rant_discovery_update(st, 1003000, out, sizeof out);
    CHECK(nc_down_n==2, "node-core: timeout drops both (downs=%u)", nc_down_n);
    CHECK(i_rant_node_core_resolve(nc,idA,&d)==1, "node-core: dropped peer kept (resolves)");

    /* The same uuid returns: RESUME re fires PEER_UP under the same id */
    nc_up_n=0;
    n=nc_dgram(buf,1,0,99,5001,"nc-self",4); dc_feed(st,sa,7400,0,buf,n,1100000);
    CHECK(nc_up_n==1 && nc_up_id==idA, "node-core: resume re-ups same id (ups=%u id=%u)", nc_up_n, nc_up_id);

    /* GONE (BYE) on the active peer: one PEER_DOWN, and it does not resolve any more */
    nc_down_n=0;
    n=nc_dgram(buf,1,0x01,99,5001,NULL,1); dc_feed(st,sa,7400,0,buf,n,1100001);
    CHECK(nc_down_n==1, "node-core: GONE on active fires down (downs=%u)", nc_down_n);
    CHECK(i_rant_node_core_resolve(nc,idA,&d)==0, "node-core: GONE peer freed (no resolve)");
}

static const TestCase cases[] = {
    { "discovery_core", disc_core_checks },
    { "node_core", node_core_checks },
};

TEST_MAIN(cases)
