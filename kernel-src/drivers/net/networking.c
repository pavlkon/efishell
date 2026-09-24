// SPDX-License-Identifier: GPL-2.0-only
#include "net_internal.h"

/* Ethernet/IP host stack; all mutable protocol state is protected by net_lock. */
#define NET_MAX(a, b) ((a) > (b) ? (a) : (b))
#define NET_RXQ 64u
#define NET_NEIGHBORS 64u
#define NET_DGRAMS 8u
#define NET_MSS 1440u
#define NET_STREAM 16384u
#define NET_FRAG_BYTES 8192u
#define NET_FRAGS 8u
#define NET_NONE 0xffffffffu
#define TCP_FIN 1u
#define TCP_SYN 2u
#define TCP_RST 4u
#define TCP_PSH 8u
#define TCP_ACK 16u
enum {
    NS_NEW,
    NS_BOUND,
    NS_LISTEN,
    NS_SYN_SENT,
    NS_SYN_RCVD,
    NS_ESTABLISHED,
    NS_FIN_WAIT1,
    NS_FIN_WAIT2,
    NS_CLOSE_WAIT,
    NS_CLOSING,
    NS_LAST_ACK,
    NS_TIME_WAIT,
    NS_ERROR
};

typedef struct {
    BOOLEAN used;
    UINT32 interface, family;
    UINT8 address[16], mac[6];
    UINT64 expires, requested;
    UINT32 probes;
} net_neighbor;
typedef struct {
    k_net_address from;
    UINT32 length;
    UINT8 bytes[1472];
} net_datagram;
typedef struct {
    BOOLEAN used, detached, peer_fin, close_requested, retransmitted;
    UINT32 id, owner, family, type, state, parent, backlog;
    k_net_address local, peer;
    UINT32 snd_una, snd_nxt, rcv_nxt, iss, mss, peer_window, cwnd, threshold;
    UINT32 rx_head, rx_count, tx_count, q_head, q_count, fin_seq, dupacks;
    UINT32 srtt, rttvar, rto, retry;
    UINT64 deadline, sent_at, activity, persist_at;
    INT32 error;
    UINT8 rx[NET_STREAM], tx[NET_STREAM];
    net_datagram datagrams[NET_DGRAMS];
} net_socket;
typedef struct {
    BOOLEAN used;
    UINT32 interface, family, protocol, id, final_size;
    UINT8 src[16], dst[16], bytes[NET_FRAG_BYTES], seen[NET_FRAG_BYTES / 8];
    UINT64 expires;
} net_fragment;
static k_mutex net_lock = K_MUTEX_INIT;
static net_device net_devices[K_NET_MAX_IF];
static net_socket net_sockets[K_NET_MAX_SOCKETS];
static net_neighbor net_neighbors[NET_NEIGHBORS];
static net_fragment net_fragments[NET_FRAGS];
static k_net_frame net_queue[NET_RXQ];
static UINT32 net_devices_count, net_head, net_queued, net_next_socket = 1, net_ephemeral = 49152;
static UINT16 net_ip_id;
static UINT64 net_key[2], net_counter, net_gc_at;
static BOOLEAN net_ready;
static struct {
    BOOLEAN used, done;
    UINT32 token, owner, ms;
    k_net_address peer;
    UINT64 sent;
} net_pings[8];
static struct {
    k_wifi_info info;
    k_wifi_ops ops;
    void *context;
} net_wifi[K_WIFI_MAX];
static UINT32 net_wifi_count;
static UINT16 net_be16(const void *p)
{
    const UINT8 *b = p;
    return (UINT16)((b[0] << 8) | b[1]);
}
static UINT32 net_be32(const void *p)
{
    const UINT8 *b = p;
    return ((UINT32)b[0] << 24) | ((UINT32)b[1] << 16) | ((UINT32)b[2] << 8) | b[3];
}
static void net_put16(void *p, UINT16 x)
{
    UINT8 *b = p;
    b[0] = (UINT8)(x >> 8);
    b[1] = (UINT8)x;
}
static void net_put32(void *p, UINT32 x)
{
    UINT8 *b = p;
    b[0] = (UINT8)(x >> 24);
    b[1] = (UINT8)(x >> 16);
    b[2] = (UINT8)(x >> 8);
    b[3] = (UINT8)x;
}
static BOOLEAN net_zero(const UINT8 *p, UINT32 n)
{
    while (n--)
        if (*p++)
            return FALSE;
    return TRUE;
}
static BOOLEAN net_equal(const UINT8 *a, const UINT8 *b, UINT32 n) { return memcmp(a, b, n) == 0; }
static UINT32 net_addr_bytes(UINT32 family) { return family == K_AF_INET ? 4 : 16; }
static BOOLEAN net_multicast(UINT32 family, const UINT8 *a)
{
    return family == 4 ? (a[0] >= 224) : a[0] == 255;
}
static BOOLEAN net_prefix(const UINT8 *a, const UINT8 *b, UINT32 bits)
{
    for (UINT32 i = 0; i < bits / 8; ++i)
        if (a[i] != b[i])
            return FALSE;
    return !(bits % 8) || !((a[bits / 8] ^ b[bits / 8]) & (0xffu << (8 - bits % 8)));
}
static UINT32 net_sum(UINT32 sum, const void *data, UINT32 n)
{
    const UINT8 *p = data;
    while (n > 1) {
        sum += net_be16(p);
        p += 2;
        n -= 2;
    }
    if (n)
        sum += (UINT32)*p << 8;
    return sum;
}
static UINT16 net_finish(UINT32 sum)
{
    while (sum >> 16)
        sum = (sum & 65535) + (sum >> 16);
    return (UINT16)~sum;
}
UINT16 k_net_checksum(const void *data, UINT32 n)
{
    return (!data && n) ? 0 : net_finish(net_sum(0, data, n));
}
static UINT16 net_transport_sum(UINT32 family, const UINT8 *src, const UINT8 *dst, UINT32 proto,
                                const void *p, UINT32 n)
{
    UINT32 sum = net_sum(0, src, net_addr_bytes(family));
    sum = net_sum(sum, dst, net_addr_bytes(family));
    sum += proto + (n & 65535) + (n >> 16);
    return net_finish(net_sum(sum, p, n));
}
static UINT64 net_rot(UINT64 n, UINT32 b) { return (n << b) | (n >> (64 - b)); }
static void net_hash_round(UINT64 v[4])
{
    v[0] += v[1];
    v[1] = net_rot(v[1], 13);
    v[1] ^= v[0];
    v[0] = net_rot(v[0], 32);
    v[2] += v[3];
    v[3] = net_rot(v[3], 16);
    v[3] ^= v[2];
    v[0] += v[3];
    v[3] = net_rot(v[3], 21);
    v[3] ^= v[0];
    v[2] += v[1];
    v[1] = net_rot(v[1], 17);
    v[1] ^= v[2];
    v[2] = net_rot(v[2], 32);
}
static UINT32 net_random(void)
{
    UINT64 v[4] = {0x736f6d6570736575ULL ^ net_key[0], 0x646f72616e646f6dULL ^ net_key[1],
                   0x6c7967656e657261ULL ^ net_key[0], 0x7465646279746573ULL ^ net_key[1]};
    UINT64 m = ++net_counter;
    v[3] ^= m;
    net_hash_round(v);
    net_hash_round(v);
    v[0] ^= m;
    m = 8ULL << 56;
    v[3] ^= m;
    net_hash_round(v);
    net_hash_round(v);
    v[0] ^= m;
    v[2] ^= 255;
    for (UINT32 i = 0; i < 4; ++i)
        net_hash_round(v);
    return (UINT32)(v[0] ^ v[1] ^ v[2] ^ v[3]);
}
UINT32 k_net_nonce(void)
{
    if (!net_ready || k_mutex_lock(&net_lock))
        return 0;
    UINT32 value = net_random();
    k_mutex_unlock(&net_lock);
    return value;
}
static BOOLEAN net_privileged(void)
{
    task *t = current_task();
    return !t || !t->process || t->process->execution_class == K_EXEC_RING1;
}
static BOOLEAN net_canceled(void)
{
    task *t = current_task();
    return t && t->control_request;
}
static int net_lock_enter(void) { return net_ready ? k_mutex_lock(&net_lock) : K_ENETDOWN; }
static BOOLEAN net_address_ok(const k_net_address *a)
{
    return a && (a->family == 4 || a->family == 6) && !a->reserved &&
           (a->interface == K_NET_ANY_IF || a->interface < net_devices_count) &&
           (a->family != 4 || net_zero(a->bytes + 4, 12));
}
static int net_parse4(const char *s, UINT8 *out)
{
    for (UINT32 i = 0; i < 4; ++i) {
        UINT32 x = 0, n = 0;
        while (*s >= '0' && *s <= '9') {
            x = x * 10 + (UINT32)(*s++ - '0');
            if (++n > 3 || x > 255)
                return K_EINVAL;
        }
        if (!n)
            return K_EINVAL;
        out[i] = (UINT8)x;
        if (i < 3) {
            if (*s++ != '.')
                return K_EINVAL;
        } else if (*s)
            return K_EINVAL;
    }
    return 0;
}
int k_net_parse(const char *s, UINT32 family, k_net_address *out)
{
    if (!s || !out || (family != 4 && family != 6))
        return K_EINVAL;
    k_net_address a = {0};
    a.family = family;
    a.interface = K_NET_ANY_IF;
    if (family == 4) {
        int e = net_parse4(s, a.bytes);
        if (e)
            return e;
        *out = a;
        return 0;
    }
    UINT16 words[8] = {0};
    UINT32 n = 0;
    INT32 gap = -1;
    if (*s == ':') {
        if (s[1] != ':')
            return K_EINVAL;
        s += 2;
        gap = 0;
    }
    while (*s) {
        if (n == 8)
            return K_EINVAL;
        UINT32 v = 0, digits = 0;
        while (*s && *s != ':') {
            UINT32 d = *s >= '0' && *s <= '9'   ? (UINT32)(*s - '0')
                       : *s >= 'a' && *s <= 'f' ? (UINT32)(*s - 'a' + 10)
                       : *s >= 'A' && *s <= 'F' ? (UINT32)(*s - 'A' + 10)
                                                : 99;
            if (d > 15 || ++digits > 4)
                return K_EINVAL;
            v = (v << 4) | d;
            ++s;
        }
        if (!digits)
            return K_EINVAL;
        words[n++] = (UINT16)v;
        if (*s) {
            ++s;
            if (*s == ':') {
                if (gap >= 0)
                    return K_EINVAL;
                gap = (INT32)n;
                ++s;
            } else if (!*s)
                return K_EINVAL;
        }
    }
    if (gap < 0 && n != 8)
        return K_EINVAL;
    if (gap >= 0) {
        if (n >= 8)
            return K_EINVAL;
        UINT32 shift = 8 - n;
        for (INT32 i = (INT32)n - 1; i >= gap; --i)
            words[i + shift] = words[i];
        for (UINT32 i = (UINT32)gap; i < (UINT32)gap + shift; ++i)
            words[i] = 0;
    }
    for (UINT32 i = 0; i < 8; ++i)
        net_put16(a.bytes + 2 * i, words[i]);
    *out = a;
    return 0;
}
int k_net_format(const k_net_address *a, char *out, UINT32 capacity)
{
    if (!a || !out || (a->family != 4 && a->family != 6))
        return K_EINVAL;
    char b[48];
    UINT32 n = 0;
    if (a->family == 4) {
        for (UINT32 i = 0; i < 4; ++i) {
            UINT32 v = a->bytes[i];
            if (i)
                b[n++] = '.';
            if (v >= 100)
                b[n++] = (char)('0' + v / 100);
            if (v >= 10)
                b[n++] = (char)('0' + v / 10 % 10);
            b[n++] = (char)('0' + v % 10);
        }
    } else {
        static const char h[] = "0123456789abcdef";
        for (UINT32 i = 0; i < 8; ++i) {
            if (i)
                b[n++] = ':';
            UINT16 v = net_be16(a->bytes + i * 2);
            BOOLEAN shown = FALSE;
            for (INT32 shift = 12; shift >= 0; shift -= 4)
                if ((v >> shift) || shown || !shift) {
                    b[n++] = h[(v >> shift) & 15];
                    shown = TRUE;
                }
        }
    }
    if (capacity <= n)
        return K_E2BIG;
    mem_copy(out, b, n);
    out[n] = 0;
    return 0;
}
int net_enqueue(UINT32 id, const void *p, UINT32 n)
{
    net_device *d = &net_devices[id];
    if (n < 14 || n > K_NET_FRAME_MAX) {
        ++d->info.errors;
        return K_EMSGSIZE;
    }
    if (net_queued == NET_RXQ) {
        ++d->info.dropped;
        return K_EAGAIN;
    }
    k_net_frame *f = &net_queue[(net_head + net_queued++) % NET_RXQ];
    f->interface = id;
    f->length = n;
    f->tick = k_ticks();
    mem_copy(f->bytes, p, n);
    return 0;
}
static int net_ether(net_device *d, const UINT8 mac[6], UINT16 type, const void *p, UINT32 n)
{
    if (n > 1500)
        return K_EMSGSIZE;
    UINT8 frame[1514];
    mem_copy(frame, mac, 6);
    mem_copy(frame + 6, d->info.mac, 6);
    net_put16(frame + 12, type);
    mem_copy(frame + 14, p, n);
    return nic_transmit(d, frame, n + 14);
}
static void net_receive_frame(const k_net_frame *);
static int net_ip_send(UINT32, UINT32, const UINT8 *, const UINT8 *, UINT32, const void *, UINT32,
                       UINT32);
