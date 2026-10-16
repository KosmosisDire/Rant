/* Reaching nodes that cannot hear multicast: a relay introduces a unicast only node, a
 * node behind a NAT, and a node stating its own locator. spec/testing.md says more. */
#include "support/test.h"

/* The relay phase: discovery for a node that cannot multicast. U opens unicast_only with
 * one seed, R's data port, and every U to B fact arrives through R's proxied announces. */
static unsigned long rly_recv = 0;
static char rly_last[64];
static void rly_on_message(const RantMsg *msg){
    size_t k = msg->data.len < sizeof rly_last - 1 ? msg->data.len : sizeof rly_last - 1;
    memcpy(rly_last, msg->data.data, k); rly_last[k] = '\0';
    rly_recv++;
}
/* is `name` an ACTIVE peer of n? (the peer view is discovery's own, valid until the next poll) */
static int rly_sees(RantNode *n, const char *name){
    uint16_t c = 0, i; size_t nl = strlen(name);
    const i_RantDiscoveryPeerView *p = test_peers(n, &c);
    for (i=0;i<c;i++)
        if (p[i].liveness == RANT_PEER_ACTIVE && p[i].name.len == nl &&
            memcmp(p[i].name.data, name, nl) == 0) return 1;
    return 0;
}
static void relay_checks(void){
    /* R's data port is fixed so U can seed exactly one address. The domain base keeps
       parallel programs apart */
    const uint16_t R_PORT = test_free_port((uint16_t)(20000u + test_domain_base % 15000u));
    RantAddr seed;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4; seed.port=R_PORT;

    /* (a) INTRODUCTION + DATA, then the relay dies */
    { RantAllocator ua = rant_allocator_heap(0);
      RantAllocator ra = rant_allocator_heap(0);
      RantAllocator ba = rant_allocator_heap(0);
      RantNodeOpts uo, ro, bo; RantNode *U=NULL, *R=NULL, *B=NULL;
      RantTopic *ut=NULL, *bt=NULL;
      memset(&ro,0,sizeof ro); ro.domain=TEST_DOMAIN+27; ro.disable_shm=1; ro.discovery.max_peers=4;
      ro.discovery.announce_interval_us=200000;
      ro.net.multicast_interface="127.0.0.1";
      ro.net.data_port=R_PORT;                 /* the one address anybody ever states */
      bo=ro; bo.net.data_port=0;
      uo=ro; uo.net.data_port=0;
      uo.net.unicast_only=1;                    /* no group join, no group sends, RELAY_ME */
      uo.net.seed_peers=&seed; uo.net.n_seed_peers=1;
      rly_recv=0; rly_last[0]='\0';

      R = rant_node_open(&ra, "rly-relay", NULL, NULL, &ro);
      CHECK(R!=NULL, "relay: relay node opens (is port %d free?)", (int)R_PORT);
      B = rant_node_open(&ba, "rly-far", rly_on_message, NULL, &bo);
      U = rant_node_open(&ua, "rly-uni", NULL, NULL, &uo);
      CHECK(U!=NULL, "relay: unicast-only node opens with no multicast membership");
      CHECK(B!=NULL, "relay: far node opens");
      if (U && R && B){
          bt = rant_node_create_topic(B, "rly/t", RANT_SUB_ONLY, NULL,
                   &(RantTopicOpts){ .qos={ .reliability=RANT_RELIABLE } });
          ut = rant_node_create_topic(U, "rly/t", RANT_PUB_ONLY, NULL,
                   &(RantTopicOpts){ .qos={ .reliability=RANT_RELIABLE, .keep_last=8 } });
          CHECK(ut && bt, "relay: topics up");

          PUMP3_UNTIL(U, R, B, 10000, rly_sees(B,"rly-uni") && rly_sees(U,"rly-far"));
          CHECK(rly_sees(B,"rly-uni"),
                   "relay: far node discovers the unicast-only node it cannot hear");
          CHECK(rly_sees(U,"rly-far"),
                   "relay: and the unicast-only node discovers it back");

          if (ut && bt){
              int r;
              PUMP3_UNTIL(U, R, B, 10000, rant_topic_match_count(ut)!=0);
              CHECK(rant_topic_match_count(ut)==1, "relay: topic matches across the introduction (%d)",
                       rant_topic_match_count(ut));
              r = rant_topic_send(ut, rant_bytes("over-unicast", 12), NULL);
              PUMP3_UNTIL(U, R, B, 10000, rly_recv!=0);
              CHECK(r==RANT_OK && rly_recv==1 && strcmp(rly_last,"over-unicast")==0,
                       "relay: data flows unicast end to end (r=%d recv=%lu '%s')", r, rly_recv, rly_last);
          }

          /* the relay only INTRODUCED: it is not in the data path and not needed to keep
             the pair alive, since each now unicasts its announces to the other directly */
          rant_node_close(R,1); R=NULL;
          test_pump(U, B, 1500);                        /* past the 800 ms peer timeout */
          CHECK(rly_sees(B,"rly-uni") && rly_sees(U,"rly-far"),
                   "relay: the pair outlives the relay (B sees U=%d, U sees B=%d)",
                   rly_sees(B,"rly-uni"), rly_sees(U,"rly-far"));
          CHECK(!rly_sees(B,"rly-relay") && !rly_sees(U,"rly-relay"),
                   "relay: ...and the departed relay itself is gone from both");
          if (ut && bt){
              int r = rant_topic_send(ut, rant_bytes("after-relay-died", 16), NULL);
              PUMP_UNTIL(U, B, 10000, rly_recv >= 2);
              CHECK(r==RANT_OK && rly_recv==2 && strcmp(rly_last,"after-relay-died")==0,
                       "relay: data still flows with the relay dead (r=%d recv=%lu '%s')",
                       r, rly_recv, rly_last);
          }
      }
      if (R) rant_node_close(R,1);
      if (U) rant_node_close(U,1);
      if (B) rant_node_close(B,1);
      rant_allocator_reset(&ua); rant_allocator_reset(&ra); rant_allocator_reset(&ba);
    }

    /* (b) CONTROL: the same unicast only node with nobody to relay it is unreachable in
       both directions, so (a) measured the relay, not the loopback */
    { RantAllocator ua = rant_allocator_heap(0);
      RantAllocator ba = rant_allocator_heap(0);
      RantNodeOpts uo, bo; RantNode *U=NULL, *B=NULL;
      memset(&bo,0,sizeof bo); bo.domain=TEST_DOMAIN+28; bo.disable_shm=1; bo.discovery.max_peers=4;
      bo.discovery.announce_interval_us=200000;
      bo.net.multicast_interface="127.0.0.1";
      uo=bo; uo.net.unicast_only=1;              /* no seeds: it can announce to nobody at all */
      U = rant_node_open(&ua, "rly-alone", NULL, NULL, &uo);
      B = rant_node_open(&ba, "rly-far2", NULL, NULL, &bo);
      CHECK(U && B, "relay: control pair up");
      if (U && B){
          test_pump(U, B, 800);                   /* several announce intervals each way */
          CHECK(!rly_sees(B,"rly-alone") && !rly_sees(U,"rly-far2"),
                   "relay: unrelayed unicast-only node stays invisible (B sees U=%d, U sees B=%d)",
                   rly_sees(B,"rly-alone"), rly_sees(U,"rly-far2"));
      }
      if (U) rant_node_close(U,1);
      if (B) rant_node_close(B,1);
      rant_allocator_reset(&ua); rant_allocator_reset(&ba);
    }
}

