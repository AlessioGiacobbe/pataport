// SPDX-License-Identifier: GPL-3.0-or-later
/* Online ad hoc play (Pataport): sceNet, sceNetAdhoc (PDP/PTP), sceNetAdhocctl and
 * sceNetAdhocMatching over the internet, off unless PATAPORT_ADHOC_SERVER=host[:port] is set.
 *
 * A PSP's ad hoc mode is direct Wi-Fi between nearby consoles. Here a lobby server that speaks
 * the PPSSPP ad hoc server protocol (tools/adhoc_server.py) plays the part of the airwaves'
 * discovery: it tells each player which other players share its group and at which address.
 * Game data then goes peer to peer: a PDP socket is a UDP socket and a PTP socket a TCP one,
 * on the PSP port plus PATAPORT_ADHOC_PORT_OFFSET (default 10000, as PPSSPP's default).
 * Matching (the lobby library games use to pick a host and its players) runs over UDP on its
 * own port with a small packet set of its own (HELLO, JOIN, ACCEPT, CANCEL, BIRTH, DEATH, BYE,
 * PING).
 *
 * The libraries' background threads are host work in the scheduler's ad hoc service
 * (sched_set_adhoc_hooks); the guest's adhocctl handlers and matching callbacks run in order,
 * one at a time, each on a short-lived guest thread with the priority and stack size the game
 * gave the library, as the PSP runs them on the library's event thread.
 *
 * Other knobs: PATAPORT_ADHOC_BIND=ip (the local address to use, e.g. to run two instances on
 * one machine), PATAPORT_NICKNAME, PATAPORT_MAC (hle.c). Error codes are the PSPSDK's; the exact
 * results of unusual misuse are not measured on hardware. */

#include "hle_internal.h"
#include "adhoc_sock.h"

/* ---- PSP constants (PSPSDK pspnet_adhoc*.h) ---------------------------------------------- */
#define ADHOC_ERR_INVALID_SOCKET_ID  0x80410701u
#define ADHOC_ERR_INVALID_ADDR       0x80410702u
#define ADHOC_ERR_INVALID_PORT       0x80410703u
#define ADHOC_ERR_INVALID_DATALEN    0x80410705u
#define ADHOC_ERR_NOT_ENOUGH_SPACE   0x80400706u
#define ADHOC_ERR_WOULD_BLOCK        0x80410709u
#define ADHOC_ERR_PORT_IN_USE        0x8041070au
#define ADHOC_ERR_NOT_CONNECTED      0x8041070bu
#define ADHOC_ERR_DISCONNECTED       0x8041070cu
#define ADHOC_ERR_NOT_LISTENED       0x8041070eu
#define ADHOC_ERR_SOCKET_ID_NOT_AVAIL 0x8041070fu
#define ADHOC_ERR_INVALID_ARG        0x80410711u
#define ADHOC_ERR_NOT_INITIALIZED    0x80410712u
#define ADHOC_ERR_ALREADY_INITIALIZED 0x80410713u
#define ADHOC_ERR_TIMEOUT            0x80410715u
#define ADHOC_ERR_CONNECTION_REFUSED 0x80410718u
#define CTL_ERR_INVALID_ARG          0x80410b04u
#define CTL_ERR_ALREADY_INITIALIZED  0x80410b07u
#define CTL_ERR_NOT_INITIALIZED      0x80410b08u
#define CTL_ERR_TOO_MANY_HANDLERS    0x80410b12u
#define MATCH_ERR_INVALID_MODE       0x80410801u
#define MATCH_ERR_INVALID_PORT       0x80410802u
#define MATCH_ERR_INVALID_MAXNUM     0x80410803u
#define MATCH_ERR_INVALID_OPTLEN     0x80410805u
#define MATCH_ERR_INVALID_ARG        0x80410806u
#define MATCH_ERR_INVALID_ID         0x80410807u
#define MATCH_ERR_ID_NOT_AVAIL       0x80410808u
#define MATCH_ERR_IS_RUNNING         0x8041080au
#define MATCH_ERR_NOT_RUNNING        0x8041080bu
#define MATCH_ERR_UNKNOWN_TARGET     0x8041080cu
#define MATCH_ERR_TARGET_NOT_READY   0x8041080du
#define MATCH_ERR_EXCEED_MAXNUM      0x8041080eu
#define MATCH_ERR_REQUEST_IN_PROGRESS 0x8041080fu
#define MATCH_ERR_ALREADY_ESTABLISHED 0x80410810u
#define MATCH_ERR_ALREADY_INITIALIZED 0x80410812u
#define MATCH_ERR_NOT_INITIALIZED    0x80410813u
#define MATCH_ERR_PORT_IN_USE        0x80410814u

enum { CTL_EV_ERROR, CTL_EV_CONNECT, CTL_EV_DISCONNECT, CTL_EV_SCAN };
enum { CTL_DISCONNECTED, CTL_CONNECTED, CTL_SCANNING };
enum { M_PARENT = 1, M_CHILD = 2, M_P2P = 3 };
enum { EV_HELLO = 1, EV_REQUEST, EV_LEAVE, EV_DENY, EV_CANCEL, EV_ACCEPT, EV_ESTABLISHED,
       EV_TIMEOUT, EV_ERROR, EV_BYE };

#define OPT_MAX 1024u

/* ---- configuration ---------------------------------------------------------------------- */
static int s_cfg_done, s_on;
static uint32_t s_srv_ip, s_bind_ip;
static uint16_t s_srv_port = 27312, s_off = 10000;
static char s_nick[128] = "Pataport";

int hle_adhoc_enabled(void) {
    if (s_cfg_done) return s_on;
    s_cfg_done = 1;
    const char *srv = getenv("PATAPORT_ADHOC_SERVER");
    if (!srv || !*srv) return 0;
    char host[256];
    snprintf(host, sizeof host, "%s", srv);
    char *colon = strrchr(host, ':');
    if (colon) { *colon = '\0'; s_srv_port = (uint16_t)atoi(colon + 1); }
    s_srv_ip = as_resolve(host);
    if (!s_srv_ip || !s_srv_port) {
        fprintf(stderr, "ADHOC: the lobby server '%s' does not resolve; online play stays off\n", srv);
        return 0;
    }
    const char *bind_ip = getenv("PATAPORT_ADHOC_BIND");
    if (bind_ip && *bind_ip) s_bind_ip = as_resolve(bind_ip);
    const char *off = getenv("PATAPORT_ADHOC_PORT_OFFSET");
    if (off && *off) s_off = (uint16_t)atoi(off);
    const char *nick = getenv("PATAPORT_NICKNAME");
    if (nick && *nick) snprintf(s_nick, sizeof s_nick, "%s", nick);
    s_on = 1;
    fprintf(stderr, "ADHOC: online play through the lobby at %s:%u (data ports +%u)\n", host,
            s_srv_port, s_off);
    return 1;
}

static int self_mac(uint8_t m[6]) {
    if (!console_profile_identity()) return 0;
    memcpy(m, s_console.mac, 6);
    return 1;
}

static uint64_t now_us(void) { return sched_vtime_us(); }