static void net_tcp_pump(net_socket *, UINT64);
static void net_poll_protocols(UINT64);
static net_socket *net_socket_owned(UINT32 h)
{
    for (UINT32 i = 0; i < K_NET_MAX_SOCKETS; ++i) {
        net_socket *s = &net_sockets[i];
        if (s->used && s->id == h && !s->detached && s->owner == k_task_id())
            return s;
    }
    return NULL;
}
static net_socket *net_socket_new(UINT32 family, UINT32 type, UINT32 owner)
{
    if (!net_next_socket)
        return NULL;
    for (UINT32 i = 0; i < K_NET_MAX_SOCKETS; ++i)
        if (!net_sockets[i].used) {
            net_socket *s = &net_sockets[i];
            mem_zero(s, sizeof(*s));
            s->used = TRUE;
            s->id = net_next_socket++;
            s->owner = owner;
            s->family = family;
            s->type = type;
            s->local.family = s->peer.family = family;
            s->local.interface = s->peer.interface = NET_NONE;
            s->mss = family == 6 ? 1220 : 536;
            s->rto = 1000;
            s->peer_window = 65535;
            s->cwnd = s->mss * 2;
            s->threshold = 65535;
            s->activity = k_uptime_ms();
            return s;
        }
    return NULL;
}
static BOOLEAN net_port_busy(net_socket *self, const k_net_address *a)
{
    for (UINT32 i = 0; i < K_NET_MAX_SOCKETS; ++i) {
        net_socket *s = &net_sockets[i];
        if (!s->used || s == self || s->parent || s->family != self->family ||
            s->type != self->type || s->local.port != a->port)
            continue;
        if (s->local.interface != NET_NONE && a->interface != NET_NONE &&
            s->local.interface != a->interface)
            continue;
        UINT32 n = net_addr_bytes(a->family);
        if (net_zero(a->bytes, n) || net_zero(s->local.bytes, n) ||
            net_equal(a->bytes, s->local.bytes, n))
            return TRUE;
    }
    return FALSE;
}
static int net_autobind(net_socket *s)
{
    if (s->local.port)
        return 0;
    net_ephemeral = 49152 + (net_random() & 16383);
    for (UINT32 tries = 0; tries < 16384; ++tries) {
        s->local.port = (UINT16)net_ephemeral++;
        if (net_ephemeral > 65535)
            net_ephemeral = 49152;
        if (!net_port_busy(s, &s->local))
            return 0;
    }
    s->local.port = 0;
    return K_EADDRINUSE;
}
static BOOLEAN net_local(net_device *d, UINT32 family, const UINT8 *a)
{
    if (family == 4)
        return (d->info.flags & K_NET_READY4) && net_equal(a, d->info.config.address, 4);
    return (d->info.flags & K_NET_READY6) &&
           (net_equal(a, d->info.link_local6, 16) ||
            (!d->dadglobal && !net_zero(d->info.config.address6, 16) &&
             net_equal(a, d->info.config.address6, 16)));
}
static BOOLEAN net_broadcast4(net_device *d, const UINT8 *a)
{
    if (net_be32(a) == 0xffffffff)
        return TRUE;
    UINT32 mask = net_be32(d->info.config.mask), ip = net_be32(d->info.config.address);
    return mask && mask != 0xffffffff && net_be32(a) == (ip | ~mask);
}
static int net_route(const k_net_address *to, UINT32 bound, UINT32 *id, UINT8 src[16])
{
    UINT32 preferred = to->interface != NET_NONE ? to->interface : bound;
    UINT32 n = net_addr_bytes(to->family);
    if (preferred != NET_NONE && preferred >= net_devices_count)
        return K_ENETUNREACH;
    INT32 fallback = -1;
    for (UINT32 i = 0; i < net_devices_count; ++i) {
        net_device *d = &net_devices[i];
        if (preferred != NET_NONE && i != preferred)
            continue;
        if (d->dead || !(d->info.flags & K_NET_UP) || !(d->info.flags & K_NET_LINK))
            continue;
        BOOLEAN match = FALSE;
        const UINT8 *source = to->family == 4 ? d->info.config.address : d->info.config.address6;
        if (to->family == 4) {
            UINT32 ip = net_be32(to->bytes), local = net_be32(source),
                   mask = net_be32(d->info.config.mask);
            if (!i) {
                match = net_equal(to->bytes, d->info.config.address, 4);
                source = d->info.config.address;
            } else {
                if ((ip >> 24) == 127)
                    continue;
                match = net_broadcast4(d, to->bytes) || (mask && (ip & mask) == (local & mask));
                if (!(d->info.flags & K_NET_READY4) && !net_broadcast4(d, to->bytes))
                    continue;
                if (!net_zero(d->info.config.gateway, 4))
                    fallback = (INT32)i;
            }
        } else {
            if (!i) {
                static const UINT8 one[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
                match = net_equal(to->bytes, one, 16);
                source = d->info.link_local6;
            } else {
                if (!(d->info.flags & K_NET_READY6))
                    continue;
                if ((to->bytes[0] == 0xfe && (to->bytes[1] & 0xc0) == 0x80) ||
                    to->bytes[0] == 255) {
                    match = TRUE;
                    source = d->info.link_local6;
                } else if (!d->dadglobal && !net_zero(source, 16))
                    match = net_prefix(to->bytes, source, d->info.config.prefix6);
                if (!net_zero(d->info.config.gateway6, 16) && !d->dadglobal &&
                    !net_zero(source, 16))
                    fallback = (INT32)i;
            }
        }
        if (match) {
            *id = i;
            mem_zero(src, 16);
            mem_copy(src, source, n);
            return 0;
        }
    }
    if (fallback >= 0) {
        net_device *d = &net_devices[fallback];
        *id = (UINT32)fallback;
        mem_zero(src, 16);
        mem_copy(src, to->family == 4 ? d->info.config.address : d->info.config.address6, n);
        return 0;
    }
    return K_ENETUNREACH;
}
static net_neighbor *net_neighbor_find(UINT32 id, UINT32 family, const UINT8 *a, BOOLEAN create)
{
    net_neighbor *free = NULL;
    UINT64 now = k_uptime_ms();
    for (UINT32 i = 0; i < NET_NEIGHBORS; ++i) {
        net_neighbor *e = &net_neighbors[i];
        if (e->used && e->interface == id && e->family == family &&
            net_equal(e->address, a, net_addr_bytes(family)))
            return e;
        if (!e->used || e->expires <= now)
            free = e;
    }
    if (!create || !free)
        return NULL;
    mem_zero(free, sizeof(*free));
    free->used = TRUE;
    free->interface = id;
    free->family = family;
    mem_copy(free->address, a, net_addr_bytes(family));
    free->expires = now + 10000;
    return free;
}
static void net_neighbor_learn(UINT32 id, UINT32 family, const UINT8 *a, const UINT8 *mac,
                               BOOLEAN solicited)
{
    if ((mac[0] & 1) || net_zero(mac, 6) || net_zero(a, net_addr_bytes(family)) ||
        net_multicast(family, a))
        return;
    net_neighbor *e = net_neighbor_find(id, family, a, TRUE);
    if (!e)
        return;
    if (!solicited && !net_zero(e->mac, 6) && !net_equal(e->mac, mac, 6))
        return;
    mem_copy(e->mac, mac, 6);
    e->expires = k_uptime_ms() + (solicited ? 30000 : 5000);
    e->probes = 0;
}
static int net_arp_send(net_device *d, UINT16 op, const UINT8 *mac, const UINT8 *spa,
                        const UINT8 *tpa)
{
    UINT8 p[28] = {0};
    net_put16(p, 1);
    net_put16(p + 2, 0x0800);
    p[4] = 6;
    p[5] = 4;
    net_put16(p + 6, op);
    mem_copy(p + 8, d->info.mac, 6);
    mem_copy(p + 14, spa, 4);
    if (op == 2)
        mem_copy(p + 18, mac, 6);
    mem_copy(p + 24, tpa, 4);
    return net_ether(d, mac, 0x0806, p, 28);
}
static int net_nd_solicit(net_device *d, const UINT8 *target, BOOLEAN dad)
{
    UINT8 dst[16] = {0xff, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0xff, 0, 0, 0}, src[16] = {0},
          p[32] = {0};
    mem_copy(dst + 13, target + 13, 3);
    p[0] = 135;
    mem_copy(p + 8, target, 16);
    UINT32 n = 24;
    if (!dad) {
        mem_copy(src, d->info.link_local6, 16);
        p[24] = 1;
        p[25] = 1;
        mem_copy(p + 26, d->info.mac, 6);
        n = 32;
    }
    net_put16(p + 2, net_transport_sum(6, src, dst, 58, p, n));
    return net_ip_send(d->info.id, 6, src, dst, 58, p, n, 255);
}
static int net_resolve_mac(net_device *d, UINT32 family, const UINT8 *dst, UINT8 *mac)
{
    if (d->kind == NIC_LOOP || net_local(d, family, dst)) {
        mem_copy(mac, d->info.mac, 6);
        return 0;
    }
    if (family == 4 && net_broadcast4(d, dst)) {
        memset(mac, 255, 6);
        return 0;
    }
    if (family == 6 && dst[0] == 255) {
        mac[0] = 0x33;
        mac[1] = 0x33;
        mem_copy(mac + 2, dst + 12, 4);
        return 0;
    }
    const UINT8 *next = dst;
    if (family == 4) {
        UINT32 mask = net_be32(d->info.config.mask);
        if ((net_be32(dst) & mask) != (net_be32(d->info.config.address) & mask))
            next = d->info.config.gateway;
    } else if (!(dst[0] == 0xfe && (dst[1] & 0xc0) == 0x80) &&
               !net_prefix(dst, d->info.config.address6, d->info.config.prefix6))
        next = d->info.config.gateway6;
    if (net_zero(next, net_addr_bytes(family)))
        return K_ENETUNREACH;
    net_neighbor *e = net_neighbor_find(d->info.id, family, next, TRUE);
    if (!e)
        return K_ENOSPC;
    UINT64 now = k_uptime_ms();
    if (!net_zero(e->mac, 6) && e->expires > now) {
        mem_copy(mac, e->mac, 6);
        return 0;
    }
    if (e->expires <= now) {
        mem_zero(e->mac, 6);
        e->probes = 0;
        e->requested = 0;
        e->expires = now + 10000;
    }
    if (e->probes >= 3)
        return K_ENETUNREACH;
    if (!e->requested || now - e->requested >= 1000) {
        int result;
        if (family == 4) {
            UINT8 broadcast[6] = {255, 255, 255, 255, 255, 255};
            result = net_arp_send(d, 1, broadcast, d->info.config.address, next);
        } else
            result = net_nd_solicit(d, next, FALSE);
        if (!result) {
            e->requested = now ? now : 1;
            ++e->probes;
        }
    }
    return K_EAGAIN;
}
static int net_ip_send(UINT32 id, UINT32 family, const UINT8 *src, const UINT8 *dst, UINT32 proto,
                       const void *bytes, UINT32 n, UINT32 ttl)
{
    if (id >= net_devices_count)
        return K_ENETUNREACH;
    net_device *d = &net_devices[id];
    UINT32 h = family == 4 ? 20 : 40;
    if (n > d->info.mtu - h)
        return K_EMSGSIZE;
    UINT8 packet[1500], mac[6];
    mem_zero(packet, h);
    int e = net_resolve_mac(d, family, dst, mac);
    if (e)
        return e;
    if (family == 4) {
        packet[0] = 0x45;
        net_put16(packet + 2, (UINT16)(n + 20));
        net_put16(packet + 4, ++net_ip_id);
        if (proto == 6)
            net_put16(packet + 6, 0x4000);
        packet[8] = (UINT8)ttl;
        packet[9] = (UINT8)proto;
        mem_copy(packet + 12, src, 4);
        mem_copy(packet + 16, dst, 4);
        net_put16(packet + 10, k_net_checksum(packet, 20));
    } else {
        packet[0] = 0x60;
        net_put16(packet + 4, (UINT16)n);
        packet[6] = (UINT8)proto;
        packet[7] = (UINT8)ttl;
        mem_copy(packet + 8, src, 16);
        mem_copy(packet + 24, dst, 16);
    }
    mem_copy(packet + h, bytes, n);
    if (d->kind != NIC_LOOP && net_local(d, family, dst)) {
        UINT8 frame[1514];
        mem_copy(frame, mac, 6);
        mem_copy(frame + 6, d->info.mac, 6);
        net_put16(frame + 12, family == 4 ? 0x0800 : 0x86dd);
        mem_copy(frame + 14, packet, n + h);
        if (net_queued == NET_RXQ)
            return K_EAGAIN;
        net_enqueue(id, frame, n + h + 14);
        ++d->info.tx_packets;
        d->info.tx_bytes += n + h + 14;
        return 0;
    }
    return net_ether(d, mac, family == 4 ? 0x0800 : 0x86dd, packet, n + h);
}
static void net_arp_input(net_device *d, const UINT8 *p, UINT32 n, const UINT8 *mac)
{
    if (n < 28 || net_be16(p) != 1 || net_be16(p + 2) != 0x0800 || p[4] != 6 || p[5] != 4 ||
        (net_be16(p + 6) != 1 && net_be16(p + 6) != 2) || !net_equal(mac, p + 8, 6) ||
        (mac[0] & 1)) {
        ++d->info.errors;
        return;
    }
    if (net_equal(mac, d->info.mac, 6))
        return;
    if (!net_zero(d->info.config.address, 4) &&
        (net_equal(p + 14, d->info.config.address, 4) ||
         (d->dad4 && net_zero(p + 14, 4) && net_equal(p + 24, d->info.config.address, 4)))) {
        d->info.flags |= K_NET_CONFLICT;
        d->info.flags &= ~K_NET_READY4;
        d->dad4 = 0;
        return;
    }
    BOOLEAN ours = net_local(d, 4, p + 24);
    net_neighbor *e = net_neighbor_find(d->info.id, 4, p + 14, FALSE);
    if ((ours && net_be16(p + 6) == 1) ||
        (ours && net_be16(p + 6) == 2 && net_equal(p + 18, d->info.mac, 6) && e && e->probes))
        net_neighbor_learn(d->info.id, 4, p + 14, mac, TRUE);
    if (ours && net_be16(p + 6) == 1)
        (void)net_arp_send(d, 2, mac, d->info.config.address, p + 14);
}
static BOOLEAN net_icmp_allow(net_device *d)
{
    UINT64 now = k_uptime_ms();
    if (now - d->icmp_at >= 1000) {
        d->icmp_at = now;
        d->icmp_budget = 20;
    }
    if (!d->icmp_budget)
        return FALSE;
    --d->icmp_budget;
    return TRUE;
}
static void net_udp_input(net_device *, UINT32, const UINT8 *, const UINT8 *, const UINT8 *,
                          UINT32);
static void net_tcp_input(net_device *, UINT32, const UINT8 *, const UINT8 *, const UINT8 *,
                          UINT32);
static void net_nd_input(net_device *d, const UINT8 *src, const UINT8 *dst, const UINT8 *p,
                         UINT32 n, UINT32 hop, const UINT8 *mac)
{
    if (hop != 255 || n < 8 || p[1] || (mac[0] & 1) || net_zero(mac, 6))
        return;
    if (p[0] == 134) {
        if (!(d->info.config.flags & K_NET_AUTO6) || n < 16 || src[0] != 0xfe ||
            (src[1] & 0xc0) != 0x80)
            return;
        for (UINT32 at = 16; at < n;) {
            if (n - at < 2 || !p[at + 1] || p[at + 1] * 8u > n - at)
                return;
            UINT32 len = p[at + 1] * 8u, type = p[at];
            if ((type == 1 && (len != 8 || !net_equal(p + at + 2, mac, 6))) ||
                (type == 3 && len != 32) || (type == 5 && len != 8) ||
                (type == 25 && (len < 24 || ((len - 8) % 16))))
                return;
            at += len;
        }
        UINT64 now = k_uptime_ms();
        UINT32 lifetime = net_be16(p + 6);
        if (lifetime) {
            mem_copy(d->info.config.gateway6, src, 16);
            d->router_until = now + (UINT64)lifetime * 1000;
        } else if (net_equal(d->info.config.gateway6, src, 16)) {
            mem_zero(d->info.config.gateway6, 16);
            d->router_until = 0;
        }
        for (UINT32 at = 16; at < n;) {
            if (n - at < 2 || !p[at + 1] || p[at + 1] * 8u > n - at)
                return;
            UINT32 len = p[at + 1] * 8u;
            if (p[at] == 1 && len == 8 && net_equal(p + at + 2, mac, 6))
                net_neighbor_learn(d->info.id, 6, src, mac, FALSE);
            if (p[at] == 3 && len == 32 && p[at + 2] == 64 && (p[at + 3] & 0xc0) == 0xc0 &&
                p[at + 16] != 255 && !(p[at + 16] == 0xfe && (p[at + 17] & 0xc0) == 0x80)) {
                UINT32 valid = net_be32(p + at + 4), preferred = net_be32(p + at + 8);
                if (preferred <= valid && valid) {
                    UINT8 address[16];
                    mem_copy(address, p + at + 16, 8);
                    mem_copy(address + 8, d->info.link_local6 + 8, 8);
                    if (!net_equal(address, d->info.config.address6, 16)) {
                        mem_copy(d->info.config.address6, address, 16);
                        d->dadglobal = 3;
                        d->dad_at = now;
                    }
                    d->info.config.prefix6 = 64;
                    d->prefix_until = now + (UINT64)valid * 1000;
                }
            }
            if (p[at] == 25 && len >= 24 && ((len - 8) % 16) == 0) {
                UINT32 life = net_be32(p + at + 4);
                if (p[at + 8] != 255 && !net_zero(p + at + 8, 16)) {
                    if (life) {
                        mem_copy(d->info.config.dns6, p + at + 8, 16);
                        d->dns_until = now + (UINT64)life * 1000;
                    } else if (net_equal(d->info.config.dns6, p + at + 8, 16)) {
                        mem_zero(d->info.config.dns6, 16);
                        d->dns_until = 0;
                    }
                }
            }
            if (p[at] == 5 && len == 8) {
                UINT32 mtu = net_be32(p + at + 4);
                if (mtu >= 1280 && mtu <= (d->kind == NIC_EXTERNAL ? d->external.mtu : 1500u))
                    d->info.mtu = mtu;
            }
            at += len;
        }
        return;
    }
    if ((p[0] != 135 && p[0] != 136) || n < 24 || p[8] == 255)
        return;
    const UINT8 *target = p + 8;
    BOOLEAN unspecified = net_zero(src, 16), ours = net_local(d, 6, target);
    if (p[0] == 136 && unspecified)
        return;
    UINT8 learned[6];
    BOOLEAN have = FALSE;
    for (UINT32 at = 24; at < n;) {
        if (n - at < 2 || !p[at + 1] || p[at + 1] * 8u > n - at)
            return;
        UINT32 len = p[at + 1] * 8u;
        if ((p[at] == 1 || p[at] == 2) && len == 8) {
            if (!net_equal(p + at + 2, mac, 6))
                return;
            mem_copy(learned, p + at + 2, 6);
            have = TRUE;
        }
        at += len;
    }
    if (unspecified && p[0] == 135) {
        UINT8 expected[16] = {255, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 255, 0, 0, 0};
        mem_copy(expected + 13, target + 13, 3);
        if (!net_equal(dst, expected, 16))
            return;
    }
    if (unspecified && have)
        return;
    if (!net_equal(mac, d->info.mac, 6) &&
        ((d->dad6 && net_equal(target, d->info.link_local6, 16)) ||
         (d->dadglobal && net_equal(target, d->info.config.address6, 16)))) {
        d->info.flags |= K_NET_CONFLICT;
        d->info.flags &= ~K_NET_READY6;
        d->dad6 = d->dadglobal = 0;
        return;
    }
    if (p[0] == 136) {
        if ((p[4] & 0x40) && dst[0] == 255)
            return;
        net_neighbor *e = net_neighbor_find(d->info.id, 6, target, FALSE);
        if (have && e)
            net_neighbor_learn(d->info.id, 6, target, learned, (p[4] & 0x40) != 0);
        return;
    }
    if (!ours)
        return;
    if (have && !unspecified)
        net_neighbor_learn(d->info.id, 6, src, learned, FALSE);
    UINT8 response[32] = {0}, destination[16];
    response[0] = 136;
    response[4] = unspecified ? 0x20 : 0x60;
    mem_copy(response + 8, target, 16);
    response[24] = 2;
    response[25] = 1;
    mem_copy(response + 26, d->info.mac, 6);
    if (unspecified) {
        mem_zero(destination, 16);
        destination[0] = 255;
        destination[1] = 2;
        destination[15] = 1;
    } else
        mem_copy(destination, src, 16);
    net_put16(response + 2, net_transport_sum(6, target, destination, 58, response, 32));
    (void)net_ip_send(d->info.id, 6, target, destination, 58, response, 32, 255);
}
static void net_tcp_error(net_socket *, int);
static void net_icmp_failure(net_device *d, UINT32 family, const UINT8 *dst, const UINT8 *p,
                             UINT32 n)
{
    BOOLEAN too_big = family == 4 ? (p[0] == 3 && p[1] == 4) : (p[0] == 2 && p[1] == 0);
    BOOLEAN unreachable = family == 4 ? (p[0] == 3 && p[1] <= 15) : (p[0] == 1 && p[1] <= 7);
    BOOLEAN expired = family == 4 ? (p[0] == 11 && p[1] <= 1) : (p[0] == 3 && p[1] <= 1);
    if ((!too_big && !unreachable && !expired) || !net_local(d, family, dst))
        return;
    const UINT8 *q = p + 8, *source, *peer;
    UINT32 size = net_addr_bytes(family), h, proto, original;
    n -= 8;
    if (family == 4) {
        if (n < 28 || q[0] >> 4 != 4)
            return;
        h = (q[0] & 15) * 4u;
        if (h < 20 || h > n - 8 || k_net_checksum(q, h) || (net_be16(q + 6) & 8191))
            return;
        source = q + 12;
        peer = q + 16;
        proto = q[9];
        original = net_be16(q + 2);
        if (original < h + 8)
            return;
    } else {
        if (n < 48 || q[0] >> 4 != 6)
            return;
        h = 40;
        source = q + 8;
        peer = q + 24;
        proto = q[6];
        original = 40 + net_be16(q + 4);
        if (original < 48)
            return;
    }
    if (!net_equal(source, dst, size) || (proto != 6 && proto != 17))
        return;
    for (UINT32 i = 0; i < K_NET_MAX_SOCKETS; ++i) {
        net_socket *s = &net_sockets[i];
        if (!s->used || s->family != family || s->local.port != net_be16(q + h) ||
            s->peer.port != net_be16(q + h + 2) ||
            (s->local.interface != NET_NONE && s->local.interface != d->info.id) ||
            !net_equal(s->peer.bytes, peer, size) ||
            (!net_zero(s->local.bytes, size) && !net_equal(s->local.bytes, source, size)))
            continue;
        if (proto == 6 && s->type == K_SOCK_TCP) {
            UINT32 seq = net_be32(q + h + 4);
            if (s->snd_nxt == s->snd_una || seq - s->snd_una >= s->snd_nxt - s->snd_una)
                continue;
            if (too_big) {
                UINT32 mtu = family == 4 ? net_be16(p + 6) : net_be32(p + 4);
                if (!mtu && family == 4)
                    mtu = 576;
                if (mtu < (family == 4 ? 68u : 1280u) || mtu >= original)
                    continue;
                UINT32 mss = mtu - (family == 4 ? 40u : 60u);
                if (mss < s->mss) {
                    s->mss = mss;
                    s->sent_at = 0;
                    s->retransmitted = TRUE;
                }
            } else if (s->state == NS_SYN_SENT)
                net_tcp_error(s, expired ? K_ETIMEDOUT : K_ENETUNREACH);
        } else if (proto == 17 && s->type == K_SOCK_UDP) {
            s->error = too_big                                 ? K_EMSGSIZE
                       : expired                               ? K_ETIMEDOUT
                       : (family == 4 ? p[1] == 3 : p[1] == 4) ? K_ECONNREFUSED
                                                               : K_ENETUNREACH;
        }
        return;
    }
}
static void net_udp_unreachable(net_device *d, UINT32 family, const UINT8 *src, const UINT8 *dst,
                                const UINT8 *p, UINT32 n)
{
    if (!net_local(d, family, dst) || net_multicast(family, src) ||
        net_zero(src, net_addr_bytes(family)) || (family == 4 && net_broadcast4(d, src)) ||
        !net_icmp_allow(d))
        return;
    UINT8 message[1280] = {0};
    UINT32 h = family == 4 ? 20 : 40, take = family == 4 ? 8 : MIN(n, 1232u);
    UINT8 *q = message + 8;
    message[0] = family == 4 ? 3 : 1;
    message[1] = family == 4 ? 3 : 4;
    if (family == 4) {
        q[0] = 0x45;
        net_put16(q + 2, (UINT16)(20 + n));
        q[8] = 64;
        q[9] = 17;
        mem_copy(q + 12, src, 4);
        mem_copy(q + 16, dst, 4);
        net_put16(q + 10, k_net_checksum(q, 20));
    } else {
        q[0] = 0x60;
        net_put16(q + 4, (UINT16)n);
        q[6] = 17;
        q[7] = 64;
        mem_copy(q + 8, src, 16);
        mem_copy(q + 24, dst, 16);
    }
    mem_copy(q + h, p, take);
    UINT32 bytes = 8 + h + take;
    net_put16(message + 2, family == 4 ? k_net_checksum(message, bytes)
                                       : net_transport_sum(6, dst, src, 58, message, bytes));
    (void)net_ip_send(d->info.id, family, dst, src, family == 4 ? 1 : 58, message, bytes, 64);
}
static void net_icmp_input(net_device *d, UINT32 family, const UINT8 *src, const UINT8 *dst,
                           const UINT8 *p, UINT32 n, UINT32 hop, const UINT8 *mac)
{
    if (n < 8 || (family == 4 ? k_net_checksum(p, n) : net_transport_sum(6, src, dst, 58, p, n))) {
        ++d->info.errors;
        return;
    }
    if (family == 6 && p[0] >= 133 && p[0] <= 137) {
        net_nd_input(d, src, dst, p, n, hop, mac);
        return;
    }
    net_icmp_failure(d, family, dst, p, n);
    UINT32 request = family == 4 ? 8 : 128, reply = family == 4 ? 0 : 129;
    if (p[0] == request && !p[1] && net_local(d, family, dst) && n <= 1440 &&
        !net_multicast(family, src) && !net_zero(src, net_addr_bytes(family)) &&
        net_icmp_allow(d)) {
        UINT8 b[1440];
        mem_copy(b, p, n);
        b[0] = (UINT8)reply;
        b[2] = b[3] = 0;
        net_put16(b + 2,
                  family == 4 ? k_net_checksum(b, n) : net_transport_sum(6, dst, src, 58, b, n));
        (void)net_ip_send(d->info.id, family, dst, src, family == 4 ? 1 : 58, b, n, 64);
        return;
    }
    if (p[0] == reply && !p[1] && n == 24) {
        UINT32 token = net_be32(p + 4);
        for (UINT32 i = 0; i < 8; ++i)
            if (net_pings[i].used && !net_pings[i].done && net_pings[i].token == token &&
                net_pings[i].peer.family == family && net_pings[i].peer.interface == d->info.id &&
                net_equal(net_pings[i].peer.bytes, src, net_addr_bytes(family)) &&
                rd64(p + 8) == net_pings[i].sent && net_be32(p + 16) == (token ^ 0x91c573abu)) {
                net_pings[i].ms = (UINT32)(k_uptime_ms() - net_pings[i].sent);
                net_pings[i].done = TRUE;
            }
        return;
    }
}
static void net_ip_deliver(net_device *d, UINT32 family, const UINT8 *src, const UINT8 *dst,
                           UINT32 proto, const UINT8 *p, UINT32 n, UINT32 hop, const UINT8 *mac)
{
    if (proto == (family == 4 ? 1u : 58u))
        net_icmp_input(d, family, src, dst, p, n, hop, mac);
    else if (proto == 17)
        net_udp_input(d, family, src, dst, p, n);
    else if (proto == 6)
        net_tcp_input(d, family, src, dst, p, n);
    else
        ++d->info.dropped;
}
static void net_reassemble(net_device *d, UINT32 family, const UINT8 *src, const UINT8 *dst,
                           UINT32 proto, UINT32 id, UINT32 offset, BOOLEAN more, const UINT8 *p,
                           UINT32 n, UINT32 hop, const UINT8 *mac)
{
    if (!n || offset > NET_FRAG_BYTES || n > NET_FRAG_BYTES - offset || (more && (n & 7))) {
        ++d->info.errors;
        return;
    }
    UINT64 now = k_uptime_ms();
    net_fragment *f = NULL, *free = NULL;
    for (UINT32 i = 0; i < NET_FRAGS; ++i) {
        net_fragment *x = &net_fragments[i];
        if (x->used && x->expires <= now)
            x->used = FALSE;
        if (!x->used)
            free = x;
        else if (x->interface == d->info.id && x->family == family && x->id == id &&
                 x->protocol == proto && net_equal(x->src, src, net_addr_bytes(family)) &&
                 net_equal(x->dst, dst, net_addr_bytes(family)))
            f = x;
    }
    if (!f) {
        if (!free) {
            ++d->info.dropped;
            return;
        }
        f = free;
        mem_zero(f, sizeof(*f));
        f->used = TRUE;
        f->interface = d->info.id;
        f->family = family;
        f->id = id;
        f->protocol = proto;
        mem_copy(f->src, src, net_addr_bytes(family));
        mem_copy(f->dst, dst, net_addr_bytes(family));
        f->expires = now + 30000;
    }
    if ((f->final_size && offset + n > f->final_size) ||
        (!more && f->final_size && f->final_size != offset + n)) {
        f->used = FALSE;
        ++d->info.errors;
        return;
    }
    for (UINT32 i = offset; i < offset + n; ++i)
        if (f->seen[i / 8] & (1u << (i % 8))) {
            f->used = FALSE;
            ++d->info.errors;
            return;
        }
    if (!more) {
        for (UINT32 i = offset + n; i < NET_FRAG_BYTES; ++i)
            if (f->seen[i / 8] & (1u << (i % 8))) {
                f->used = FALSE;
                ++d->info.errors;
                return;
            }
        f->final_size = offset + n;
    }
    mem_copy(f->bytes + offset, p, n);
    for (UINT32 i = offset; i < offset + n; ++i)
        f->seen[i / 8] |= (UINT8)(1u << (i % 8));
    if (f->final_size) {
        for (UINT32 i = 0; i < f->final_size; ++i)
            if (!(f->seen[i / 8] & (1u << (i % 8))))
                return;
        net_ip_deliver(d, family, src, dst, proto, f->bytes, f->final_size, hop, mac);
        f->used = FALSE;
    }
}
static void net_receive_frame(const k_net_frame *f)
{
    net_device *d = &net_devices[f->interface];
    const UINT8 *p = f->bytes;
    UINT32 n = f->length;
    ++d->info.rx_packets;
    d->info.rx_bytes += n;
    if (d->capture_owner) {
        if (d->capture_count < 8)
            d->capture[(d->capture_head + d->capture_count++) % 8] = *f;
        else
            ++d->info.dropped;
    }
    if (!(d->info.flags & K_NET_UP) || n < 14 || (p[6] & 1) || net_zero(p + 6, 6)) {
        ++d->info.dropped;
        return;
    }
    if (!(p[0] & 1) && !net_equal(p, d->info.mac, 6)) {
        ++d->info.dropped;
        return;
    }
    UINT16 ether = net_be16(p + 12);
    const UINT8 *mac = p + 6;
    p += 14;
    n -= 14;
    if (ether == 0x0806) {
        net_arp_input(d, p, n, mac);
        return;
    }
    if (ether == 0x0800) {
        if (n < 20 || p[0] >> 4 != 4 || (p[0] & 15) < 5) {
            ++d->info.errors;
            return;
        }
        UINT32 h = (p[0] & 15) * 4, total = net_be16(p + 2), fragment = net_be16(p + 6);
        if (h > n || total < h || total > n || !p[8] || k_net_checksum(p, h) ||
            (fragment & 0x8000) || net_multicast(4, p + 12) || p[12] == 255) {
            ++d->info.errors;
            return;
        }
        if (d->info.id && p[12] == 127) {
            ++d->info.dropped;
            return;
        }
        if (!net_local(d, 4, p + 16) && !net_broadcast4(d, p + 16)) {
            ++d->info.dropped;
            return;
        }
        for (UINT32 at = 20; at < h;) {
            UINT8 opt = p[at];
            if (!opt)
                break;
            if (opt == 1) {
                ++at;
                continue;
            }
            if (h - at < 2 || p[at + 1] < 2 || p[at + 1] > h - at || opt == 131 || opt == 137) {
                ++d->info.errors;
                return;
            }
            at += p[at + 1];
        }
        if ((fragment & 0x4000) && (fragment & 0x3fff)) {
            ++d->info.errors;
            return;
        }
        if (fragment & 0x3fff)
            net_reassemble(d, 4, p + 12, p + 16, p[9], net_be16(p + 4), (fragment & 8191) * 8,
                           (fragment & 8192) != 0, p + h, total - h, p[8], mac);
        else
            net_ip_deliver(d, 4, p + 12, p + 16, p[9], p + h, total - h, p[8], mac);
        return;
    }
    if (ether == 0x86dd) {
        if (n < 40 || p[0] >> 4 != 6 || !p[7] || p[8] == 255 || net_be16(p + 4) > n - 40) {
            ++d->info.errors;
            return;
        }
        UINT32 remaining = net_be16(p + 4), at = 40, proto = p[6];
        if (!net_local(d, 6, p + 24) && p[24] != 255 &&
            !(d->dad6 && net_equal(p + 24, d->info.link_local6, 16)) &&
            !(d->dadglobal && net_equal(p + 24, d->info.config.address6, 16))) {
            ++d->info.dropped;
            return;
        }
        for (UINT32 ext = 0; ext < 8; ++ext) {
            if (proto == 44) {
                if (remaining < 8 || p[at + 1] || (net_be16(p + at + 2) & 6)) {
                    ++d->info.errors;
                    return;
                }
                UINT32 flags = net_be16(p + at + 2);
                if (p[at] == 44 || p[at] == 0 || p[at] == 43 || p[at] == 60 || p[at] == 58) {
                    ++d->info.dropped;
                    return;
                }
                if (flags & 0xfff9)
                    net_reassemble(d, 6, p + 8, p + 24, p[at], net_be32(p + at + 4), flags & 0xfff8,
                                   (flags & 1) != 0, p + at + 8, remaining - 8, p[7], mac);
                else
                    net_ip_deliver(d, 6, p + 8, p + 24, p[at], p + at + 8, remaining - 8, p[7],
                                   mac);
                return;
            }
            if (proto != 0 && proto != 60 && proto != 43) {
                net_ip_deliver(d, 6, p + 8, p + 24, proto, p + at, remaining, p[7], mac);
                return;
            }
            if (remaining < 8 || (proto == 0 && at != 40)) {
                ++d->info.errors;
                return;
            }
            UINT32 len = (p[at + 1] + 1u) * 8;
            if (len > remaining) {
                ++d->info.errors;
                return;
            }
            if (proto == 43) {
                if (p[at + 3]) {
                    ++d->info.dropped;
                    return;
                }
            } else
                for (UINT32 option = 2; option < len;) {
                    UINT8 type = p[at + option];
                    if (!type) {
                        ++option;
                        continue;
                    }
                    if (len - option < 2 || p[at + option + 1] > len - option - 2 ||
                        (type & 0xc0)) {
                        ++d->info.dropped;
                        return;
                    }
                    option += 2 + p[at + option + 1];
                }
            proto = p[at];
            at += len;
            remaining -= len;
        }
        ++d->info.dropped;
        return;
    }
    ++d->info.dropped;
}
static int net_udp_output(net_socket *s, const void *bytes, UINT32 n, const k_net_address *to)
{
    UINT32 id;
    UINT8 src[16];
    int e = net_route(to, s->local.interface, &id, src);
    if (e)
        return e;
    net_device *d = &net_devices[id];
    UINT32 max = d->info.mtu - (s->family == 4 ? 28 : 48);
    if (n > max || n > 1472)
        return K_EMSGSIZE;
    if (!net_zero(s->local.bytes, net_addr_bytes(s->family))) {
        if (!net_local(d, s->family, s->local.bytes))
            return K_ENETUNREACH;
        mem_copy(src, s->local.bytes, 16);
    }
    if (s->family == 4 && !(d->info.flags & K_NET_READY4)) {
        if (!net_privileged())
            return K_EPERM;
        mem_zero(src, 16);
    }
    UINT8 p[1480];
    net_put16(p, s->local.port);
    net_put16(p + 2, to->port);
    net_put16(p + 4, (UINT16)(n + 8));
    p[6] = p[7] = 0;
    mem_copy(p + 8, bytes, n);
    UINT16 sum = net_transport_sum(s->family, src, to->bytes, 17, p, n + 8);
    net_put16(p + 6, sum ? sum : 0xffff);
    return net_ip_send(id, s->family, src, to->bytes, 17, p, n + 8, 64);
}
static void net_udp_input(net_device *d, UINT32 family, const UINT8 *src, const UINT8 *dst,
                          const UINT8 *p, UINT32 n)
{
    if (n < 8 || !net_be16(p + 2) || net_be16(p + 4) < 8 || net_be16(p + 4) > n) {
        ++d->info.errors;
        return;
    }
    n = net_be16(p + 4);
    if ((family == 6 && !net_be16(p + 6)) ||
        (net_be16(p + 6) && net_transport_sum(family, src, dst, 17, p, n))) {
        ++d->info.errors;
        return;
    }
    if (n - 8 > 1472) {
        ++d->info.dropped;
        return;
    }
    UINT32 size = net_addr_bytes(family);
    for (UINT32 i = 0; i < K_NET_MAX_SOCKETS; ++i) {
        net_socket *s = &net_sockets[i];
        if (!s->used || s->detached || s->type != K_SOCK_UDP || s->family != family ||
            s->local.port != net_be16(p + 2))
            continue;
        if (s->local.interface != NET_NONE && s->local.interface != d->info.id)
            continue;
        if (!net_zero(s->local.bytes, size) && !net_equal(s->local.bytes, dst, size))
            continue;
        if (s->peer.port && (s->peer.port != net_be16(p) || !net_equal(s->peer.bytes, src, size)))
            continue;
        if (s->q_count == NET_DGRAMS) {
            ++d->info.dropped;
            return;
        }
        net_datagram *q = &s->datagrams[(s->q_head + s->q_count++) % NET_DGRAMS];
        mem_zero(&q->from, sizeof(q->from));
        q->from.family = family;
        q->from.interface = d->info.id;
        q->from.port = net_be16(p);
        mem_copy(q->from.bytes, src, size);
        q->length = n - 8;
        mem_copy(q->bytes, p + 8, n - 8);
        return;
    }
    net_udp_unreachable(d, family, src, dst, p, n);
    ++d->info.dropped;
}
static BOOLEAN net_seq_before(UINT32 a, UINT32 b) { return (INT32)(a - b) < 0; }
static int net_tcp_packet(net_socket *s, UINT32 sequence, UINT32 flags, const void *bytes, UINT32 n)
{
    UINT8 p[1500];
    UINT32 h = flags & TCP_SYN ? 24 : 20;
    if (s->local.interface >= net_devices_count)
        return K_ENETUNREACH;
    UINT32 limit = net_devices[s->local.interface].info.mtu - (s->family == 4 ? 20u : 40u) - h;
    if (n > limit)
        return K_EMSGSIZE;
    mem_zero(p, h);
    net_put16(p, s->local.port);
    net_put16(p + 2, s->peer.port);
    net_put32(p + 4, sequence);
    if (flags & TCP_ACK)
        net_put32(p + 8, s->rcv_nxt);
    p[12] = (UINT8)((h / 4) << 4);
    p[13] = (UINT8)flags;
    net_put16(p + 14, (UINT16)(NET_STREAM - s->rx_count));
    if (flags & TCP_SYN) {
        p[20] = 2;
        p[21] = 4;
        net_put16(p + 22, (UINT16)(s->family == 6 ? 1220 : NET_MSS));
    }
    if (n)
        mem_copy(p + h, bytes, n);
    net_put16(p + 16, net_transport_sum(s->family, s->local.bytes, s->peer.bytes, 6, p, h + n));
    return net_ip_send(s->local.interface, s->family, s->local.bytes, s->peer.bytes, 6, p, h + n,
                       64);
}
static void net_tcp_error(net_socket *s, int e)
{
    s->state = NS_ERROR;
    s->error = e;
    s->deadline = k_uptime_ms() + 30000;
    if (s->detached)
        s->used = FALSE;
}
static void net_tcp_ack(net_socket *s) { (void)net_tcp_packet(s, s->snd_nxt, TCP_ACK, NULL, 0); }
static int net_tcp_options(const UINT8 *p, UINT32 h, UINT32 *mss)
{
    for (UINT32 at = 20; at < h;) {
        if (!p[at])
            break;
        if (p[at] == 1) {
            ++at;
            continue;
        }
        if (h - at < 2 || p[at + 1] < 2 || p[at + 1] > h - at)
            return K_EINVAL;
        if (p[at] == 2) {
            if (p[at + 1] != 4 || net_be16(p + at + 2) < 64)
                return K_EINVAL;
            *mss = MIN(*mss, net_be16(p + at + 2));
        }
        at += p[at + 1];
    }
    return 0;
}
static void net_tcp_rtt(net_socket *s, UINT64 now)
{
    if (!s->retransmitted && s->sent_at) {
        UINT32 sample = (UINT32)MIN(now - s->sent_at, 60000);
        if (!sample)
            sample = 1;
        if (!s->srtt) {
            s->srtt = sample;
            s->rttvar = sample / 2;
        } else {
            UINT32 diff = s->srtt > sample ? s->srtt - sample : sample - s->srtt;
            s->rttvar = (3 * s->rttvar + diff) / 4;
            s->srtt = (7 * s->srtt + sample) / 8;
        }
        s->rto = MIN(60000u, s->srtt + 4 * s->rttvar);
        if (s->rto < 1000)
            s->rto = 1000;
    }
    s->retry = 0;
    s->retransmitted = FALSE;
    s->sent_at = now ? now : 1;
}
static void net_tcp_pump(net_socket *s, UINT64 now)
{
    if (!s->used || s->type != K_SOCK_TCP)
        return;
    if (s->state == NS_TIME_WAIT) {
        if (now >= s->deadline)
            s->used = FALSE;
        return;
    }
    if (s->state == NS_ERROR) {
        if (s->detached)
            s->used = FALSE;
        return;
    }
    if (s->state == NS_FIN_WAIT2 && now >= s->deadline) {
        net_tcp_error(s, K_ETIMEDOUT);
        return;
    }
    if (s->state >= NS_SYN_SENT && s->state <= NS_LAST_ACK &&
        (s->local.interface >= net_devices_count ||
         !net_local(&net_devices[s->local.interface], s->family, s->local.bytes))) {
        net_tcp_error(s, K_ENETDOWN);
        return;
    }
    if (s->local.interface < net_devices_count) {
        UINT32 limit = net_devices[s->local.interface].info.mtu - (s->family == 4 ? 40u : 60u);
        if (s->mss > limit)
            s->mss = limit;
    }
    BOOLEAN syn = s->state == NS_SYN_SENT || s->state == NS_SYN_RCVD;
    BOOLEAN fin = s->state == NS_FIN_WAIT1 || s->state == NS_CLOSING || s->state == NS_LAST_ACK;
    BOOLEAN flight = s->snd_nxt != s->snd_una;
    if (syn || fin || (flight && s->tx_count)) {
        if (!s->sent_at || now - s->sent_at >= s->rto) {
            if (s->retry >= 8) {
                net_tcp_error(s, K_ETIMEDOUT);
                return;
            }
            UINT32 flags = syn   ? (TCP_SYN | (s->state == NS_SYN_RCVD ? TCP_ACK : 0))
                           : fin ? (TCP_FIN | TCP_ACK)
                                 : TCP_ACK;
            UINT32 n = (!syn && !fin) ? MIN(s->mss, s->snd_nxt - s->snd_una) : 0;
            int e = net_tcp_packet(s,
                                   syn   ? s->iss
                                   : fin ? s->fin_seq
                                         : s->snd_una,
                                   flags, n ? s->tx : NULL, n);
            if (!e) {
                if (s->sent_at) {
                    ++s->retry;
                    s->retransmitted = TRUE;
                    s->threshold = MIN(s->cwnd / 2, NET_STREAM);
                    if (s->threshold < 2 * s->mss)
                        s->threshold = 2 * s->mss;
                    s->cwnd = s->mss;
                    s->rto = MIN(60000u, s->rto * 2);
                    ++net_devices[s->local.interface].info.tcp_retransmits;
                }
                s->sent_at = now ? now : 1;
            } else if (e != K_EAGAIN) {
                net_tcp_error(s, e);
                return;
            }
        }
        if (syn || fin)
            return;
    }
    if (s->state != NS_ESTABLISHED && s->state != NS_CLOSE_WAIT)
        return;
    UINT32 inflight = s->snd_nxt - s->snd_una, window = MIN(s->peer_window, s->cwnd);
    while (inflight < s->tx_count && inflight < window) {
        UINT32 n = MIN(s->mss, MIN(s->tx_count - inflight, window - inflight));
        int e = net_tcp_packet(s, s->snd_nxt, TCP_ACK | TCP_PSH, s->tx + inflight, n);
        if (e) {
            if (e != K_EAGAIN)
                net_tcp_error(s, e);
            return;
        }
        if (!inflight) {
            s->sent_at = now ? now : 1;
            s->retransmitted = FALSE;
        }
        s->snd_nxt += n;
        inflight += n;
    }
    if (!s->peer_window && s->tx_count && !inflight && now >= s->persist_at) {
        (void)net_tcp_packet(s, s->snd_una - 1, TCP_ACK, NULL, 0);
        s->persist_at = now + 1000;
    }
    if (s->close_requested && !s->tx_count) {
        s->fin_seq = s->snd_nxt++;
        s->state = s->peer_fin ? NS_LAST_ACK : NS_FIN_WAIT1;
        s->sent_at = 0;
        s->retry = 0;
        net_tcp_pump(s, now);
    }
}
static void net_tcp_input(net_device *d, UINT32 family, const UINT8 *src, const UINT8 *dst,
                          const UINT8 *p, UINT32 n)
{
    if (n < 20 || !net_local(d, family, dst) || net_multicast(family, src) ||
        net_zero(src, net_addr_bytes(family)) || !net_be16(p) || !net_be16(p + 2) ||
        net_transport_sum(family, src, dst, 6, p, n)) {
        ++d->info.errors;
        return;
    }
    UINT32 h = (p[12] >> 4) * 4;
    if (h < 20 || h > n || (p[12] & 15)) {
        ++d->info.errors;
        return;
    }
    UINT32 len = n - h, flags = p[13], seq = net_be32(p + 4), ack = net_be32(p + 8),
           size = net_addr_bytes(family);
    if (flags & 0x20) {
        ++d->info.dropped;
        return;
    } /* Urgent data is not exposed. */
    net_socket *s = NULL, *listener = NULL;
    for (UINT32 i = 0; i < K_NET_MAX_SOCKETS; ++i) {
        net_socket *x = &net_sockets[i];
        if (!x->used || x->type != K_SOCK_TCP || x->family != family ||
            x->local.port != net_be16(p + 2))
            continue;
        if (x->local.interface != NET_NONE && x->local.interface != d->info.id)
            continue;
        if (!net_zero(x->local.bytes, size) && !net_equal(x->local.bytes, dst, size))
            continue;
        if (x->state == NS_LISTEN && !x->detached) {
            listener = x;
            continue;
        }
        if (x->peer.port == net_be16(p) && net_equal(x->peer.bytes, src, size)) {
            s = x;
            break;
        }
    }
    UINT64 now = k_uptime_ms();
    if (!s && listener && (flags & (TCP_SYN | TCP_ACK | TCP_RST | TCP_FIN)) == TCP_SYN) {
        UINT32 waiting = 0;
        for (UINT32 i = 0; i < K_NET_MAX_SOCKETS; ++i)
            if (net_sockets[i].used && net_sockets[i].parent == listener->id)
                ++waiting;
        if (waiting >= listener->backlog) {
            ++d->info.dropped;
            return;
        }
        UINT32 mss = family == 6 ? 1220 : NET_MSS;
        if (net_tcp_options(p, h, &mss)) {
            ++d->info.errors;
            return;
        }
        s = net_socket_new(family, K_SOCK_TCP, listener->owner);
        if (!s) {
            ++d->info.dropped;
            return;
        }
        s->parent = listener->id;
        s->state = NS_SYN_RCVD;
        s->local.interface = s->peer.interface = d->info.id;
        s->local.port = net_be16(p + 2);
        s->peer.port = net_be16(p);
        mem_copy(s->local.bytes, dst, size);
        mem_copy(s->peer.bytes, src, size);
        s->iss = net_random();
        s->snd_una = s->iss;
        s->snd_nxt = s->iss + 1;
        s->rcv_nxt = seq + 1;
        s->mss = mss;
        s->cwnd = mss * 2;
        s->peer_window = net_be16(p + 14);
        net_tcp_pump(s, now);
        return;
    }
    if (!s) {
        if (!(flags & TCP_RST) && net_icmp_allow(d)) {
            static net_socket temp; /* Serialized by net_lock. */
            mem_zero(&temp, sizeof(temp));
            temp.family = family;
            temp.local.interface = d->info.id;
            temp.local.port = net_be16(p + 2);
            temp.peer.port = net_be16(p);
            mem_copy(temp.local.bytes, dst, size);
            mem_copy(temp.peer.bytes, src, size);
            temp.rcv_nxt = seq + len + ((flags & TCP_SYN) ? 1 : 0) + ((flags & TCP_FIN) ? 1 : 0);
            (void)net_tcp_packet(&temp, (flags & TCP_ACK) ? ack : 0,
                                 TCP_RST | ((flags & TCP_ACK) ? 0 : TCP_ACK), NULL, 0);
        }
        return;
    }
    s->activity = now;
    if (s->state == NS_SYN_SENT) {
        if (flags & TCP_RST) {
            if ((flags & TCP_ACK) && ack == s->snd_nxt)
                net_tcp_error(s, K_ECONNREFUSED);
            return;
        }
        if ((flags & (TCP_SYN | TCP_ACK)) != (TCP_SYN | TCP_ACK) || ack != s->snd_nxt)
            return;
        UINT32 mss = family == 6 ? 1220 : NET_MSS;
        if (net_tcp_options(p, h, &mss))
            return;
        s->mss = mss;
        s->cwnd = mss * 2;
        s->peer_window = net_be16(p + 14);
        s->snd_una = ack;
        s->rcv_nxt = seq + 1;
        s->state = NS_ESTABLISHED;
        net_tcp_rtt(s, now);
        s->sent_at = 0;
        net_tcp_ack(s);
        return;
    }
    if (s->state == NS_TIME_WAIT) {
        if (flags & TCP_FIN) {
            net_tcp_ack(s);
            s->deadline = now + 120000;
        }
        return;
    }
    if (s->state == NS_ERROR)
        return;
    if (s->state == NS_SYN_RCVD && (flags & TCP_SYN) && seq + 1 == s->rcv_nxt) {
        s->sent_at = 0;
        net_tcp_pump(s, now);
        return;
    }
    UINT32 window = NET_STREAM - s->rx_count,
           seglen = len + ((flags & TCP_FIN) ? 1 : 0) + ((flags & TCP_SYN) ? 1 : 0);
    BOOLEAN acceptable = window
                             ? (seglen ? (!net_seq_before(seq, s->rcv_nxt) &&
                                          net_seq_before(seq, s->rcv_nxt + window)) ||
                                             (!net_seq_before(seq + seglen - 1, s->rcv_nxt) &&
                                              net_seq_before(seq + seglen - 1, s->rcv_nxt + window))
                                       : !net_seq_before(seq, s->rcv_nxt) &&
                                             net_seq_before(seq, s->rcv_nxt + window))
                             : !seglen && seq == s->rcv_nxt;
    if (!acceptable) {
        if (!(flags & TCP_RST))
            net_tcp_ack(s);
        return;
    }
    if (flags & TCP_RST) {
        if (seq == s->rcv_nxt)
            net_tcp_error(s, K_ECONNRESET);
        else if (net_icmp_allow(d))
            net_tcp_ack(s);
        return;
    }
    if (flags & TCP_SYN) {
        if (net_icmp_allow(d))
            net_tcp_ack(s);
        return;
    }
    if (!(flags & TCP_ACK))
        return;
    if (net_seq_before(s->snd_nxt, ack)) {
        net_tcp_ack(s);
        return;
    }
    if (s->state == NS_SYN_RCVD) {
        if (ack != s->snd_nxt || seq != s->rcv_nxt)
            return;
        s->snd_una = ack;
        s->state = NS_ESTABLISHED;
        net_tcp_rtt(s, now);
        s->sent_at = 0;
    } else if (net_seq_before(s->snd_una, ack)) {
        UINT32 acknowledged = ack - s->snd_una;
        BOOLEAN finishing =
            s->state == NS_FIN_WAIT1 || s->state == NS_CLOSING || s->state == NS_LAST_ACK;
        if (finishing) {
            if (ack != s->fin_seq + 1)
                return;
            s->snd_una = ack;
            s->sent_at = 0;
            if (s->state == NS_LAST_ACK) {
                s->used = FALSE;
                return;
            }
            if (s->state == NS_CLOSING) {
                s->state = NS_TIME_WAIT;
                s->deadline = now + 120000;
            } else {
                s->state = NS_FIN_WAIT2;
                s->deadline = now + 60000;
            }
        } else {
            if (acknowledged > s->tx_count)
                return;
            for (UINT32 i = acknowledged; i < s->tx_count; ++i)
                s->tx[i - acknowledged] = s->tx[i];
            s->tx_count -= acknowledged;
            s->snd_una = ack;
            net_tcp_rtt(s, now);
            if (s->cwnd < s->threshold)
                s->cwnd += MIN(acknowledged, s->mss);
            else
                s->cwnd += NET_MAX(1u, s->mss * s->mss / s->cwnd);
            s->cwnd = MIN(s->cwnd, NET_STREAM);
            s->dupacks = 0;
            if (s->snd_nxt == s->snd_una)
                s->sent_at = 0;
        }
    } else if (ack == s->snd_una && s->tx_count && s->snd_nxt != s->snd_una && !len &&
               s->peer_window == net_be16(p + 14)) {
        if (++s->dupacks == 3) {
            s->threshold = NET_MAX(2 * s->mss, s->cwnd / 2);
            s->cwnd = s->mss;
            s->retransmitted = TRUE;
            s->sent_at = now;
            (void)net_tcp_packet(s, s->snd_una, TCP_ACK, s->tx,
                                 MIN(s->mss, s->snd_nxt - s->snd_una));
            ++d->info.tcp_retransmits;
        }
    }
    s->peer_window = net_be16(p + 14);
    if (s->peer_fin && len) {
        net_tcp_ack(s);
        return;
    }
    if (len || flags & TCP_FIN) {
        if (net_seq_before(seq, s->rcv_nxt)) {
            UINT32 trim = MIN(s->rcv_nxt - seq, len);
            p += trim;
            seq += trim;
            len -= trim;
        }
        if (seq != s->rcv_nxt) {
            net_tcp_ack(s);
            return;
        }
        UINT32 take = MIN(len, NET_STREAM - s->rx_count);
        for (UINT32 i = 0; i < take; ++i)
            s->rx[(s->rx_head + s->rx_count + i) % NET_STREAM] = p[h + i];
        s->rx_count += take;
        s->rcv_nxt += take;
        if ((flags & TCP_FIN) && take == len && !s->peer_fin) {
            s->peer_fin = TRUE;
            ++s->rcv_nxt;
            if (s->state == NS_ESTABLISHED)
                s->state = NS_CLOSE_WAIT;
            else if (s->state == NS_FIN_WAIT1)
                s->state = NS_CLOSING;
            else if (s->state == NS_FIN_WAIT2) {
                s->state = NS_TIME_WAIT;
                s->deadline = now + 120000;
            }
        }
        net_tcp_ack(s);
    }
    net_tcp_pump(s, now);
}
UINT32 k_net_count(void) { return __atomic_load_n(&net_devices_count, __ATOMIC_ACQUIRE); }
int k_net_get(UINT32 id, k_net_info *out)
{
    if (!out)
        return K_EINVAL;
    int e = net_lock_enter();
    if (e)
        return e;
    if (id >= net_devices_count)
        e = K_ENOENT;
    else {
        *out = net_devices[id].info;
        UINT64 now = k_uptime_ms(), until = net_devices[id].config_until;
        out->lease_remaining_ms = until > now ? until - now : 0;
    }
    k_mutex_unlock(&net_lock);
    return e;
}
int k_net_diagnose(UINT32 id, k_net_diag *out)
{
    if (!out)
        return K_EINVAL;
    int e = net_lock_enter();
    if (e)
        return e;
    if (id >= net_devices_count)
        e = K_ENOENT;
    else {
        net_device *d = &net_devices[id];
        if (d->kind != NIC_IGC && d->kind != NIC_E1000)
            e = K_ENOTSUP;
        else {
            if (!d->dead)
                nic_snapshot(d);
            *out = d->diag;
        }
    }
    k_mutex_unlock(&net_lock);
    return e;
}
int k_net_configure(const k_net_config *config)
{
    if (!config || !config->interface || config->interface >= k_net_count() ||
        (config->flags & ~3u) || config->prefix6 > 128 || config->lease_seconds > 604800)
        return K_EINVAL;
    if (!net_privileged())
        return K_EPERM;
    UINT32 ip = net_be32(config->address), mask = net_be32(config->mask),
           gateway = net_be32(config->gateway);
    if (ip) {
        UINT32 inverse = ~mask;
        if (!mask || (inverse & (inverse + 1)) || config->address[0] == 0 ||
            config->address[0] == 127 || config->address[0] >= 224 ||
            (inverse > 1 && ((ip & inverse) == 0 || (ip & inverse) == inverse)) ||
            (gateway && ((gateway & mask) != (ip & mask) || config->gateway[0] >= 224)))
            return K_EINVAL;
    } else if (mask || gateway)
        return K_EINVAL;
    if (config->address6[0] == 255 || config->gateway6[0] == 255 || config->dns6[0] == 255 ||
        (!net_zero(config->address6, 16) && !config->prefix6))
        return K_EINVAL;
    int e = net_lock_enter();
    if (e)
        return e;
    net_device *d = &net_devices[config->interface];
    if (d->dead) {
        e = d->info.error ? d->info.error : K_ENETDOWN;
        goto out;
    }
    BOOLEAN changed = !net_equal(config->address, d->info.config.address, 4) ||
                      !net_equal(config->address6, d->info.config.address6, 16) ||
                      ((config->flags ^ d->info.config.flags) & K_NET_UP) != 0;
    d->info.config = *config;
    d->info.flags = (d->info.flags & ~(K_NET_UP | K_NET_AUTO6)) | config->flags;
    UINT64 now = k_uptime_ms();
    d->config_until = config->lease_seconds ? now + (UINT64)config->lease_seconds * 1000 : 0;
    if (changed) {
        d->info.flags &= ~(K_NET_READY4 | K_NET_READY6 | K_NET_CONFLICT);
        d->info.mtu = d->kind == NIC_EXTERNAL ? d->external.mtu : 1500;
        d->dad4 = ip ? 3 : 0;
        d->dad6 = 3;
        d->dadglobal = net_zero(config->address6, 16) ? 0 : 3;
        d->dad_at = now;
        d->probe_at = now;
        d->auto_rs = 3;
        d->router_until = d->prefix_until = d->dns_until = 0;
        for (UINT32 i = 0; i < NET_NEIGHBORS; ++i)
            if (net_neighbors[i].interface == config->interface)
                net_neighbors[i].used = FALSE;
        for (UINT32 i = 0; i < K_NET_MAX_SOCKETS; ++i)
            if (net_sockets[i].used && net_sockets[i].type == K_SOCK_TCP &&
                net_sockets[i].local.interface == config->interface)
                net_tcp_error(&net_sockets[i], K_ENETDOWN);
    } else if ((config->flags & K_NET_AUTO6) && !d->auto_rs)
        d->auto_rs = 3;
out:
    k_mutex_unlock(&net_lock);
    return e;
}
int k_net_socket(UINT32 family, UINT32 type, UINT32 *handle)
{
    if (!handle || (family != 4 && family != 6) || (type != K_SOCK_UDP && type != K_SOCK_TCP))
        return K_EINVAL;
    int e = net_lock_enter();
    if (e)
        return e;
    net_socket *s = net_socket_new(family, type, k_task_id());
    if (!s)
        e = K_ENOSPC;
    else
        *handle = s->id;
    k_mutex_unlock(&net_lock);
    return e;
}
int k_net_bind(UINT32 handle, const k_net_address *a)
{
    if (!net_address_ok(a))
        return K_EINVAL;
    int e = net_lock_enter();
    if (e)
        return e;
    net_socket *s = net_socket_owned(handle);
    if (!s) {
        e = K_EPERM;
        goto out;
    }
    if (a->family != s->family || s->state != NS_NEW) {
        e = K_EINVAL;
        goto out;
    }
    if (a->port && a->port < 1024 && !net_privileged()) {
        e = K_EPERM;
        goto out;
    }
    if (!net_zero(a->bytes, net_addr_bytes(a->family))) {
        BOOLEAN found = FALSE;
        for (UINT32 i = 0; i < net_devices_count; ++i)
            if ((a->interface == NET_NONE || a->interface == i) &&
                net_local(&net_devices[i], a->family, a->bytes))
                found = TRUE;
        if (!found) {
            e = K_ENETUNREACH;
            goto out;
        }
    }
    if (a->port && net_port_busy(s, a)) {
        e = K_EADDRINUSE;
        goto out;
    }
    s->local = *a;
    e = net_autobind(s);
    if (!e)
        s->state = NS_BOUND;
out:
    k_mutex_unlock(&net_lock);
    return e;
}
int k_net_connect(UINT32 handle, const k_net_address *peer, UINT32 timeout)
{
    if (!net_address_ok(peer) || !peer->port ||
        net_zero(peer->bytes, net_addr_bytes(peer->family)) ||
        net_multicast(peer->family, peer->bytes) || timeout > 30000)
        return K_EINVAL;
    UINT64 start = k_uptime_ms();
    for (;;) {
        int e = net_lock_enter();
        if (e)
            return e;
        net_socket *s = net_socket_owned(handle);
        if (!s) {
            k_mutex_unlock(&net_lock);
            return K_EPERM;
        }
        if (s->family != peer->family) {
            k_mutex_unlock(&net_lock);
            return K_EINVAL;
        }
        if (s->state == NS_NEW || s->state == NS_BOUND) {
            UINT32 id;
            UINT8 source[16];
            e = net_route(peer, s->local.interface, &id, source);
            if (!e)
                e = net_autobind(s);
            if (!e) {
                if (!net_zero(s->local.bytes, net_addr_bytes(s->family)) &&
                    !net_equal(s->local.bytes, source, net_addr_bytes(s->family)))
                    e = K_ENETUNREACH;
            }
            if (!e) {
                s->local.interface = id;
                mem_copy(s->local.bytes, source, 16);
                s->peer = *peer;
                s->peer.interface = id;
                if (s->type == K_SOCK_UDP)
                    s->state = NS_ESTABLISHED;
                else {
                    s->iss = net_random();
                    s->snd_una = s->iss;
                    s->snd_nxt = s->iss + 1;
                    s->state = NS_SYN_SENT;
                    s->sent_at = 0;
                    net_tcp_pump(s, k_uptime_ms());
                }
            }
        } else if (s->peer.port != peer->port ||
                   !net_equal(s->peer.bytes, peer->bytes, net_addr_bytes(peer->family)))
            e = K_EINVAL;
        if (!e)
            e = s->state == NS_ESTABLISHED ? 0 : s->state == NS_ERROR ? s->error : K_EINPROGRESS;
        k_mutex_unlock(&net_lock);
        if (e != K_EINPROGRESS || !timeout)
            return e;
        if (net_canceled())
            return K_ECANCELED;
        if (k_uptime_ms() - start >= timeout)
            return K_ETIMEDOUT;
        k_sleep(1);
    }
}
int k_net_listen(UINT32 handle, UINT32 backlog)
{
    if (!backlog || backlog > 16)
        return K_EINVAL;
    int e = net_lock_enter();
    if (e)
        return e;
    net_socket *s = net_socket_owned(handle);
    if (!s)
        e = K_EPERM;
    else if (s->type != K_SOCK_TCP || (s->state != NS_NEW && s->state != NS_BOUND))
        e = K_EINVAL;
    else {
        e = net_autobind(s);
        if (!e) {
            s->state = NS_LISTEN;
            s->backlog = backlog;
        }
    }
    k_mutex_unlock(&net_lock);
    return e;
}
int k_net_accept(UINT32 handle, UINT32 *accepted, k_net_address *peer)
{
    if (!accepted)
        return K_EINVAL;
    int e = net_lock_enter();
    if (e)
        return e;
    net_socket *s = net_socket_owned(handle);
    if (!s)
        e = K_EPERM;
    else if (s->state != NS_LISTEN)
        e = K_EINVAL;
    else {
        e = K_EAGAIN;
        for (UINT32 i = 0; i < K_NET_MAX_SOCKETS; ++i) {
            net_socket *child = &net_sockets[i];
            if (!child->used || child->parent != s->id)
                continue;
            if (child->state == NS_ERROR) {
                child->used = FALSE;
                continue;
            }
            if (child->state != NS_ESTABLISHED && child->state != NS_CLOSE_WAIT)
                continue;
            child->parent = 0;
            *accepted = child->id;
            if (peer)
                *peer = child->peer;
            e = 0;
            break;
        }
    }
    k_mutex_unlock(&net_lock);
    return e;
}
int k_net_send(UINT32 handle, const void *bytes, UINT32 n, const k_net_address *to)
{
    if ((!bytes && n) || n > 4096 || (to && !net_address_ok(to)))
        return K_EINVAL;
    int e = net_lock_enter();
    if (e)
        return e;
    net_socket *s = net_socket_owned(handle);
    if (!s) {
        e = K_EPERM;
        goto out;
    }
    if (s->state == NS_ERROR) {
        e = s->error;
        goto out;
    }
    if (s->type == K_SOCK_UDP) {
        const k_net_address *destination = to ? to : &s->peer;
        if (destination->family != s->family || !destination->port) {
            e = K_EINVAL;
            goto out;
        }
        e = net_autobind(s);
        if (!e) {
            if (s->state == NS_NEW)
                s->state = NS_BOUND;
            e = net_udp_output(s, bytes, n, destination);
        }
        if (!e)
            e = (int)n;
    } else {
        if (to || (s->state != NS_ESTABLISHED && s->state != NS_CLOSE_WAIT) || s->close_requested) {
            e = K_EINVAL;
            goto out;
        }
        UINT32 take = MIN(n, NET_STREAM - s->tx_count);
        if (!take && n) {
            e = K_EAGAIN;
            goto out;
        }
        mem_copy(s->tx + s->tx_count, bytes, take);
        s->tx_count += take;
        net_tcp_pump(s, k_uptime_ms());
        e = (int)take;
    }
out:
    k_mutex_unlock(&net_lock);
    return e;
}
int k_net_receive(UINT32 handle, void *bytes, UINT32 n, k_net_address *from)
{
    if ((!bytes && n) || n > 65536)
        return K_EINVAL;
    int e = net_lock_enter();
    if (e)
        return e;
    net_socket *s = net_socket_owned(handle);
    if (!s) {
        e = K_EPERM;
        goto out;
    }
    if (s->type == K_SOCK_UDP) {
        if (!s->q_count) {
            e = s->error ? s->error : K_EAGAIN;
            s->error = 0;
            goto out;
        }
        net_datagram *q = &s->datagrams[s->q_head];
        if (n < q->length) {
            e = K_EMSGSIZE;
            goto out;
        }
        mem_copy(bytes, q->bytes, q->length);
        if (from)
            *from = q->from;
        e = (int)q->length;
        s->q_head = (s->q_head + 1) % NET_DGRAMS;
        --s->q_count;
    } else {
        if (!n) {
            e = 0;
            goto out;
        }
        if (s->rx_count) {
            UINT32 take = MIN(n, s->rx_count);
            UINT8 *b = bytes;
            for (UINT32 i = 0; i < take; ++i)
                b[i] = s->rx[(s->rx_head + i) % NET_STREAM];
            s->rx_head = (s->rx_head + take) % NET_STREAM;
            s->rx_count -= take;
            if (from)
                *from = s->peer;
            e = (int)take;
            net_tcp_ack(s);
        } else
            e = s->peer_fin ? 0 : s->state == NS_ERROR ? s->error : K_EAGAIN;
    }
out:
    k_mutex_unlock(&net_lock);
    return e;
}
static void net_close_internal(net_socket *s)
{
    if (s->type == K_SOCK_UDP || s->state == NS_NEW || s->state == NS_BOUND ||
        s->state == NS_ERROR) {
        s->used = FALSE;
        return;
    }
    if (s->state == NS_LISTEN) {
        for (UINT32 i = 0; i < K_NET_MAX_SOCKETS; ++i)
            if (net_sockets[i].used && net_sockets[i].parent == s->id) {
                (void)net_tcp_packet(&net_sockets[i], net_sockets[i].snd_nxt, TCP_RST | TCP_ACK,
                                     NULL, 0);
                net_sockets[i].used = FALSE;
            }
        s->used = FALSE;
        return;
    }
    if (s->state == NS_SYN_SENT || s->state == NS_SYN_RCVD) {
        (void)net_tcp_packet(s, s->snd_nxt, TCP_RST | TCP_ACK, NULL, 0);
        s->used = FALSE;
        return;
    }
    s->detached = TRUE;
    s->close_requested = TRUE;
    net_tcp_pump(s, k_uptime_ms());
}
int k_net_close(UINT32 h)
{
    int e = net_lock_enter();
    if (e)
        return e;
    net_socket *s = net_socket_owned(h);
    if (!s)
        e = K_EPERM;
    else
        net_close_internal(s);
    k_mutex_unlock(&net_lock);
    return e;
}
int k_net_socket_get(UINT32 h, k_socket_info *out)
{
    if (!out)
        return K_EINVAL;
    int e = net_lock_enter();
    if (e)
        return e;
    net_socket *s = net_socket_owned(h);
    if (!s)
        e = K_EPERM;
    else {
        mem_zero(out, sizeof(*out));
        out->handle = s->id;
        out->type = s->type;
        out->state = s->state;
        out->owner_task = s->owner;
        out->local = s->local;
        out->peer = s->peer;
        out->queued_rx = s->type == K_SOCK_UDP ? s->q_count : s->rx_count;
        out->queued_tx = s->tx_count;
        out->error = s->error;
    }
    k_mutex_unlock(&net_lock);
    return e;
}
int k_net_raw_send(UINT32 id, const void *bytes, UINT32 n)
{
    if (!net_privileged())
        return K_EPERM;
    if (!bytes || n < 14 || n > 1514)
        return K_EINVAL;
    int e = net_lock_enter();
    if (e)
        return e;
    if (id >= net_devices_count)
        e = K_ENOENT;
    else if (!net_equal((const UINT8 *)bytes + 6, net_devices[id].info.mac, 6))
        e = K_EINVAL;
    else
        e = nic_transmit(&net_devices[id], bytes, n);
    k_mutex_unlock(&net_lock);
    return e;
}
int k_net_raw_receive(UINT32 id, k_net_frame *out)
{
    if (!net_privileged())
        return K_EPERM;
    int e = net_lock_enter();
    if (e)
        return e;
    if (id >= net_devices_count) {
        e = K_ENOENT;
        goto done;
    }
    net_device *d = &net_devices[id];
    if (d->capture_owner && d->capture_owner != k_task_id()) {
        e = K_EBUSY;
        goto done;
    }
    if (!out) {
        d->capture_owner = 0;
        d->capture_count = 0;
        e = 0;
        goto done;
    }
    d->capture_owner = k_task_id();
    if (!d->capture_count)
        e = K_EAGAIN;
    else {
        *out = d->capture[d->capture_head];
        d->capture_head = (d->capture_head + 1) % 8;
        --d->capture_count;
    }
done:
    k_mutex_unlock(&net_lock);
    return e;
}
int k_net_ping(const k_net_address *to, UINT32 timeout, UINT32 *milliseconds)
{
    if (!net_address_ok(to) || !milliseconds || !timeout || timeout > 10000 ||
        net_multicast(to->family, to->bytes))
        return K_EINVAL;
    int e = net_lock_enter();
    if (e)
        return e;
    UINT32 slot = 8;
    for (UINT32 i = 0; i < 8; ++i)
        if (!net_pings[i].used) {
            slot = i;
            break;
        }
    if (slot == 8) {
        k_mutex_unlock(&net_lock);
        return K_ENOSPC;
    }
    UINT32 id;
    UINT8 src[16];
    e = net_route(to, NET_NONE, &id, src);
    if (e) {
        k_mutex_unlock(&net_lock);
        return e;
    }
    UINT64 start = k_uptime_ms();
    net_pings[slot].used = TRUE;
    net_pings[slot].done = FALSE;
    net_pings[slot].token = net_random();
    net_pings[slot].owner = k_task_id();
    net_pings[slot].sent = start;
    net_pings[slot].peer = *to;
    net_pings[slot].peer.interface = id;
    BOOLEAN sent = FALSE;
    k_mutex_unlock(&net_lock);
    for (;;) {
        e = net_lock_enter();
        if (e)
            return e;
        if (!sent) {
            UINT8 p[24] = {0};
            p[0] = (UINT8)(to->family == 4 ? 8 : 128);
            net_put32(p + 4, net_pings[slot].token);
            wr64(p + 8, start);
            net_put32(p + 16, net_pings[slot].token ^ 0x91c573abu);
            net_put16(p + 2, to->family == 4 ? k_net_checksum(p, 24)
                                             : net_transport_sum(6, src, to->bytes, 58, p, 24));
            e = net_ip_send(id, to->family, src, to->bytes, to->family == 4 ? 1 : 58, p, 24, 64);
            if (!e)
                sent = TRUE;
        }
        if (!e || e == K_EAGAIN)
            e = net_pings[slot].done ? 0 : K_EAGAIN;
        if (e == K_EAGAIN && net_canceled())
            e = K_ECANCELED;
        if (e == K_EAGAIN && k_uptime_ms() - start >= timeout)
            e = K_ETIMEDOUT;
        if (e != K_EAGAIN) {
            if (!e)
                *milliseconds = net_pings[slot].ms;
            net_pings[slot].used = FALSE;
            k_mutex_unlock(&net_lock);
            return e;
        }
        k_mutex_unlock(&net_lock);
        k_sleep(1);
    }
}
int k_net_attach(const k_net_driver *driver, void *context, UINT32 *interface)
{
    if ((current_task() && current_task()->process) || k_cpu_id() != 0)
        return K_EPERM;
    if (!driver || !interface || !driver->receive || !driver->transmit || !driver->link ||
        !driver->stop || driver->mtu < 1280 || driver->mtu > 1500 || (driver->mac[0] & 1) ||
        net_zero(driver->mac, 6) || smp_started)
        return K_EINVAL;
    int e = net_lock_enter();
    if (e)
        return e;
    if (net_devices_count == K_NET_MAX_IF) {
        e = K_ENOSPC;
        goto done;
    }
    net_device *d = &net_devices[net_devices_count];
    mem_zero(d, sizeof(*d));
    d->kind = NIC_EXTERNAL;
    d->external = *driver;
    d->context = context;
    d->info.id = net_devices_count;
    d->info.mtu = driver->mtu;
    d->info.config.interface = d->info.id;
    mem_copy(d->info.mac, driver->mac, 6);
    mem_copy(d->info.driver, driver->name, 15);
    decimal_name(d->info.name, "link", d->info.id);
    d->info.link_local6[0] = 0xfe;
    d->info.link_local6[1] = 0x80;
    d->info.link_local6[8] = driver->mac[0] ^ 2;
    d->info.link_local6[9] = driver->mac[1];
    d->info.link_local6[10] = driver->mac[2];
    d->info.link_local6[11] = 255;
    d->info.link_local6[12] = 254;
    mem_copy(d->info.link_local6 + 13, driver->mac + 3, 3);
    *interface = d->info.id;
    __atomic_add_fetch(&net_devices_count, 1, __ATOMIC_RELEASE);
done:
    k_mutex_unlock(&net_lock);
    return e;
}
int k_wifi_get(UINT32 id, k_wifi_info *out)
{
    if (!out)
        return K_EINVAL;
    int e = net_lock_enter();
    if (e)
        return e;
    if (id >= net_wifi_count)
        e = K_ENOENT;
    else
        *out = net_wifi[id].info;
    k_mutex_unlock(&net_lock);
    return e;
}
int k_wifi_register(UINT32 id, const k_wifi_ops *ops, void *context)
{
    if (current_task() && current_task()->process)
        return K_EPERM;
    if (!ops || !ops->control || !ops->scan_result || ops->interface >= net_devices_count ||
        net_devices[ops->interface].kind != NIC_EXTERNAL || smp_started)
        return K_EINVAL;
    int e = net_lock_enter();
    if (e)
        return e;
    if (id >= net_wifi_count)
        e = K_ENOENT;
    else if (net_wifi[id].ops.control)
        e = K_EEXIST;
    else {
        net_wifi[id].ops = *ops;
        net_wifi[id].context = context;
        net_wifi[id].info.interface = ops->interface;
        net_wifi[id].info.error = 0;
        net_wifi[id].info.capabilities = 1;
        str_copy(net_wifi[id].info.driver, "registered", 24);
    }
    k_mutex_unlock(&net_lock);
    return e;
}
int k_wifi_control(UINT32 id, const k_wifi_request *request)
{
    if (!net_privileged())
        return K_EPERM;
    if (!request || request->operation < 1 || request->operation > 3 || request->channel > 196 ||
        request->ssid_length > 32 || request->security > K_WIFI_RSN ||
        !net_zero(request->reserved, 2))
        return K_EINVAL;
    if (request->operation == K_WIFI_JOIN && (!request->ssid_length || (request->bssid[0] & 1)))
        return K_EINVAL;
    int e = net_lock_enter();
    if (e)
        return e;
    if (id >= net_wifi_count)
        e = K_ENOENT;
    else if (!net_wifi[id].ops.control)
        e = K_ENOTSUP;
    else
        e = net_wifi[id].ops.control(net_wifi[id].context, request);
    k_mutex_unlock(&net_lock);
    return e;
}
int k_wifi_scan_result(UINT32 id, UINT32 index, k_wifi_bss *out)
{
    if (!out || index >= K_WIFI_BSS_MAX)
        return K_EINVAL;
    int e = net_lock_enter();
    if (e)
        return e;
    if (id >= net_wifi_count)
        e = K_ENOENT;
    else if (!net_wifi[id].ops.scan_result)
        e = K_ENOTSUP;
    else
        e = net_wifi[id].ops.scan_result(net_wifi[id].context, index, out);
    k_mutex_unlock(&net_lock);
    return e;
}
int k_wifi_decode_beacon(const void *bytes, UINT32 n, k_wifi_bss *out)
{
    if (!bytes || !out || n < 36 || n > 4096)
        return K_EINVAL;
    const UINT8 *p = bytes;
    if ((p[0] != 0x80 && p[0] != 0x50) || (p[1] & 0x43) || (p[16] & 1) || net_zero(p + 16, 6))
        return K_EINVAL;
    k_wifi_bss b = {0};
    mem_copy(b.bssid, p + 16, 6);
    b.security = (rd16(p + 34) & 16) ? K_WIFI_PRIVACY : K_WIFI_OPEN;
    BOOLEAN ssid = FALSE;
    for (UINT32 at = 36; at < n;) {
        if (n - at < 2 || p[at + 1] > n - at - 2)
            return K_EINVAL;
        UINT32 len = p[at + 1];
        if (p[at] == 0) {
            if (ssid || len > 32)
                return K_EINVAL;
            ssid = TRUE;
            b.ssid_length = (UINT8)len;
            mem_copy(b.ssid, p + at + 2, len);
        }
        if (p[at] == 3) {
            if (len != 1 || !p[at + 2] || p[at + 2] > 196)
                return K_EINVAL;
            b.channel = p[at + 2];
        }
        if (p[at] == 48) {
            if (len < 2 || rd16(p + at + 2) != 1)
                return K_EINVAL;
            b.security = K_WIFI_RSN;
        }
        at += 2 + len;
    }
    if (!ssid)
        return K_EINVAL;
    *out = b;
    return 0;
}
static void net_poll_protocols(UINT64 now)
{
    for (UINT32 i = 1; i < net_devices_count; ++i) {
        net_device *d = &net_devices[i];
        if (d->config_until && now >= d->config_until) {
            d->config_until = 0;
            d->info.flags &= ~K_NET_READY4;
            mem_zero(d->info.config.address, 16);
            d->dad4 = 0;
        }
        if (d->router_until && now >= d->router_until) {
            mem_zero(d->info.config.gateway6, 16);
            d->router_until = 0;
        }
        if (d->dns_until && now >= d->dns_until) {
            mem_zero(d->info.config.dns6, 16);
            d->dns_until = 0;
        }
        if (d->prefix_until && now >= d->prefix_until) {
            mem_zero(d->info.config.address6, 16);
            d->prefix_until = 0;
            d->dadglobal = 0;
        }
        if (d->dead || (d->info.flags & (K_NET_UP | K_NET_LINK)) != (K_NET_UP | K_NET_LINK))
            continue;
        if (!(d->info.flags & K_NET_CONFLICT) && now >= d->dad_at) {
            d->dad_at = now + 1000;
            if (d->dad4) {
                if (d->dad4 > 1) {
                    UINT8 b[6] = {255, 255, 255, 255, 255, 255}, zero[4] = {0};
                    if (!net_arp_send(d, 1, b, zero, d->info.config.address))
                        --d->dad4;
                } else {
                    d->dad4 = 0;
                    d->info.flags |= K_NET_READY4;
                    UINT8 b[6] = {255, 255, 255, 255, 255, 255};
                    (void)net_arp_send(d, 1, b, d->info.config.address, d->info.config.address);
                }
            }
            if (d->dad6) {
                if (d->dad6 > 1) {
                    if (!net_nd_solicit(d, d->info.link_local6, TRUE))
                        --d->dad6;
                } else {
                    d->dad6 = 0;
                    d->info.flags |= K_NET_READY6;
                }
            }
            if (d->dadglobal) {
                if (d->dadglobal > 1) {
                    if (!net_nd_solicit(d, d->info.config.address6, TRUE))
                        --d->dadglobal;
                } else
                    d->dadglobal = 0;
            }
        }
        if (d->auto_rs && (d->info.config.flags & K_NET_AUTO6) && (d->info.flags & K_NET_READY6) &&
            now >= d->probe_at) {
            UINT8 dst[16] = {255, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2},
                  p[16] = {133, 0, 0, 0, 0, 0, 0, 0, 1, 1};
            mem_copy(p + 10, d->info.mac, 6);
            net_put16(p + 2, net_transport_sum(6, d->info.link_local6, dst, 58, p, 16));
            if (!net_ip_send(i, 6, d->info.link_local6, dst, 58, p, 16, 255))
                --d->auto_rs;
            d->probe_at = now + 4000;
        }
    }
    BOOLEAN gc = now >= net_gc_at;
    if (gc)
        net_gc_at = now + 1000;
    for (UINT32 i = 0; i < K_NET_MAX_SOCKETS; ++i) {
        net_socket *s = &net_sockets[i];
        if (!s->used)
            continue;
        if (gc && !s->detached) {
            k_task_info t;
            if (k_task_query(s->owner, &t) || t.state == K_TASK_ZOMBIE) {
                net_close_internal(s);
                continue;
            }
        }
        if (s->parent && s->state == NS_ERROR) {
            s->used = FALSE;
            continue;
        }
        net_tcp_pump(s, now);
    }
    if (gc)
        for (UINT32 i = 0; i < net_devices_count; ++i) {
            net_device *d = &net_devices[i];
            if (d->capture_owner) {
                k_task_info t;
                if (k_task_query(d->capture_owner, &t) || t.state == K_TASK_ZOMBIE) {
                    d->capture_owner = d->capture_count = 0;
                }
            }
        }
}
static void net_worker(void *unused)
{
    (void)unused;
    for (;;) {
        if (!k_mutex_lock(&net_lock)) {
            for (UINT32 i = 1; i < net_devices_count; ++i)
                nic_poll(&net_devices[i]);
            for (UINT32 budget = 0; budget < 64 && net_queued; ++budget) {
                k_net_frame frame = net_queue[net_head];
                net_head = (net_head + 1) % NET_RXQ;
                --net_queued;
                net_receive_frame(&frame);
            }
            net_poll_protocols(k_uptime_ms());
            k_mutex_unlock(&net_lock);
        }
        k_sleep(1);
    }
}
int k_net_init(void)
{
    if (net_ready || k_cpu_id() != 0 || smp_started)
        return K_EBUSY;
    net_key[0] = arch_cycle_count() ^ (UINT64)(UINTN)net_sockets;
    net_key[1] = net_rot(arch_cycle_count(), 23) ^ (UINT64)(UINTN)net_devices;
    for (UINT32 i = 0; i < 10; ++i) {
        UINT64 random;
        if (arch_random64(&random))
            net_key[i & 1] ^= random;
    }

    net_ephemeral = 49152 + (net_random() & 16383);
    net_device *lo = &net_devices[0];
    lo->kind = NIC_LOOP;
    lo->info.id = 0;
    lo->info.mtu = 1500;
    lo->info.flags = K_NET_UP | K_NET_LINK | K_NET_READY4 | K_NET_READY6;
    str_copy(lo->info.name, "lo", 16);
    str_copy(lo->info.driver, "loopback", 16);
    lo->info.mac[0] = 2;
    lo->info.mac[5] = 1;
    lo->info.config.flags = K_NET_UP;
    lo->info.config.address[0] = 127;
    lo->info.config.address[3] = 1;
    lo->info.config.mask[0] = 255;
    lo->info.link_local6[15] = 1;
    lo->info.config.address6[15] = 1;
    lo->info.config.prefix6 = 128;
    net_devices_count = 1;
    for (UINT32 bus = 0; bus < 256; ++bus)
        for (UINT32 slot = 0; slot < 32; ++slot) {
            UINT16 first = (UINT16)((bus << 8) | (slot << 3));
            if ((pci_get(first, 0) & 65535) == 65535)
                continue;
            UINT32 functions = (pci_get(first, 0x0c) & 0x00800000) ? 8 : 1;
            for (UINT32 function = 0; function < functions; ++function) {
                UINT16 bdf = (UINT16)(first | function);
                UINT32 identity = pci_get(bdf, 0);
                if ((identity & 65535) == 65535)
                    continue;
                UINT32 cls = pci_get(bdf, 8) >> 16;
                UINT16 vendor = (UINT16)identity, device = (UINT16)(identity >> 16);
                if (cls == 0x0280 && net_wifi_count < K_WIFI_MAX) {
                    UINT32 wi = net_wifi_count++;
                    net_wifi[wi].info.id = wi;
                    net_wifi[wi].info.interface = NET_NONE;
                    net_wifi[wi].info.vendor = vendor;
                    net_wifi[wi].info.device = device;
                    net_wifi[wi].info.pci_bdf = bdf;
                    net_wifi[wi].info.error = K_ENOTSUP;
                    str_copy(net_wifi[wi].info.driver, "driver/firmware needed", 24);
                    continue;
                }
                if (cls != 0x0200 || net_devices_count == K_NET_MAX_IF)
                    continue;
                net_device *nd = &net_devices[net_devices_count];
                nd->info.id = net_devices_count;
                nd->info.vendor = vendor;
                nd->info.device = device;
                nd->info.pci_bdf = bdf;
                nd->bdf = bdf;
                nd->info.mtu = 1500;
                nd->info.config.interface = net_devices_count;
                decimal_name(nd->info.name, "eth", net_devices_count - 1);
                int result = nic_start(nd);
                if (result) {
                    if (nd->kind != NIC_LOOP)
                        nic_fail(nd, result);
                    else {
                        nd->dead = TRUE;
                        nd->info.error = result;
                    }
                } else {
                    nd->info.link_local6[0] = 0xfe;
                    nd->info.link_local6[1] = 0x80;
                    nd->info.link_local6[8] = nd->info.mac[0] ^ 2;
                    nd->info.link_local6[9] = nd->info.mac[1];
                    nd->info.link_local6[10] = nd->info.mac[2];
                    nd->info.link_local6[11] = 0xff;
                    nd->info.link_local6[12] = 0xfe;
                    mem_copy(nd->info.link_local6 + 13, nd->info.mac + 3, 3);
                    nic_poll(nd);
                }
                ++net_devices_count;
            }
        }
    net_ready = TRUE;
    UINT32 tid;
    int result = k_task_create("network", net_worker, NULL, 384, 0, &tid);
    if (result) {
        net_ready = FALSE;
        for (UINT32 i = 1; i < net_devices_count; ++i)
            if (!net_devices[i].dead)
                nic_fail(&net_devices[i], result);
    }
    return result;
}
INT64 net_syscall(k_process *p, const k_syscall_request *f)
{
    UINT32 op = (UINT32)f->number;
    if (f->args[0] > 0xffffffffu && op != K_SYS_NETCONFIG && op != K_SYS_NETPING)
        return K_EINVAL;
    switch (op) {
    case K_SYS_NETINFO: {
        k_net_info info;
        int e = k_as_check(p->as, f->args[1], sizeof(info), TRUE);
        if (!e)
            e = k_net_get((UINT32)f->args[0], &info);
        return e ? e : k_copy_to_user(p->as, f->args[1], &info, sizeof(info));
    }
    case K_SYS_NETCONFIG: {
        k_net_config c;
        int e = k_copy_from_user(p->as, &c, f->args[0], sizeof(c));
        return e ? e : k_net_configure(&c);
    }
    case K_SYS_SOCKET: {
        if (f->args[1] > 0xffffffffu)
            return K_EINVAL;
        UINT32 h;
        int e = k_net_socket((UINT32)f->args[0], (UINT32)f->args[1], &h);
        return e ? e : (INT64)h;
    }
    case K_SYS_BIND:
    case K_SYS_CONNECT: {
        k_net_address a;
        int e = k_copy_from_user(p->as, &a, f->args[1], sizeof(a));
        if (e)
            return e;
        if (op == K_SYS_CONNECT) {
            if (f->args[2] > 30000)
                return K_EINVAL;
            return k_net_connect((UINT32)f->args[0], &a, (UINT32)f->args[2]);
        }
        return k_net_bind((UINT32)f->args[0], &a);
    }
    case K_SYS_LISTEN:
        if (f->args[1] > 16)
            return K_EINVAL;
        return k_net_listen((UINT32)f->args[0], (UINT32)f->args[1]);
    case K_SYS_ACCEPT: {
        k_net_address a;
        UINT32 h;
        int e = f->args[1] ? k_as_check(p->as, f->args[1], sizeof(a), TRUE) : 0;
        if (!e)
            e = k_net_accept((UINT32)f->args[0], &h, &a);
        if (!e && f->args[1])
            e = k_copy_to_user(p->as, f->args[1], &a, sizeof(a));
        return e ? e : (INT64)h;
    }
    case K_SYS_SENDTO: {
        if (f->args[2] > 4096)
            return K_E2BIG;
        UINT8 b[4096];
        k_net_address a;
        int e = k_copy_from_user(p->as, b, f->args[1], f->args[2]);
        if (!e && f->args[3])
            e = k_copy_from_user(p->as, &a, f->args[3], sizeof(a));
        return e ? e
                 : k_net_send((UINT32)f->args[0], b, (UINT32)f->args[2], f->args[3] ? &a : NULL);
    }
    case K_SYS_RECVFROM: {
        if (f->args[2] > 4096)
            return K_E2BIG;
        UINT8 b[4096];
        k_net_address a = {0};
        int e = k_as_check(p->as, f->args[1], f->args[2], TRUE);
        if (!e && f->args[3])
            e = k_as_check(p->as, f->args[3], sizeof(a), TRUE);
        if (e)
            return e;
        int n = k_net_receive((UINT32)f->args[0], b, (UINT32)f->args[2], &a);
        if (n < 0)
            return n;
        if (n)
            e = k_copy_to_user(p->as, f->args[1], b, (UINT32)n);
        if (!e && f->args[3])
            e = k_copy_to_user(p->as, f->args[3], &a, sizeof(a));
        return e ? e : n;
    }
    case K_SYS_NETCLOSE:
        return k_net_close((UINT32)f->args[0]);
    case K_SYS_SOCKINFO: {
        k_socket_info i;
        int e = k_as_check(p->as, f->args[1], sizeof(i), TRUE);
        if (!e)
            e = k_net_socket_get((UINT32)f->args[0], &i);
        return e ? e : k_copy_to_user(p->as, f->args[1], &i, sizeof(i));
    }
    case K_SYS_RAWSEND: {
        if (f->args[2] > 1514)
            return K_E2BIG;
        UINT8 b[1514];
        int e = k_copy_from_user(p->as, b, f->args[1], f->args[2]);
        return e ? e : k_net_raw_send((UINT32)f->args[0], b, (UINT32)f->args[2]);
    }
    case K_SYS_RAWRECV: {
        k_net_frame frame;
        int e = f->args[1] ? k_as_check(p->as, f->args[1], sizeof(frame), TRUE) : 0;
        if (!e)
            e = k_net_raw_receive((UINT32)f->args[0], f->args[1] ? &frame : NULL);
        return e ? e : f->args[1] ? k_copy_to_user(p->as, f->args[1], &frame, sizeof(frame)) : 0;
    }
    case K_SYS_NETPING: {
        k_net_address a;
        UINT32 ms;
        int e = k_copy_from_user(p->as, &a, f->args[0], sizeof(a));
        if (f->args[1] > 10000)
            return K_EINVAL;
        if (!e)
            e = k_net_ping(&a, (UINT32)f->args[1], &ms);
        return e ? e : (INT64)ms;
    }
    case K_SYS_WIFIINFO: {
        k_wifi_info i;
        int e = k_as_check(p->as, f->args[1], sizeof(i), TRUE);
        if (!e)
            e = k_wifi_get((UINT32)f->args[0], &i);
        return e ? e : k_copy_to_user(p->as, f->args[1], &i, sizeof(i));
    }
    case K_SYS_WIFICTRL: {
        k_wifi_request r;
        int e = k_copy_from_user(p->as, &r, f->args[1], sizeof(r));
        return e ? e : k_wifi_control((UINT32)f->args[0], &r);
    }
    case K_SYS_WIFISCAN: {
        if (f->args[1] >= K_WIFI_BSS_MAX)
            return K_EINVAL;
        k_wifi_bss b;
        int e = k_as_check(p->as, f->args[2], sizeof(b), TRUE);
        if (!e)
            e = k_wifi_scan_result((UINT32)f->args[0], (UINT32)f->args[1], &b);
        return e ? e : k_copy_to_user(p->as, f->args[2], &b, sizeof(b));
    }
    default:
        return K_ENOTSUP;
    }
}