/* The NAT phase: a unicast only node behind an outbound only NAT. U advertises a black
 * holed port, so only the observed sources can carry the mesh (spec/testing.md). */
static void nat_checks(void){
    const uint16_t R2_PORT = test_free_port((uint16_t)(21000u + test_domain_base % 15000u));
    uint16_t DEAD_PORT = 0;
    RantAddr seed;
    i_RantSock bh;
    memset(&seed,0,sizeof seed); seed.ip[0]=127; seed.ip[3]=1; seed.ip_len=4; seed.port=R2_PORT;

    /* the black hole owns U's advertised port and never reads it, so every datagram sent to
       U's locator disappears like a NAT drop. OS assigned, so it never collides. */
    i_rant_plat_startup();            /* raw socket outside any node: needs the net stack up */
    bh = i_rant_plat_udp_open();
    if (bh != RANT_SOCK_BAD){
        if (i_rant_plat_bind(bh, i_rant_plat_ipv4(127,0,0,1), 0, 0)) DEAD_PORT = i_rant_plat_local_port(bh);
        if (!DEAD_PORT){ i_rant_plat_close(bh); bh = RANT_SOCK_BAD; }
    }
    CHECK(DEAD_PORT != 0, "nat: black-hole port binds");

    { RantAllocator ua = rant_allocator_heap(0);
      RantAllocator ra = rant_allocator_heap(0);
      RantAllocator ba = rant_allocator_heap(0);
      RantNodeOpts uo, ro, bo; RantNode *U=NULL, *R=NULL, *B=NULL;
      RantTopic *ut=NULL, *bt=NULL;
      memset(&ro,0,sizeof ro); ro.domain=TEST_DOMAIN+29; ro.disable_shm=1; ro.discovery.max_peers=4;
      ro.discovery.announce_interval_us=200000;
      ro.net.multicast_interface="127.0.0.1";
      ro.net.data_port=R2_PORT;                /* the seeded address */
      bo=ro; bo.net.data_port=0;
      uo=ro; uo.net.data_port=0;
      uo.net.unicast_only=1;
      uo.net.advertise_port=DEAD_PORT;         /* the locator lie: what a NAT'd node's blob
                                                  port amounts to on the outside */
      uo.net.seed_peers=&seed; uo.net.n_seed_peers=1;
      rly_recv=0; rly_last[0]='\0';

      R = rant_node_open(&ra, "nat-relay", NULL, NULL, &ro);
      B = rant_node_open(&ba, "nat-far", rly_on_message, NULL, &bo);
      U = rant_node_open(&ua, "nat-uni", NULL, NULL, &uo);
      CHECK(R!=NULL, "nat: relay node opens (is port %d free?)", (int)R2_PORT);
      CHECK(U!=NULL && B!=NULL, "nat: NAT'd + far nodes open");
      if (U && R && B){
          bt = rant_node_create_topic(B, "nat/t", RANT_SUB_ONLY, NULL,
                   &(RantTopicOpts){ .qos={ .reliability=RANT_RELIABLE } });
          ut = rant_node_create_topic(U, "nat/t", RANT_PUB_ONLY, NULL,
                   &(RantTopicOpts){ .qos={ .reliability=RANT_RELIABLE, .keep_last=8 } });
          CHECK(ut && bt, "nat: topics up");

          /* B cannot dial U (locator black-holed) and U cannot hear multicast: the pair
             can only form through R's introduction followed by U speaking first */
          PUMP3_UNTIL(U, R, B, 10000, rly_sees(B,"nat-uni") && rly_sees(U,"nat-far"));
          CHECK(rly_sees(U,"nat-far"),
                   "nat: the NAT'd node is INTRODUCED to a peer it cannot hear");
          CHECK(rly_sees(B,"nat-uni"),
                   "nat: and reaches it first, so the far node binds its observed source");

          if (ut && bt){
              int r;
              PUMP3_UNTIL(U, R, B, 10000, rant_topic_match_count(ut)!=0);
              CHECK(rant_topic_match_count(ut)==1, "nat: topic matches through the mappings (%d)",
                       rant_topic_match_count(ut));
              r = rant_topic_send(ut, rant_bytes("through-the-nat", 15), NULL);
              PUMP3_UNTIL(U, R, B, 10000, rly_recv!=0);
              CHECK(r==RANT_OK && rly_recv==1 && strcmp(rly_last,"through-the-nat")==0,
                       "nat: reliable data flows despite a dead locator (r=%d recv=%lu '%s')",
                       r, rly_recv, rly_last);
          }

          /* the introducer is bootstrap only: the pair sustains itself point to point */
          rant_node_close(R,1); R=NULL;
          test_pump(U, B, 1500);                        /* past the 800 ms peer timeout */
          CHECK(rly_sees(B,"nat-uni") && rly_sees(U,"nat-far"),
                   "nat: the pair outlives the introducer (B sees U=%d, U sees B=%d)",
                   rly_sees(B,"nat-uni"), rly_sees(U,"nat-far"));
          if (ut && bt){
              int r = rant_topic_send(ut, rant_bytes("after-introducer-died", 21), NULL);
              PUMP_UNTIL(U, B, 10000, rly_recv >= 2);
              CHECK(r==RANT_OK && rly_recv==2 && strcmp(rly_last,"after-introducer-died")==0,
                       "nat: data still flows with the introducer dead (r=%d recv=%lu '%s')",
                       r, rly_recv, rly_last);
          }
      }
      if (R) rant_node_close(R,1);
      if (U) rant_node_close(U,1);
      if (B) rant_node_close(B,1);
      rant_allocator_reset(&ua); rant_allocator_reset(&ra); rant_allocator_reset(&ba);
    }
    if (bh != RANT_SOCK_BAD) i_rant_plat_close(bh);
    i_rant_plat_cleanup();
}