static const char *mac_str(const uint8_t *m) {
    static char buf[4][18];
    static int n;
    char *b = buf[n++ & 3];
    snprintf(b, 18, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    return b;
}

static int guest_read(uint32_t addr, void *out, uint32_t len) {
    if (!len) return 1;
    if (!addr || !sr_guest_span_readable(addr, len)) return 0;
    for (uint32_t i = 0; i < len; i++) ((uint8_t *)out)[i] = MEM_R8(addr + i);
    return 1;
}

static int guest_write(uint32_t addr, const void *in, uint32_t len) {
    if (!len) return 1;
    if (!addr || !sr_guest_span_writable(addr, len)) return 0;
    for (uint32_t i = 0; i < len; i++) MEM_W8(addr + i, ((const uint8_t *)in)[i]);
    return 1;
}

/* A blocking call's wait: 1 to wait a millisecond and retry, 0 once `timeout` (0 = none) passed. */
static int wait_more(uint64_t start, uint32_t timeout) {
    if (timeout && now_us() - start >= timeout) return 0;
    sched_delay_current(1000);
    return 1;
}

/* ---- guest calls: handlers and callbacks, one at a time on their own thread -------------- */
typedef struct {
    uint32_t fn, gp, prio, stack, blob_args, len;
    uint32_t a[5];
    const char *name;
    uint8_t blob[8 + OPT_MAX];
} GuestCall;

#define CALL_QUEUE 64
static GuestCall s_calls[CALL_QUEUE];
static int s_call_head, s_call_count;
static uint32_t s_call_uid;

static void call_push(uint32_t fn, uint32_t gp, uint32_t prio, uint32_t stack, const char *name,
                      const uint32_t a[5], uint32_t blob_args, const void *blob, uint32_t len) {
    if (!fn) return;
    if (s_call_count == CALL_QUEUE) {
        fprintf(stderr, "ADHOC: event queue full; dropped a %s call\n", name);
        return;
    }
    GuestCall *c = &s_calls[(s_call_head + s_call_count++) % CALL_QUEUE];
    c->fn = fn;
    c->gp = gp;
    c->prio = prio >= 8u && prio <= 119u ? prio : 48u;
    c->stack = stack >= 0x2000u && stack <= 0x100000u ? stack : 0x4000u;
    c->name = name;
    memcpy(c->a, a, sizeof c->a);
    c->blob_args = blob_args;
    c->len = len > sizeof c->blob ? (uint32_t)sizeof c->blob : len;
    if (c->len) memcpy(c->blob, blob, c->len);
}

static void calls_pump(void) {
    if (s_call_uid) {
        if (sched_delete_thread(s_call_uid) != 0) return;   /* the last call is still running */
        s_call_uid = 0;
    }
    if (!s_call_count) return;
    GuestCall *c = &s_calls[s_call_head];
    s_call_head = (s_call_head + 1) % CALL_QUEUE;
    s_call_count--;
    uint32_t uid = sched_create_thread_ex(c->fn, (int)c->prio, c->stack, 0x80000000u, c->name);
    if (!uid || (uid & 0x80000000u)) {
        fprintf(stderr, "ADHOC: could not create the %s thread (0x%08x)\n", c->name, uid);
        return;
    }
    sched_mark_code_pointer_entry(uid);
    sched_thread_set_gp(uid, c->gp);
    if (sched_start_thread(uid, 0, 0) != 0 ||
        sched_thread_prime_call(uid, c->a, c->blob_args, c->blob, c->len) != 0) {
        fprintf(stderr, "ADHOC: could not start the %s thread\n", c->name);
        return;
    }
    s_call_uid = uid;
}

/* ---- the lobby: adhocctl ------------------------------------------------------------------ */
typedef struct { uint8_t mac[6]; uint32_t ip; char nick[129]; } Peer;
#define MAX_PEERS 16
static Peer s_peers[MAX_PEERS];
static int s_npeers;

static const Peer *peer_by_mac(const uint8_t *mac) {
    for (int i = 0; i < s_npeers; i++) if (!memcmp(s_peers[i].mac, mac, 6)) return &s_peers[i];
    return NULL;
}
static const Peer *peer_by_ip(uint32_t ip) {
    for (int i = 0; i < s_npeers; i++) if (s_peers[i].ip == ip) return &s_peers[i];
    return NULL;
}

enum { SRV_OFF, SRV_CONNECTING, SRV_READY, SRV_FAILED };
static struct {
    int inited, state;
    uint32_t prio, stack;
    char product[9];
    struct { uint32_t fn, arg, gp; } handler[4];
    uint8_t group[8], bssid[6];
    int want_group;
    int srv, srv_state;
    uint64_t srv_deadline, next_ping;
    uint8_t rx[2048];
    int rxn;
} C = { .srv = -1 };

static int s_netconf_pending;
static uint64_t s_netconf_deadline;

static void ctl_event(uint32_t flag, uint32_t error) {
    for (int i = 0; i < 4; i++) {
        if (!C.handler[i].fn) continue;
        const uint32_t a[5] = { flag, error, C.handler[i].arg, 0, 0 };
        call_push(C.handler[i].fn, C.handler[i].gp, C.prio, C.stack, "SceNetAdhocctl", a, 0, NULL, 0);
    }
}

static void srv_send(const void *p, int len) {
    if (C.srv_state != SRV_READY) return;
    if (as_send(C.srv, p, len) != len)
        fprintf(stderr, "ADHOC: a %d-byte packet to the lobby did not go out whole\n", len);
}

static void srv_send_connect(void) {
    uint8_t p[9] = { 2 };
    memcpy(p + 1, C.group, 8);
    srv_send(p, sizeof p);
}

static void srv_lost(const char *why) {
    if (C.srv >= 0) as_close(C.srv);
    C.srv = -1;
    C.srv_state = SRV_FAILED;
    s_npeers = 0;
    fprintf(stderr, "ADHOC: %s\n", why);
    if (C.state == CTL_CONNECTED) {
        C.state = CTL_DISCONNECTED;
        ctl_event(CTL_EV_DISCONNECT, 0);
    }
}

static int srv_packet_len(uint8_t op) {
    switch (op) {
    case 0: case 5: return 1;
    case 2: return 139;
    case 3: return 5;
    case 4: return 15;
    case 6: return 7;
    case 7: return 193;
    default: return -1;
    }
}

static void srv_packet(const uint8_t *p) {
    switch (p[0]) {
    case 2: {   /* CONNECT: a player is (now) in our group */
        Peer np;
        memcpy(np.nick, p + 1, 128);
        np.nick[128] = '\0';
        memcpy(np.mac, p + 129, 6);
        memcpy(&np.ip, p + 135, 4);
        Peer *slot = (Peer *)peer_by_mac(np.mac);
        if (!slot && s_npeers < MAX_PEERS) slot = &s_peers[s_npeers++];
        if (slot) *slot = np;
        fprintf(stderr, "ADHOC: %s (%s) is in the group\n", np.nick, mac_str(np.mac));
        break;
    }
    case 3: {   /* DISCONNECT: the player with this address left */
        uint32_t ip;
        memcpy(&ip, p + 1, 4);
        for (int i = 0; i < s_npeers; i++) {
            if (s_peers[i].ip != ip) continue;
            fprintf(stderr, "ADHOC: %s left the group\n", s_peers[i].nick);
            s_peers[i--] = s_peers[--s_npeers];
        }
        break;
    }
    case 6:     /* CONNECT_BSSID: we are in the group */
        memcpy(C.bssid, p + 1, 6);
        C.want_group = 0;
        if (C.state != CTL_CONNECTED) {
            C.state = CTL_CONNECTED;
            fprintf(stderr, "ADHOC: joined group '%.8s' (%d other players)\n", (const char *)C.group,
                    s_npeers);
            ctl_event(CTL_EV_CONNECT, 0);
        }
        break;
    case 5:     /* SCAN_COMPLETE */
        if (C.state == CTL_SCANNING) {
            C.state = CTL_DISCONNECTED;
            ctl_event(CTL_EV_SCAN, 0);
        }
        break;
    default:    /* PING, SCAN results and chat carry nothing this runtime uses */
        break;
    }
}

static void srv_poll(void) {
    uint64_t now = now_us();
    if (C.srv_state == SRV_CONNECTING) {
        int r = as_tcp_connected(C.srv);
        if (r == AS_AGAIN && now < C.srv_deadline) return;
        if (r != 1) { srv_lost("the lobby server does not answer"); return; }
        uint8_t login[144] = { 1 };
        self_mac(login + 1);
        memcpy(login + 7, s_nick, strlen(s_nick) < 128 ? strlen(s_nick) : 127);
        memcpy(login + 135, C.product, 9);
        C.srv_state = SRV_READY;
        srv_send(login, sizeof login);
        C.next_ping = now + 1000000u;
        fprintf(stderr, "ADHOC: logged in to the lobby as %s (%.9s)\n", s_nick, C.product);
        if (C.want_group) srv_send_connect();
    }
    if (C.srv_state != SRV_READY) return;
    for (;;) {
        int n = as_recv(C.srv, C.rx + C.rxn, (int)sizeof C.rx - C.rxn);
        if (n == AS_AGAIN) break;
        if (n < 0) { srv_lost("the lobby server closed the connection"); return; }
        C.rxn += n;
        int at = 0;
        while (at < C.rxn) {
            int len = srv_packet_len(C.rx[at]);
            if (len < 0) { srv_lost("the lobby server sent an unknown packet"); return; }
            if (at + len > C.rxn) break;
            srv_packet(C.rx + at);
            at += len;
        }
        memmove(C.rx, C.rx + at, (size_t)(C.rxn - at));
        C.rxn -= at;
    }
    if (now >= C.next_ping) {
        uint8_t ping = 0;
        srv_send(&ping, 1);
        C.next_ping = now + 1000000u;
    }
}

static void ctl_connect(const uint8_t group[8]) {
    memcpy(C.group, group, 8);
    C.want_group = 1;
    if (C.srv_state == SRV_READY) srv_send_connect();
}

static void ctl_disconnect(void) {
    uint8_t p = 3;
    if (C.state == CTL_CONNECTED) srv_send(&p, 1);
    C.want_group = 0;
    s_npeers = 0;
    if (C.state == CTL_CONNECTED) {
        C.state = CTL_DISCONNECTED;
        ctl_event(CTL_EV_DISCONNECT, 0);
    }
}

/* sceNetAdhocctlInit(int stacksize, int priority, SceNetAdhocctlAdhocId *id) */
static uint32_t h_CtlInit(CpuState *s) {
    if (C.inited) return CTL_ERR_ALREADY_INITIALIZED;
    uint8_t id[16];
    if (!guest_read(A2, id, sizeof id)) return CTL_ERR_INVALID_ARG;
    memcpy(C.product, id + 4, 9);
    C.stack = A0;
    C.prio = A1;
    C.state = CTL_DISCONNECTED;
    C.rxn = 0;
    C.srv = as_tcp_open(s_bind_ip, 0);
    int r = C.srv >= 0 ? as_tcp_connect(C.srv, s_srv_ip, s_srv_port) : AS_FAIL;
    C.srv_state = r == AS_FAIL ? SRV_FAILED : SRV_CONNECTING;
    C.srv_deadline = now_us() + 5000000u;
    if (r == AS_FAIL) fprintf(stderr, "ADHOC: cannot open a connection to the lobby server\n");
    C.inited = 1;
    return 0;
}

static uint32_t h_CtlTerm(CpuState *s) {
    (void)s;
    if (!C.inited) return 0;
    ctl_disconnect();
    if (C.srv >= 0) as_close(C.srv);
    memset(&C, 0, sizeof C);
    C.srv = -1;
    return 0;
}

static uint32_t h_CtlAddHandler(CpuState *s) {
    for (uint32_t i = 0; i < 4; i++) {
        if (C.handler[i].fn) continue;
        C.handler[i].fn = A0;
        C.handler[i].arg = A1;
        C.handler[i].gp = s->r[28];
        return i;
    }
    return CTL_ERR_TOO_MANY_HANDLERS;
}

static uint32_t h_CtlDelHandler(CpuState *s) {
    if (A0 < 4) memset(&C.handler[A0], 0, sizeof C.handler[A0]);
    return 0;
}

/* sceNetAdhocctlCreate / Connect (const SceNetAdhocctlGroupName *name) */
static uint32_t h_CtlConnect(CpuState *s) {
    if (!C.inited) return CTL_ERR_NOT_INITIALIZED;
    uint8_t g[8];
    if (!guest_read(A0, g, 8)) return CTL_ERR_INVALID_ARG;
    ctl_connect(g);
    return 0;
}

static uint32_t h_CtlDisconnect(CpuState *s) {
    (void)s;
    if (!C.inited) return CTL_ERR_NOT_INITIALIZED;
    ctl_disconnect();
    return 0;
}

static uint32_t h_CtlGetState(CpuState *s) {
    if (!C.inited) return CTL_ERR_NOT_INITIALIZED;
    if (!A0 || !sr_guest_span_writable(A0, 4)) return CTL_ERR_INVALID_ARG;
    MEM_W32(A0, (uint32_t)C.state);
    return 0;
}

/* The netconf dialog's ad hoc actions (pspUtilityNetconfData: action at +0x30, adhoc
 * parameters {name[8], timeout} at the pointer at +0x34): 2 connect, 4 create, 5 join. */
int hle_adhoc_netconf_start(uint32_t param) {
    if (!hle_adhoc_enabled() || !C.inited || !sr_guest_span_readable(param, 0x38)) return 0;
    uint32_t action = MEM_R32(param + 0x30), adhoc = MEM_R32(param + 0x34);
    uint8_t g[8];
    if ((action != 2 && action != 4 && action != 5) || !guest_read(adhoc, g, 8)) return 0;
    ctl_connect(g);
    s_netconf_pending = 1;
    s_netconf_deadline = now_us() + 20000000u;
    fprintf(stderr, "ADHOC: connecting to group '%.8s'\n", (const char *)g);
    return 1;
}

int hle_adhoc_netconf_poll(uint32_t *result) {
    if (!s_netconf_pending || C.state == CTL_CONNECTED) {
        *result = 0;
        s_netconf_pending = 0;
        return 1;
    }
    if (C.srv_state == SRV_FAILED || now_us() >= s_netconf_deadline) {
        fprintf(stderr, "ADHOC: could not join the group; the dialog reports it as cancelled\n");
        C.want_group = 0;
        s_netconf_pending = 0;
        *result = 1;
        return 1;
    }
    return 0;
}

/* ---- sceNet ------------------------------------------------------------------------------ */
static uint32_t s_net_pool;
static int s_net_inited, s_adhoc_inited;

static uint32_t h_NetInit(CpuState *s) { s_net_pool = A0; s_net_inited = 1; return 0; }
static uint32_t h_NetTerm(CpuState *s) { (void)s; s_net_inited = 0; return 0; }

/* sceNetGetMallocStat(SceNetMallocStat *stat): {pool, maximum, free} */
static uint32_t h_NetGetMallocStat(CpuState *s) {
    if (!A0 || !sr_guest_span_writable(A0, 12)) return ADHOC_ERR_INVALID_ARG;
    MEM_W32(A0, s_net_pool);
    MEM_W32(A0 + 4, s_net_pool / 4u);
    MEM_W32(A0 + 8, s_net_pool - s_net_pool / 4u);
    return 0;
}

/* sceNetEtherStrton(const char *str, u8 *mac): "xx:xx:xx:xx:xx:xx" to six bytes. */
static uint32_t h_NetEtherStrton(CpuState *s) {
    char text[18] = {0};
    if (!guest_read(A0, text, 17)) return ADHOC_ERR_INVALID_ARG;
    unsigned b[6];
    if (sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
        return ADHOC_ERR_INVALID_ARG;
    uint8_t m[6];
    for (int i = 0; i < 6; i++) m[i] = (uint8_t)b[i];
    return guest_write(A1, m, 6) ? 0u : ADHOC_ERR_INVALID_ARG;
}

/* ---- sceNetAdhoc: PDP (UDP) and PTP (TCP) sockets ----------------------------------------- */
static const uint8_t k_broadcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

typedef struct { int used, h; uint16_t port; } Pdp;
enum { PTP_OPEN = 1, PTP_CONNECTING, PTP_ESTABLISHED, PTP_LISTEN };
typedef struct { int used, h, state; uint8_t pmac[6]; uint16_t lport, pport; } Ptp;
#define MAX_SOCK 32
static Pdp s_pdp[MAX_SOCK];
static Ptp s_ptp[MAX_SOCK];

static Pdp *pdp_get(uint32_t id) { return id >= 1 && id <= MAX_SOCK && s_pdp[id - 1].used ? &s_pdp[id - 1] : NULL; }
static Ptp *ptp_get(uint32_t id) { return id >= 1 && id <= MAX_SOCK && s_ptp[id - 1].used ? &s_ptp[id - 1] : NULL; }

static int own_mac_arg(uint32_t addr) {
    uint8_t m[6], self[6];
    return guest_read(addr, m, 6) && self_mac(self) && !memcmp(m, self, 6);
}

static uint32_t h_AdhocInit(CpuState *s) {
    (void)s;
    if (s_adhoc_inited) return ADHOC_ERR_ALREADY_INITIALIZED;
    s_adhoc_inited = 1;
    return 0;
}

static uint32_t h_AdhocTerm(CpuState *s) {
    (void)s;
    for (int i = 0; i < MAX_SOCK; i++) {
        if (s_pdp[i].used) as_close(s_pdp[i].h);
        if (s_ptp[i].used) as_close(s_ptp[i].h);
    }
    memset(s_pdp, 0, sizeof s_pdp);
    memset(s_ptp, 0, sizeof s_ptp);
    s_adhoc_inited = 0;
    return 0;
}

/* sceNetAdhocPdpCreate(u8 *mac, u16 port, unsigned bufsize, int flag) */
static uint32_t h_PdpCreate(CpuState *s) {
    if (!s_adhoc_inited) return ADHOC_ERR_NOT_INITIALIZED;
    if (!own_mac_arg(A0)) return ADHOC_ERR_INVALID_ADDR;
    for (uint32_t i = 0; i < MAX_SOCK; i++) {
        if (s_pdp[i].used) continue;
        uint16_t port = (uint16_t)A1;
        int h = as_udp_open(s_bind_ip, port ? (uint16_t)(port + s_off) : 0);
        if (h == AS_INUSE) return ADHOC_ERR_PORT_IN_USE;
        if (h < 0) return ADHOC_ERR_INVALID_ARG;
        s_pdp[i] = (Pdp){ 1, h, port };
        return i + 1;
    }
    return ADHOC_ERR_SOCKET_ID_NOT_AVAIL;
}

static uint32_t h_PdpDelete(CpuState *s) {
    Pdp *p = pdp_get(A0);
    if (!p) return ADHOC_ERR_INVALID_SOCKET_ID;
    as_close(p->h);
    p->used = 0;
    return 0;
}

/* sceNetAdhocPdpSend(int id, u8 *dest, u16 port, void *data, unsigned len, unsigned timeout,
 * int flag). A broadcast address goes to every player in the group. */
static uint32_t h_PdpSend(CpuState *s) {
    Pdp *p = pdp_get(A0);
    uint32_t len = stack_arg(s, 0);
    uint8_t dest[6];
    static uint8_t buf[65536];
    if (!p) return ADHOC_ERR_INVALID_SOCKET_ID;
    if (!guest_read(A1, dest, 6)) return ADHOC_ERR_INVALID_ADDR;
    if (!A2) return ADHOC_ERR_INVALID_PORT;
    if (len > 65507u || !guest_read(A3, buf, len)) return ADHOC_ERR_INVALID_DATALEN;
    uint16_t port = (uint16_t)(A2 + s_off);
    for (int i = 0; i < s_npeers; i++)
        if (!memcmp(dest, k_broadcast, 6) || !memcmp(dest, s_peers[i].mac, 6))
            (void)as_sendto(p->h, buf, (int)len, s_peers[i].ip, port);
    return 0;
}

/* sceNetAdhocPdpRecv(int id, u8 *src, u16 *port, void *buf, int *len, unsigned timeout, int flag)
 * Datagrams from addresses outside the group are dropped. A buffer too small for the next
 * datagram leaves it queued and reports the size it needs. */
static uint32_t h_PdpRecv(CpuState *s) {
    const uint32_t id = A0, src = A1, portp = A2, buf = A3, lenp = stack_arg(s, 0);
    const uint32_t timeout = stack_arg(s, 1), nonblock = stack_arg(s, 2);
    const uint64_t start = now_us();
    static uint8_t data[65536];
    if (!lenp || !sr_guest_span_writable(lenp, 4)) return ADHOC_ERR_INVALID_ARG;
    for (;;) {
        Pdp *p = pdp_get(id);
        if (!p) return ADHOC_ERR_INVALID_SOCKET_ID;
        uint32_t ip;
        uint16_t port;
        int n = as_recvfrom(p->h, data, (int)sizeof data, &ip, &port, 1);
        if (n >= 0) {
            const Peer *peer = peer_by_ip(ip);
            if (!peer) { (void)as_recvfrom(p->h, NULL, 0, NULL, NULL, 0); continue; }
            uint32_t room = MEM_R32(lenp);
            if ((uint32_t)n > room) { MEM_W32(lenp, (uint32_t)n); return ADHOC_ERR_NOT_ENOUGH_SPACE; }
            (void)as_recvfrom(p->h, NULL, 0, NULL, NULL, 0);
            if (!guest_write(buf, data, (uint32_t)n)) return ADHOC_ERR_INVALID_ARG;
            (void)guest_write(src, peer->mac, 6);
            if (portp && sr_guest_span_writable(portp, 2)) MEM_W16(portp, (uint16_t)(port - s_off));
            MEM_W32(lenp, (uint32_t)n);
            return 0;
        }
        if (nonblock) return ADHOC_ERR_WOULD_BLOCK;
        if (!wait_more(start, timeout)) return ADHOC_ERR_TIMEOUT;
    }
}

static int ptp_new(void) {
    for (int i = 0; i < MAX_SOCK; i++) if (!s_ptp[i].used) return i;
    return -1;
}

/* sceNetAdhocPtpOpen(u8 *src, u16 sport, u8 *dst, u16 dport, unsigned bufsize,
 * unsigned rexmt_int, int rexmt_cnt, int flag) */
static uint32_t h_PtpOpen(CpuState *s) {
    if (!s_adhoc_inited) return ADHOC_ERR_NOT_INITIALIZED;
    if (!own_mac_arg(A0)) return ADHOC_ERR_INVALID_ADDR;
    Ptp t = { 1, -1, PTP_OPEN, {0}, (uint16_t)A1, (uint16_t)A3 };
    if (!guest_read(A2, t.pmac, 6) || !memcmp(t.pmac, k_broadcast, 6)) return ADHOC_ERR_INVALID_ADDR;
    if (!t.pport) return ADHOC_ERR_INVALID_PORT;
    int i = ptp_new();
    if (i < 0) return ADHOC_ERR_SOCKET_ID_NOT_AVAIL;
    s_ptp[i] = t;
    return (uint32_t)i + 1;
}

/* (Re)start a connection attempt: bound to the local port, so the peer sees which it is. */
static int ptp_dial(Ptp *t) {
    const Peer *peer = peer_by_mac(t->pmac);
    if (!peer) return AS_AGAIN;               /* the peer is not in the group (yet) */
    if (t->h >= 0) as_close(t->h);
    t->h = as_tcp_open(s_bind_ip, t->lport ? (uint16_t)(t->lport + s_off) : 0);
    if (t->h < 0) return t->h == AS_INUSE ? AS_INUSE : AS_FAIL;
    int r = as_tcp_connect(t->h, peer->ip, (uint16_t)(t->pport + s_off));
    t->state = PTP_CONNECTING;
    return r == AS_FAIL ? AS_FAIL : 0;
}

/* sceNetAdhocPtpConnect(int id, unsigned timeout, int flag): refused attempts are retried until
 * the timeout, as the PSP retransmits its connection request. */
static uint32_t h_PtpConnect(CpuState *s) {
    const uint32_t id = A0, timeout = A1, nonblock = A2;
    const uint64_t start = now_us();
    for (;;) {
        Ptp *t = ptp_get(id);
        if (!t) return ADHOC_ERR_INVALID_SOCKET_ID;
        if (t->state == PTP_ESTABLISHED) return 0;
        if (t->state == PTP_LISTEN) return ADHOC_ERR_CONNECTION_REFUSED;
        int r = t->state == PTP_OPEN ? ptp_dial(t) : as_tcp_connected(t->h);
        if (r == AS_INUSE) return ADHOC_ERR_PORT_IN_USE;
        if (r == 1) { t->state = PTP_ESTABLISHED; return 0; }
        if (r == AS_FAIL) t->state = PTP_OPEN;   /* refused: dial again on the next round */
        if (nonblock) return ADHOC_ERR_WOULD_BLOCK;
        if (!wait_more(start, timeout)) return ADHOC_ERR_TIMEOUT;
    }
}

/* sceNetAdhocPtpListen(u8 *src, u16 sport, unsigned bufsize, unsigned rexmt_int, int rexmt_cnt,
 * int backlog, int flag) */
static uint32_t h_PtpListen(CpuState *s) {
    if (!s_adhoc_inited) return ADHOC_ERR_NOT_INITIALIZED;
    if (!own_mac_arg(A0)) return ADHOC_ERR_INVALID_ADDR;
    if (!A1) return ADHOC_ERR_INVALID_PORT;
    int i = ptp_new();
    if (i < 0) return ADHOC_ERR_SOCKET_ID_NOT_AVAIL;
    int h = as_tcp_open(s_bind_ip, (uint16_t)(A1 + s_off));
    if (h == AS_INUSE) return ADHOC_ERR_PORT_IN_USE;
    if (h < 0 || as_tcp_listen(h, (int)stack_arg(s, 1)) != 0) {
        if (h >= 0) as_close(h);
        return ADHOC_ERR_INVALID_ARG;
    }
    s_ptp[i] = (Ptp){ 1, h, PTP_LISTEN, {0}, (uint16_t)A1, 0 };
    return (uint32_t)i + 1;
}

/* sceNetAdhocPtpAccept(int id, u8 *mac, u16 *port, unsigned timeout, int flag) */
static uint32_t h_PtpAccept(CpuState *s) {
    const uint32_t id = A0, macp = A1, portp = A2, timeout = A3, nonblock = stack_arg(s, 0);
    const uint64_t start = now_us();
    for (;;) {
        Ptp *l = ptp_get(id);
        if (!l) return ADHOC_ERR_INVALID_SOCKET_ID;
        if (l->state != PTP_LISTEN) return ADHOC_ERR_NOT_LISTENED;
        uint32_t ip;
        uint16_t port;
        int h = as_tcp_accept(l->h, &ip, &port);
        if (h >= 0) {
            const Peer *peer = peer_by_ip(ip);
            int i = peer ? ptp_new() : -1;
            if (i < 0) { as_close(h); continue; }   /* not a player of the group */
            s_ptp[i] = (Ptp){ 1, h, PTP_ESTABLISHED, {0}, l->lport, (uint16_t)(port - s_off) };
            memcpy(s_ptp[i].pmac, peer->mac, 6);
            (void)guest_write(macp, peer->mac, 6);
            if (portp && sr_guest_span_writable(portp, 2)) MEM_W16(portp, s_ptp[i].pport);
            return (uint32_t)i + 1;
        }
        if (nonblock) return ADHOC_ERR_WOULD_BLOCK;
        if (!wait_more(start, timeout)) return ADHOC_ERR_TIMEOUT;
    }
}

/* sceNetAdhocPtpSend(int id, const void *data, int *len, unsigned timeout, int flag) */
static uint32_t h_PtpSend(CpuState *s) {
    const uint32_t id = A0, data = A1, lenp = A2, timeout = A3, nonblock = stack_arg(s, 0);
    const uint64_t start = now_us();
    static uint8_t buf[65536];
    if (!lenp || !sr_guest_span_writable(lenp, 4)) return ADHOC_ERR_INVALID_ARG;
    const uint32_t len = MEM_R32(lenp);
    if (len > sizeof buf) return ADHOC_ERR_INVALID_DATALEN;
    for (;;) {
        Ptp *t = ptp_get(id);
        if (!t) return ADHOC_ERR_INVALID_SOCKET_ID;
        if (t->state != PTP_ESTABLISHED) return ADHOC_ERR_NOT_CONNECTED;
        /* Read every round: another thread's send may have used the buffer while this one waited. */
        if (!guest_read(data, buf, len)) return ADHOC_ERR_INVALID_DATALEN;
        int n = len ? as_send(t->h, buf, (int)len) : 0;
        if (n >= 0) { MEM_W32(lenp, (uint32_t)n); return 0; }
        if (n == AS_CLOSED) return ADHOC_ERR_DISCONNECTED;
        if (nonblock) return ADHOC_ERR_WOULD_BLOCK;
        if (!wait_more(start, timeout)) return ADHOC_ERR_TIMEOUT;
    }
}

/* sceNetAdhocPtpRecv(int id, void *buf, int *len, unsigned timeout, int flag) */
static uint32_t h_PtpRecv(CpuState *s) {
    const uint32_t id = A0, buf = A1, lenp = A2, timeout = A3, nonblock = stack_arg(s, 0);
    const uint64_t start = now_us();
    static uint8_t data[65536];
    if (!lenp || !sr_guest_span_writable(lenp, 4)) return ADHOC_ERR_INVALID_ARG;
    for (;;) {
        Ptp *t = ptp_get(id);
        if (!t) return ADHOC_ERR_INVALID_SOCKET_ID;
        if (t->state != PTP_ESTABLISHED) return ADHOC_ERR_NOT_CONNECTED;
        uint32_t room = MEM_R32(lenp);
        int n = as_recv(t->h, data, (int)(room < sizeof data ? room : sizeof data));
        if (n > 0) {
            if (!guest_write(buf, data, (uint32_t)n)) return ADHOC_ERR_INVALID_ARG;
            MEM_W32(lenp, (uint32_t)n);
            return 0;
        }
        if (n == AS_CLOSED) return ADHOC_ERR_DISCONNECTED;
        if (nonblock) return ADHOC_ERR_WOULD_BLOCK;
        if (!wait_more(start, timeout)) return ADHOC_ERR_TIMEOUT;
    }
}

/* sceNetAdhocPtpFlush(int id, unsigned timeout, int flag): TCP_NODELAY sockets send at once. */
static uint32_t h_PtpFlush(CpuState *s) {
    Ptp *t = ptp_get(A0);
    if (!t) return ADHOC_ERR_INVALID_SOCKET_ID;
    return t->state == PTP_ESTABLISHED ? 0u : ADHOC_ERR_NOT_CONNECTED;
}

static uint32_t h_PtpClose(CpuState *s) {
    Ptp *t = ptp_get(A0);
    if (!t) return ADHOC_ERR_INVALID_SOCKET_ID;
    if (t->h >= 0) as_close(t->h);
    t->used = 0;
    return 0;
}

/* ---- sceNetAdhocMatching -------------------------------------------------------------- */
enum { MP_OFFER = 1, MP_OUT_REQ, MP_IN_REQ, MP_PARENT, MP_CHILD, MP_P2P, MP_SIBLING };
enum { PK_PING = 0, PK_HELLO, PK_JOIN, PK_ACCEPT, PK_CANCEL, PK_BIRTH = 7, PK_DEATH, PK_BYE };

typedef struct {
    int used, state, tries;
    uint8_t mac[6];
    uint64_t last, next_rexmt;
} MPeer;

typedef struct {
    int used, running, h;
    uint32_t mode, maxnum, port, hello_int, keepalive_int, init_count, rexmt_int, cb, gp;
    uint32_t evth_prio, evth_stack, hello_len, join_len;
    uint8_t hello[OPT_MAX], join[OPT_MAX];
    uint64_t next_hello, next_ping;
    MPeer peer[16];
} Matching;

#define MAX_MATCHING 8
static Matching s_m[MAX_MATCHING];
static int s_match_inited;

static Matching *m_get(uint32_t id) { return id >= 1 && id <= MAX_MATCHING && s_m[id - 1].used ? &s_m[id - 1] : NULL; }

static MPeer *m_peer(Matching *m, const uint8_t *mac, int create) {
    MPeer *free_slot = NULL;
    for (int i = 0; i < 16; i++) {
        if (m->peer[i].used && !memcmp(m->peer[i].mac, mac, 6)) return &m->peer[i];
        if (!m->peer[i].used && !free_slot) free_slot = &m->peer[i];
    }
    if (!create || !free_slot) return NULL;
    memset(free_slot, 0, sizeof *free_slot);
    free_slot->used = 1;
    memcpy(free_slot->mac, mac, 6);
    free_slot->last = now_us();
    return free_slot;
}

static int m_established(const MPeer *p) {
    return p->used && (p->state == MP_PARENT || p->state == MP_CHILD || p->state == MP_P2P);
}

static int m_count(const Matching *m, int state) {
    int n = 0;
    for (int i = 0; i < 16; i++) n += m->peer[i].used && m->peer[i].state == state;
    return n;
}

static void m_event(Matching *m, uint32_t ev, const uint8_t *mac, const uint8_t *opt, uint32_t optlen) {
    uint8_t blob[8 + OPT_MAX] = {0};
    if (optlen > OPT_MAX) optlen = OPT_MAX;
    memcpy(blob, mac, 6);
    if (optlen) memcpy(blob + 8, opt, optlen);
    const uint32_t a[5] = { (uint32_t)(m - s_m) + 1u, ev, 0u, optlen, optlen ? 8u : 0u };
    call_push(m->cb, m->gp, m->evth_prio, m->evth_stack, "SceNetAdhocMatchingEvent", a,
              (1u << 2) | (optlen ? 1u << 4 : 0u), blob, 8 + optlen);
    static const char *names[] = { "?", "HELLO", "REQUEST", "LEAVE", "DENY", "CANCEL", "ACCEPT",
                                   "ESTABLISHED", "TIMEOUT", "ERROR", "BYE" };
    if (ev != EV_HELLO)
        fprintf(stderr, "ADHOC: matching %u: %s %s\n", a[0], ev <= EV_BYE ? names[ev] : "?",
                mac_str(mac));
}

static void m_send(Matching *m, const uint8_t *mac, const uint8_t *p, uint32_t len) {
    const Peer *peer = peer_by_mac(mac);
    if (peer && m->h >= 0) (void)as_sendto(m->h, p, (int)len, peer->ip, (uint16_t)(m->port + s_off));
}

/* [op][optlen u32][opt] */
static void m_send_opt(Matching *m, const uint8_t *mac, uint8_t op, const uint8_t *opt, uint32_t len) {
    uint8_t p[5 + OPT_MAX];
    if (len > OPT_MAX) len = OPT_MAX;
    p[0] = op;
    memcpy(p + 1, &len, 4);
    if (len) memcpy(p + 5, opt, len);
    m_send(m, mac, p, 5 + len);
}

static void m_send_mac(Matching *m, const uint8_t *to, uint8_t op, const uint8_t *mac) {
    uint8_t p[7] = { op };
    memcpy(p + 1, mac, 6);
    m_send(m, to, p, 7);
}

static void m_tell_children(Matching *m, uint8_t op, const uint8_t *mac) {
    for (int i = 0; i < 16; i++)
        if (m->peer[i].used && m->peer[i].state == MP_CHILD && memcmp(m->peer[i].mac, mac, 6))
            m_send_mac(m, m->peer[i].mac, op, mac);
}

static void m_drop(Matching *m, MPeer *p, uint32_t ev) {
    uint8_t mac[6];
    memcpy(mac, p->mac, 6);
    int was_child = p->state == MP_CHILD;
    p->used = 0;
    if (ev) m_event(m, ev, mac, NULL, 0);
    if (was_child) m_tell_children(m, PK_DEATH, mac);
}

/* Accept a pending request: the host answers with its option data and its other children. */
static void m_accept(Matching *m, MPeer *p, const uint8_t *opt, uint32_t optlen) {
    uint8_t pk[9 + OPT_MAX + 16 * 6];
    uint32_t nsib = 0;
    if (optlen > OPT_MAX) optlen = OPT_MAX;
    pk[0] = PK_ACCEPT;
    memcpy(pk + 1, &optlen, 4);
    if (optlen) memcpy(pk + 9, opt, optlen);
    for (int i = 0; i < 16; i++)
        if (m->peer[i].used && m->peer[i].state == MP_CHILD)
            memcpy(pk + 9 + optlen + 6 * nsib++, m->peer[i].mac, 6);
    memcpy(pk + 5, &nsib, 4);
    p->state = m->mode == M_PARENT ? MP_CHILD : MP_P2P;
    p->last = now_us();
    m_send(m, p->mac, pk, 9 + optlen + 6 * nsib);
    if (p->state == MP_CHILD) m_tell_children(m, PK_BIRTH, p->mac);
    m_event(m, EV_ESTABLISHED, p->mac, NULL, 0);
}

static void m_packet(Matching *m, const uint8_t *mac, const uint8_t *pk, int n) {
    uint32_t optlen = 0;
    if (n >= 5) memcpy(&optlen, pk + 1, 4);
    if (optlen > (uint32_t)n - 5u || optlen > OPT_MAX) optlen = 0;
    const uint8_t *opt = pk + 5;
    MPeer *p = m_peer(m, mac, 0);
    if (p) p->last = now_us();
    switch (pk[0]) {
    case PK_HELLO:     /* a host (or P2P peer) offers itself */
        if (m->mode == M_PARENT || (p && p->state != MP_OFFER)) break;
        if (m->mode == M_P2P && m_count(m, MP_P2P)) break;
        if (!p && !(p = m_peer(m, mac, 1))) break;
        p->state = MP_OFFER;
        m_event(m, EV_HELLO, mac, opt, optlen);
        break;
    case PK_JOIN:      /* a player asks to join us */
        if (m->mode == M_CHILD || (p && m_established(p))) break;
        if (!p && !(p = m_peer(m, mac, 1))) break;
        if (p->state == MP_IN_REQ) break;
        p->state = MP_IN_REQ;
        m_event(m, EV_REQUEST, mac, opt, optlen);
        break;
    case PK_ACCEPT:    /* our request was accepted */
        if (!p || p->state != MP_OUT_REQ || n < 9) break;
        {
            uint32_t nsib = 0;
            memcpy(&nsib, pk + 5, 4);
            optlen = 0;
            memcpy(&optlen, pk + 1, 4);
            if (optlen > OPT_MAX || 9u + optlen + 6u * (uint64_t)nsib > (uint32_t)n) break;
            p->state = m->mode == M_CHILD ? MP_PARENT : MP_P2P;
            m_event(m, EV_ACCEPT, mac, pk + 9, optlen);
            m_event(m, EV_ESTABLISHED, mac, NULL, 0);
            for (uint32_t i = 0; i < nsib && m->mode == M_CHILD; i++) {
                MPeer *sib = m_peer(m, pk + 9 + optlen + 6 * i, 1);
                if (sib) sib->state = MP_SIBLING;
            }
        }
        break;
    case PK_CANCEL:
        if (!p) break;
        m_drop(m, p, p->state == MP_OUT_REQ ? EV_DENY : p->state == MP_IN_REQ ? EV_CANCEL :
                     m_established(p) ? EV_LEAVE : 0);
        break;
    case PK_BYE:
        if (p) m_drop(m, p, m_established(p) ? EV_BYE : 0);
        break;
    case PK_BIRTH:     /* the host has a new child: our sibling */
        if (n >= 7 && p && p->state == MP_PARENT) {
            MPeer *sib = m_peer(m, pk + 1, 1);
            if (sib) sib->state = MP_SIBLING;
        }
        break;
    case PK_DEATH:
        if (n >= 7 && p && p->state == MP_PARENT) {
            MPeer *sib = m_peer(m, pk + 1, 0);
            if (sib && sib->state == MP_SIBLING) sib->used = 0;
        }
        break;
    default:           /* PING only refreshes `last` */
        break;
    }
}

static void m_service(Matching *m) {
    static uint8_t pk[65536];
    uint64_t now = now_us();
    for (;;) {
        uint32_t ip;
        int n = as_recvfrom(m->h, pk, (int)sizeof pk, &ip, NULL, 0);
        if (n == AS_AGAIN || n < 0) break;
        const Peer *peer = peer_by_ip(ip);
        if (peer && n >= 1) m_packet(m, peer->mac, pk, n < (int)sizeof pk ? n : (int)sizeof pk);
    }
    if ((m->mode == M_PARENT || m->mode == M_P2P) && now >= m->next_hello &&
        !(m->mode == M_P2P && m_count(m, MP_P2P))) {
        for (int i = 0; i < s_npeers; i++) m_send_opt(m, s_peers[i].mac, PK_HELLO, m->hello, m->hello_len);
        m->next_hello = now + (m->hello_int >= 100000u ? m->hello_int : 100000u);
    }
    const int ping = now >= m->next_ping;
    if (ping) m->next_ping = now + (m->keepalive_int >= 100000u ? m->keepalive_int : 100000u);
    const uint64_t dead = (uint64_t)(m->keepalive_int ? m->keepalive_int : 500000u) *
                          (m->init_count > 3u ? m->init_count : 3u);
    for (int i = 0; i < 16; i++) {
        MPeer *p = &m->peer[i];
        if (!p->used) continue;
        if (m_established(p)) {
            if (ping) { uint8_t z = PK_PING; m_send(m, p->mac, &z, 1); }
            if (now - p->last > (dead > 3000000u ? dead : 3000000u)) m_drop(m, p, EV_TIMEOUT);
        } else if (p->state == MP_OFFER && now - p->last > 5000000u) {
            p->used = 0;                  /* the host stopped offering itself */
        } else if (p->state == MP_OUT_REQ && now >= p->next_rexmt) {
            if (++p->tries > (int)(m->init_count > 3u ? m->init_count : 3u)) { m_drop(m, p, EV_TIMEOUT); continue; }
            m_send_opt(m, p->mac, PK_JOIN, m->join, m->join_len);
            p->next_rexmt = now + (m->rexmt_int >= 200000u ? m->rexmt_int : 200000u);
        }
    }
}

static void m_stop(Matching *m) {
    if (!m->running) return;
    uint8_t bye = PK_BYE;
    for (int i = 0; i < 16; i++) if (m_established(&m->peer[i])) m_send(m, m->peer[i].mac, &bye, 1);
    memset(m->peer, 0, sizeof m->peer);
    if (m->h >= 0) as_close(m->h);
    m->h = -1;
    m->running = 0;
}

static uint32_t h_MatchInit(CpuState *s) {
    (void)s;
    if (s_match_inited) return MATCH_ERR_ALREADY_INITIALIZED;
    s_match_inited = 1;
    return 0;
}

static uint32_t h_MatchTerm(CpuState *s) {
    (void)s;
    for (int i = 0; i < MAX_MATCHING; i++) if (s_m[i].used) m_stop(&s_m[i]);
    memset(s_m, 0, sizeof s_m);
    s_match_inited = 0;
    return 0;
}

/* sceNetAdhocMatchingCreate(int mode, int maxnum, u16 port, int rxbuflen, unsigned hello_int,
 * unsigned keepalive_int, int init_count, unsigned rexmt_int, callback) */
static uint32_t h_MatchCreate(CpuState *s) {
    if (!s_match_inited) return MATCH_ERR_NOT_INITIALIZED;
    if (A0 < M_PARENT || A0 > M_P2P) return MATCH_ERR_INVALID_MODE;
    if (A1 < 2 || A1 > 16) return MATCH_ERR_INVALID_MAXNUM;
    if (!A2) return MATCH_ERR_INVALID_PORT;
    for (int i = 0; i < MAX_MATCHING; i++)
        if (s_m[i].used && s_m[i].port == A2) return MATCH_ERR_PORT_IN_USE;
    for (int i = 0; i < MAX_MATCHING; i++) {
        if (s_m[i].used) continue;
        s_m[i] = (Matching){ .used = 1, .h = -1, .mode = A0, .maxnum = A1, .port = A2,
                             .hello_int = stack_arg(s, 0), .keepalive_int = stack_arg(s, 1),
                             .init_count = stack_arg(s, 2), .rexmt_int = stack_arg(s, 3),
                             .cb = stack_arg(s, 4), .gp = s->r[28] };
        return (uint32_t)i + 1;
    }
    return MATCH_ERR_ID_NOT_AVAIL;
}

/* sceNetAdhocMatchingStart(int id, int evthPri, int evthStack, int inthPri, int inthStack,
 * int optLen, void *optData) */
static uint32_t h_MatchStart(CpuState *s) {
    Matching *m = m_get(A0);
    if (!m) return MATCH_ERR_INVALID_ID;
    if (m->running) return MATCH_ERR_IS_RUNNING;
    uint32_t optlen = stack_arg(s, 1);
    if (optlen > OPT_MAX) return MATCH_ERR_INVALID_OPTLEN;
    if (!guest_read(stack_arg(s, 2), m->hello, optlen)) return MATCH_ERR_INVALID_ARG;
    m->hello_len = optlen;
    m->h = as_udp_open(s_bind_ip, (uint16_t)(m->port + s_off));
    if (m->h < 0) return m->h == AS_INUSE ? MATCH_ERR_PORT_IN_USE : MATCH_ERR_INVALID_ARG;
    m->evth_prio = A1;
    m->evth_stack = A2;
    m->running = 1;
    m->next_hello = m->next_ping = now_us();
    fprintf(stderr, "ADHOC: matching %u started as %s (up to %u players, port %u)\n", A0,
            m->mode == M_PARENT ? "host" : m->mode == M_CHILD ? "client" : "peer", m->maxnum, m->port);
    return 0;
}

static uint32_t h_MatchStop(CpuState *s) {
    Matching *m = m_get(A0);
    if (!m) return MATCH_ERR_INVALID_ID;
    m_stop(m);
    return 0;
}

static uint32_t h_MatchDelete(CpuState *s) {
    Matching *m = m_get(A0);
    if (!m) return MATCH_ERR_INVALID_ID;
    m_stop(m);
    m->used = 0;
    return 0;
}

/* sceNetAdhocMatchingSelectTarget(int id, u8 *mac, int optlen, void *opt): a client asks the host
 * it heard to let it join; a host accepts a player's request. */
static uint32_t h_MatchSelectTarget(CpuState *s) {
    Matching *m = m_get(A0);
    uint8_t mac[6], opt[OPT_MAX];
    if (!m) return MATCH_ERR_INVALID_ID;
    if (!m->running) return MATCH_ERR_NOT_RUNNING;
    if (!guest_read(A1, mac, 6)) return MATCH_ERR_INVALID_ARG;
    if (A2 > OPT_MAX) return MATCH_ERR_INVALID_OPTLEN;
    if (!guest_read(A3, opt, A2)) return MATCH_ERR_INVALID_ARG;
    MPeer *p = m_peer(m, mac, 0);
    if (!p) return MATCH_ERR_UNKNOWN_TARGET;
    if (m_established(p)) return MATCH_ERR_ALREADY_ESTABLISHED;
    if (p->state == MP_IN_REQ) {
        if (m->mode == M_PARENT && m_count(m, MP_CHILD) + 1 >= (int)m->maxnum) return MATCH_ERR_EXCEED_MAXNUM;
        m_accept(m, p, opt, A2);
        return 0;
    }
    if (p->state == MP_OUT_REQ) return MATCH_ERR_REQUEST_IN_PROGRESS;
    if (p->state != MP_OFFER || m->mode == M_PARENT) return MATCH_ERR_TARGET_NOT_READY;
    memcpy(m->join, opt, A2);
    m->join_len = A2;
    p->state = MP_OUT_REQ;
    p->tries = 1;
    p->next_rexmt = now_us() + (m->rexmt_int >= 200000u ? m->rexmt_int : 200000u);
    m_send_opt(m, mac, PK_JOIN, opt, A2);
    return 0;
}

/* sceNetAdhocMatchingCancelTarget(int id, u8 *mac) and ...WithOpt(id, mac, optlen, opt) */
static uint32_t match_cancel(CpuState *s, uint32_t optlen, uint32_t optaddr) {
    Matching *m = m_get(A0);
    uint8_t mac[6], opt[OPT_MAX];
    if (!m) return MATCH_ERR_INVALID_ID;
    if (!m->running) return MATCH_ERR_NOT_RUNNING;
    if (!guest_read(A1, mac, 6)) return MATCH_ERR_INVALID_ARG;
    if (optlen > OPT_MAX || !guest_read(optaddr, opt, optlen)) return MATCH_ERR_INVALID_OPTLEN;
    MPeer *p = m_peer(m, mac, 0);
    if (!p) return MATCH_ERR_UNKNOWN_TARGET;
    m_send_opt(m, mac, PK_CANCEL, opt, optlen);
    m_drop(m, p, 0);
    return 0;
}
static uint32_t h_MatchCancelTarget(CpuState *s) { return match_cancel(s, 0, 0); }
static uint32_t h_MatchCancelTargetWithOpt(CpuState *s) { return match_cancel(s, A2, A3); }

static uint32_t h_MatchSetHelloOpt(CpuState *s) {
    Matching *m = m_get(A0);
    if (!m) return MATCH_ERR_INVALID_ID;
    if (A1 > OPT_MAX) return MATCH_ERR_INVALID_OPTLEN;
    if (!guest_read(A2, m->hello, A1)) return MATCH_ERR_INVALID_ARG;
    m->hello_len = A1;
    return 0;
}

/* sceNetAdhocMatchingGetMembers(int id, int *buflen, SceNetAdhocMatchingMember *buf): entries of
 * {next, mac[6], pad[2]}: a host lists itself and its children; a client its host, itself and
 * its siblings; a P2P peer itself and its partner. A NULL buffer asks for the size needed. */
static uint32_t h_MatchGetMembers(CpuState *s) {
    Matching *m = m_get(A0);
    if (!m) return MATCH_ERR_INVALID_ID;
    if (!m->running) return MATCH_ERR_NOT_RUNNING;
    if (!A1 || !sr_guest_span_writable(A1, 4)) return MATCH_ERR_INVALID_ARG;
    uint8_t list[17][6], self[6];
    int n = 0, connected = 0;
    self_mac(self);
    for (int i = 0; i < 16; i++) if (m->peer[i].used && m->peer[i].state == MP_PARENT) {
        memcpy(list[n++], m->peer[i].mac, 6);
        connected = 1;
    }
    memcpy(list[n++], self, 6);
    for (int i = 0; i < 16; i++) {
        const MPeer *p = &m->peer[i];
        if (p->used && (p->state == MP_CHILD || p->state == MP_P2P || (connected && p->state == MP_SIBLING)))
            memcpy(list[n++], p->mac, 6);
    }
    if (!A2) { MEM_W32(A1, (uint32_t)n * 12u); return 0; }
    int room = (int)(MEM_R32(A1) / 12u);
    if (room > n) room = n;
    if (room && !sr_guest_span_writable(A2, (uint32_t)room * 12u)) return MATCH_ERR_INVALID_ARG;
    for (int i = 0; i < room; i++) {
        uint32_t e = A2 + 12u * (uint32_t)i;
        MEM_W32(e, i + 1 < room ? e + 12u : 0u);
        for (int b = 0; b < 6; b++) MEM_W8(e + 4u + (uint32_t)b, list[i][b]);
        MEM_W16(e + 10u, 0);
    }
    MEM_W32(A1, (uint32_t)room * 12u);
    return 0;
}

/* ---- the scheduler's ad hoc service ----------------------------------------------------- */
static uint64_t s_next_service;

static int adhoc_active(void) {
    if (C.inited || s_call_count || s_call_uid) return 1;
    for (int i = 0; i < MAX_MATCHING; i++) if (s_m[i].running) return 1;
    return 0;
}

static uint64_t adhoc_next_deadline(void) {
    if (!adhoc_active()) return UINT64_MAX;
    return s_call_count && !s_call_uid ? 0u : s_next_service;
}

static void adhoc_service(void) {
    s_next_service = now_us() + 2000u;
    if (C.inited) srv_poll();
    for (int i = 0; i < MAX_MATCHING; i++) if (s_m[i].running) m_service(&s_m[i]);
    calls_pump();
}

/* Online play off (the default), each call keeps the offline policy's controlled refusal. */
#define ADHOC_REGISTER(nid, name, fn) \
    (s_on ? sr_hle_register(nid, name, fn) : sr_hle_register_unsupported(nid, name, 0x80010086u))

void hle_register_adhoc(void) {
    if (hle_adhoc_enabled()) sched_set_adhoc_hooks(adhoc_next_deadline, adhoc_service);
    sr_hle_register(0xd27961c9u, "sceNetEtherStrton", h_NetEtherStrton);   /* needs no network */
    /* The rest of the public ad hoc surface (PSPSDK pspnet_adhoc*.h) Patapon 3 does not call:
     * named refusals in either mode (#1139 phase 0's offline policy). */
    sr_hle_register_unsupported(0xf7d13214u, "sceNetAdhocDiscoverStop", 0x80010086u);
    sr_hle_register_unsupported(0x7f75c338u, "sceNetAdhocGameModeCreateMaster", 0x80010086u);
    sr_hle_register_unsupported(0x3278ab0cu, "sceNetAdhocGameModeCreateReplica", 0x80010086u);
    sr_hle_register_unsupported(0xa0229362u, "sceNetAdhocGameModeDeleteMaster", 0x80010086u);
    sr_hle_register_unsupported(0x0b2228e9u, "sceNetAdhocGameModeDeleteReplica", 0x80010086u);
    sr_hle_register_unsupported(0x98c204c8u, "sceNetAdhocGameModeUpdateMaster", 0x80010086u);
    sr_hle_register_unsupported(0xfa324b4eu, "sceNetAdhocGameModeUpdateReplica", 0x80010086u);
    sr_hle_register_unsupported(0xc7c1fc57u, "sceNetAdhocGetPdpStat", 0x80010086u);
    sr_hle_register_unsupported(0xb9685118u, "sceNetAdhocGetPtpStat", 0x80010086u);
    sr_hle_register_unsupported(0xa5c055ceu, "sceNetAdhocctlCreateEnterGameMode", 0x80010086u);
    sr_hle_register_unsupported(0xcf8e084du, "sceNetAdhocctlExitGameMode", 0x80010086u);
    sr_hle_register_unsupported(0x99560abeu, "sceNetAdhocctlGetAddrByName", 0x80010086u);
    sr_hle_register_unsupported(0x362cbe8fu, "sceNetAdhocctlGetAdhocId", 0x80010086u);
    sr_hle_register_unsupported(0x5a014ce0u, "sceNetAdhocctlGetGameModeInfo", 0x80010086u);
    sr_hle_register_unsupported(0x8916c003u, "sceNetAdhocctlGetNameByAddr", 0x80010086u);
    sr_hle_register_unsupported(0xded9d28eu, "sceNetAdhocctlGetParameter", 0x80010086u);
    sr_hle_register_unsupported(0x8db83fdcu, "sceNetAdhocctlGetPeerInfo", 0x80010086u);
    sr_hle_register_unsupported(0xe162cb14u, "sceNetAdhocctlGetPeerList", 0x80010086u);
    sr_hle_register_unsupported(0x81aee1beu, "sceNetAdhocctlGetScanInfo", 0x80010086u);
    sr_hle_register_unsupported(0x5e7f79c9u, "sceNetAdhocctlJoin", 0x80010086u);
    sr_hle_register_unsupported(0x1ff89745u, "sceNetAdhocctlJoinEnterGameMode", 0x80010086u);
    sr_hle_register_unsupported(0x08fff7a0u, "sceNetAdhocctlScan", 0x80010086u);
    sr_hle_register_unsupported(0xec19337du, "sceNetAdhocMatchingAbortSendData", 0x80010086u);
    sr_hle_register_unsupported(0xb5d96c2au, "sceNetAdhocMatchingGetHelloOpt", 0x80010086u);
    sr_hle_register_unsupported(0x40f8f435u, "sceNetAdhocMatchingGetPoolMaxAlloc", 0x80010086u);
    sr_hle_register_unsupported(0x9c5cfb7du, "sceNetAdhocMatchingGetPoolStat", 0x80010086u);
    sr_hle_register_unsupported(0xf79472d7u, "sceNetAdhocMatchingSendData", 0x80010086u);
    ADHOC_REGISTER(0x39af39a6u, "sceNetInit", h_NetInit);
    ADHOC_REGISTER(0x281928a9u, "sceNetTerm", h_NetTerm);
    ADHOC_REGISTER(0xcc393e48u, "sceNetGetMallocStat", h_NetGetMallocStat);
    ADHOC_REGISTER(0xe1d621d7u, "sceNetAdhocInit", h_AdhocInit);
    ADHOC_REGISTER(0xa62c6f57u, "sceNetAdhocTerm", h_AdhocTerm);
    ADHOC_REGISTER(0x6f92741bu, "sceNetAdhocPdpCreate", h_PdpCreate);
    ADHOC_REGISTER(0x7f27bb5eu, "sceNetAdhocPdpDelete", h_PdpDelete);
    ADHOC_REGISTER(0xabed3790u, "sceNetAdhocPdpSend", h_PdpSend);
    ADHOC_REGISTER(0xdfe53e03u, "sceNetAdhocPdpRecv", h_PdpRecv);
    ADHOC_REGISTER(0x877f6d66u, "sceNetAdhocPtpOpen", h_PtpOpen);
    ADHOC_REGISTER(0xfc6fc07bu, "sceNetAdhocPtpConnect", h_PtpConnect);
    ADHOC_REGISTER(0xe08bdac1u, "sceNetAdhocPtpListen", h_PtpListen);
    ADHOC_REGISTER(0x9df81198u, "sceNetAdhocPtpAccept", h_PtpAccept);
    ADHOC_REGISTER(0x4da4c788u, "sceNetAdhocPtpSend", h_PtpSend);
    ADHOC_REGISTER(0x8bea2b3eu, "sceNetAdhocPtpRecv", h_PtpRecv);
    ADHOC_REGISTER(0x9ac2eeacu, "sceNetAdhocPtpFlush", h_PtpFlush);
    ADHOC_REGISTER(0x157e6225u, "sceNetAdhocPtpClose", h_PtpClose);
    ADHOC_REGISTER(0xe26f226eu, "sceNetAdhocctlInit", h_CtlInit);
    ADHOC_REGISTER(0x9d689e13u, "sceNetAdhocctlTerm", h_CtlTerm);
    ADHOC_REGISTER(0x20b317a0u, "sceNetAdhocctlAddHandler", h_CtlAddHandler);
    ADHOC_REGISTER(0x6402490bu, "sceNetAdhocctlDelHandler", h_CtlDelHandler);
    ADHOC_REGISTER(0xec0635c1u, "sceNetAdhocctlCreate", h_CtlConnect);
    ADHOC_REGISTER(0x0ad043edu, "sceNetAdhocctlConnect", h_CtlConnect);
    ADHOC_REGISTER(0x34401d65u, "sceNetAdhocctlDisconnect", h_CtlDisconnect);
    ADHOC_REGISTER(0x75ecd386u, "sceNetAdhocctlGetState", h_CtlGetState);
    ADHOC_REGISTER(0x2a2a1e07u, "sceNetAdhocMatchingInit", h_MatchInit);
    ADHOC_REGISTER(0x7945ecdau, "sceNetAdhocMatchingTerm", h_MatchTerm);
    ADHOC_REGISTER(0xca5eda6fu, "sceNetAdhocMatchingCreate", h_MatchCreate);
    ADHOC_REGISTER(0x93ef3843u, "sceNetAdhocMatchingStart", h_MatchStart);
    ADHOC_REGISTER(0x32b156b3u, "sceNetAdhocMatchingStop", h_MatchStop);
    ADHOC_REGISTER(0xf16eaf4fu, "sceNetAdhocMatchingDelete", h_MatchDelete);
    ADHOC_REGISTER(0x5e3d4b79u, "sceNetAdhocMatchingSelectTarget", h_MatchSelectTarget);
    ADHOC_REGISTER(0xea3c6108u, "sceNetAdhocMatchingCancelTarget", h_MatchCancelTarget);
    ADHOC_REGISTER(0x8f58bedfu, "sceNetAdhocMatchingCancelTargetWithOpt", h_MatchCancelTargetWithOpt);
    ADHOC_REGISTER(0xb58e61b7u, "sceNetAdhocMatchingSetHelloOpt", h_MatchSetHelloOpt);
    ADHOC_REGISTER(0xc58bcd9eu, "sceNetAdhocMatchingGetMembers", h_MatchGetMembers);
}