/* The self ip phase: stating our own locator. On one host the advertised port carries
 * the proof, it is deliberately not the port A bound. */
static int sip_addr_of(RantNode *n, const char *name, RantAddr *out){
    uint16_t c = 0, i; size_t nl = strlen(name);
    const i_RantDiscoveryPeerView *p = test_peers(n, &c);
    for (i=0;i<c;i++)
        if (p[i].liveness == RANT_PEER_ACTIVE && p[i].name.len == nl &&
            memcmp(p[i].name.data, name, nl) == 0){ *out = p[i].addr; return 1; }
    return 0;
}
static void selfip_checks(void){
    const uint16_t ADV_PORT = (uint16_t)(45000u + test_domain_base % 15000u);
    /* (a) peers record what we STATE, not where our packets came from */
    { RantAllocator aa = rant_allocator_heap(0);
      RantAllocator ba = rant_allocator_heap(0);
      RantNodeOpts ao, bo; RantNode *A=NULL, *B=NULL; RantAddr got;
      memset(&bo,0,sizeof bo); bo.domain=TEST_DOMAIN+29; bo.disable_shm=1; bo.discovery.max_peers=4;
      bo.discovery.announce_interval_us=200000; bo.net.multicast_interface="127.0.0.1";
      ao=bo; ao.net.self_ip="127.0.0.1"; ao.net.advertise_port=ADV_PORT;
      A = rant_node_open(&aa, "sip-a", NULL, NULL, &ao);
      B = rant_node_open(&ba, "sip-b", NULL, NULL, &bo);
      CHECK(A && B, "selfip: pair up");
      if (A && B){
          memset(&got,0,sizeof got);
          PUMP_UNTIL(A, B, 24000, sip_addr_of(B,"sip-a",&got));
          CHECK(got.ip_len==4 && got.ip[0]==127 && got.ip[3]==1 && got.port==ADV_PORT,
                   "selfip: peers record the stated locator (%u.%u.%u.%u:%u, want 127.0.0.1:%u)",
                   got.ip[0], got.ip[1], got.ip[2], got.ip[3],
                   (unsigned)got.port, (unsigned)ADV_PORT);
      }
      if (A) rant_node_close(A,1);
      if (B) rant_node_close(B,1);
      rant_allocator_reset(&aa); rant_allocator_reset(&ba);
    }
    /* (b) an unparseable locator is a config error, never a silent fallback to the default:
       a node advertising an unreachable address would look healthy and receive nothing */
    { RantAllocator aa = rant_allocator_heap(0);
      RantNodeOpts o; RantNode *n; RantEvent err; char line[160];
      memset(&o,0,sizeof o); o.domain=TEST_DOMAIN+29; o.net.multicast_interface="127.0.0.1";
      o.net.self_ip="not-an-ip";
      n = rant_node_open(&aa, "sip-bad", NULL, NULL, &o);
      CHECK(n==NULL, "selfip: unparseable self ip refuses the open");
      err = rant_last_error(NULL);
      CHECK(err.kind==RANT_ERROR && err.error==RANT_E_BAD_ADDRESS,
               "selfip: ...and says why (%s)", rant_event_str(&err, line, sizeof line));
      if (n) rant_node_close(n,1);
      rant_allocator_reset(&aa);
    }
}

static const TestCase cases[] = {
    { "relay", relay_checks },
    { "nat", nat_checks },
    { "self_ip", selfip_checks },
};

TEST_MAIN(cases)
