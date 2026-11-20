#include "session.h"

#include "io.h"
#include "log.h"
#include "port_pool.h"
#include "rtp_h264.h"
#include "rtsp_msg.h"
#include "sdp.h"
#include "stream.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum {
    ST_INIT = 0,
    ST_READY,
    ST_PLAYING
};

struct frame_slot {
    uint8_t *data;
    size_t len;
    uint32_t pts;
    int marker;
};

struct frame_queue {
    struct frame_slot slots[RTS_QDEPTH];
    int head;
    int count;
    pthread_mutex_t mu;
};

struct mcast_tx;

struct transport_ops {
    const char *name;
    int (*send_rtp)(struct rtsp_session *s, const uint8_t *pkt, size_t len);
};

struct rtsp_session {
    struct rtsps_server *server;
    int fd;
    pthread_t thread;
    pthread_mutex_t mu;
    int thread_on;
    volatile int stop;
    volatile int exited;
    int in_drain;
    int state;
    int paused;
    uint32_t session_id;
    struct media_stream *stream;
    uint32_t stream_gen;
    int subscribed;
    int transport;
    const struct transport_ops *ops;
    int rtp_ch;
    int rtcp_ch;
    int udp_rtp;
    int udp_rtcp;
    unsigned short server_rtp;
    int client_rtp;
    struct sockaddr_in udp_peer;
    struct rtp_state rtp;
    int mcast_passive;
    struct mcast_tx *mtx;
    struct frame_queue q;
    uint8_t *split;
    size_t split_len;
    size_t split_cap;
};

static pthread_mutex_t g_sid_mu = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_sid = 1;

static uint32_t next_sid(void)
{
    uint32_t id;
    pthread_mutex_lock(&g_sid_mu);
    id = g_sid++;
    if (id == 0)
        id = g_sid++;
    pthread_mutex_unlock(&g_sid_mu);
    return id;
}

static void fq_init(struct frame_queue *q)
{
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->mu, NULL);
}

static void fq_push(struct frame_queue *q, const struct frame *fr)
{
    uint8_t *copy;
    int idx;
    if (!fr || !fr->data || fr->len == 0 || fr->len > RTS_MAX_NAL)
        return;
    copy = (uint8_t *)malloc(fr->len);
    if (!copy)
        return;
    memcpy(copy, fr->data, fr->len);
    pthread_mutex_lock(&q->mu);
    if (q->count == RTS_QDEPTH) {
        free(q->slots[q->head].data);
        q->head = (q->head + 1) % RTS_QDEPTH;
        q->count--;
    }
    idx = (q->head + q->count) % RTS_QDEPTH;
    q->slots[idx].data = copy;
    q->slots[idx].len = fr->len;
    q->slots[idx].pts = fr->pts90k;
    q->slots[idx].marker = fr->marker;
    q->count++;
    pthread_mutex_unlock(&q->mu);
}

static int fq_pop(struct frame_queue *q, struct frame_slot *out)
{
    pthread_mutex_lock(&q->mu);
    if (q->count == 0) {
        pthread_mutex_unlock(&q->mu);
        return 0;
    }
    *out = q->slots[q->head];
    q->slots[q->head].data = NULL;
    q->head = (q->head + 1) % RTS_QDEPTH;
    q->count--;
    pthread_mutex_unlock(&q->mu);
    return 1;
}

static void fq_flush(struct frame_queue *q)
{
    struct frame_slot fr;
    while (fq_pop(q, &fr))
        free(fr.data);
}

static void fq_destroy(struct frame_queue *q)
{
    fq_flush(q);
    pthread_mutex_destroy(&q->mu);
}

static void session_on_frame(void *user, const struct frame *fr)
{
    struct rtsp_session *s = (struct rtsp_session *)user;
    if (s->stop)
        return;
    fq_push(&s->q, fr);
}

static int session_drain(void *user);

static int tcp_send_rtp(struct rtsp_session *s, const uint8_t *pkt, size_t len)
{
    uint8_t hdr[4];
    hdr[0] = 0x24;
    hdr[1] = (uint8_t)s->rtp_ch;
    hdr[2] = (uint8_t)(len >> 8);
    hdr[3] = (uint8_t)len;
    if (io_write_all(s->fd, hdr, 4, session_drain, s) != 0)
        return -1;
    return io_write_all(s->fd, pkt, len, session_drain, s);
}

static int udp_send_rtp(struct rtsp_session *s, const uint8_t *pkt, size_t len)
{
    ssize_t n;
    if (s->udp_rtp < 0)
        return -1;
    n = sendto(s->udp_rtp, pkt, len, MSG_NOSIGNAL,
               (struct sockaddr *)&s->udp_peer, sizeof(s->udp_peer));
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        struct pollfd pfd;
        pfd.fd = s->udp_rtp;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        if (poll(&pfd, 1, 200) <= 0)
            return -1;
        n = sendto(s->udp_rtp, pkt, len, MSG_NOSIGNAL,
                   (struct sockaddr *)&s->udp_peer, sizeof(s->udp_peer));
    }
    return n == (ssize_t)len ? 0 : -1;
}

static const struct transport_ops g_tcp_ops = { "tcp", tcp_send_rtp };
static const struct transport_ops g_udp_ops = { "udp", udp_send_rtp };

#define MCAST_ADDR "239.1.1.1"
#define MCAST_TTL 16

struct mcast_tx {
    int used;
    struct media_stream *stream;
    int rtp_fd;
    int rtcp_fd;
    unsigned short port;
    struct sockaddr_in rtp_dst;
    struct sockaddr_in rtcp_dst;
    int refs;
    struct rtsp_session *owner;
    uint32_t pkt_count;
    uint32_t octet_count;
    uint32_t last_ts;
    uint64_t first_rtp_ms;
    uint64_t last_sr_ms;
};

static struct mcast_tx g_mcast[RTS_MAX_STREAMS];
static pthread_mutex_t g_mcast_mu = PTHREAD_MUTEX_INITIALIZER;

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static int open_mcast_sock(void)
{
    int fd;
    int ttl = MCAST_TTL;
    int loop = 1;
    int on = 1;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    io_set_nonblock(fd);
    return fd;
}

static void mcast_fill_dst(struct mcast_tx *tx)
{
    memset(&tx->rtp_dst, 0, sizeof(tx->rtp_dst));
    memset(&tx->rtcp_dst, 0, sizeof(tx->rtcp_dst));
    tx->rtp_dst.sin_family = AF_INET;
    tx->rtcp_dst.sin_family = AF_INET;
    inet_pton(AF_INET, MCAST_ADDR, &tx->rtp_dst.sin_addr);
    tx->rtcp_dst.sin_addr = tx->rtp_dst.sin_addr;
    tx->rtp_dst.sin_port = htons(tx->port);
    tx->rtcp_dst.sin_port = htons((unsigned short)(tx->port + 1));
}

static void mcast_free_unlocked(struct mcast_tx *tx)
{
    if (tx->rtp_fd >= 0)
        close(tx->rtp_fd);
    if (tx->rtcp_fd >= 0)
        close(tx->rtcp_fd);
    if (tx->port)
        port_pool_free(tx->port);
    memset(tx, 0, sizeof(*tx));
    tx->rtp_fd = -1;
    tx->rtcp_fd = -1;
}

static struct mcast_tx *mcast_attach(struct media_stream *st, struct rtsp_session *s, int *is_owner)
{
    struct mcast_tx *tx = NULL;
    int i;

    *is_owner = 0;
    pthread_mutex_lock(&g_mcast_mu);
    for (i = 0; i < RTS_MAX_STREAMS; i++) {
        if (g_mcast[i].used && g_mcast[i].stream == st) {
            tx = &g_mcast[i];
            break;
        }
    }
    if (!tx) {
        for (i = 0; i < RTS_MAX_STREAMS; i++) {
            if (!g_mcast[i].used) {
                tx = &g_mcast[i];
                break;
            }
        }
        if (!tx) {
            pthread_mutex_unlock(&g_mcast_mu);
            return NULL;
        }
        memset(tx, 0, sizeof(*tx));
        tx->rtp_fd = -1;
        tx->rtcp_fd = -1;
        if (port_pool_alloc(&tx->port) != 0) {
            pthread_mutex_unlock(&g_mcast_mu);
            return NULL;
        }
        tx->rtp_fd = open_mcast_sock();
        tx->rtcp_fd = open_mcast_sock();
        if (tx->rtp_fd < 0 || tx->rtcp_fd < 0) {
            mcast_free_unlocked(tx);
            pthread_mutex_unlock(&g_mcast_mu);
            return NULL;
        }
        mcast_fill_dst(tx);
        tx->stream = st;
        tx->used = 1;
    }
    tx->refs++;
    if (!tx->owner) {
        tx->owner = s;
        *is_owner = 1;
    }
    pthread_mutex_unlock(&g_mcast_mu);
    return tx;
}

static void mcast_detach(struct rtsp_session *s)
{
    struct mcast_tx *tx = s->mtx;
    if (!tx)
        return;
    pthread_mutex_lock(&g_mcast_mu);
    if (tx->owner == s)
        tx->owner = NULL;
    tx->refs--;
    if (tx->refs <= 0)
        mcast_free_unlocked(tx);
    pthread_mutex_unlock(&g_mcast_mu);
    s->mtx = NULL;
    s->mcast_passive = 0;
}

static int mcast_send_rtp(struct rtsp_session *s, const uint8_t *pkt, size_t len)
{
    struct mcast_tx *tx = s->mtx;
    ssize_t n;
    if (!tx || s->mcast_passive || tx->rtp_fd < 0)
        return s->mcast_passive ? 0 : -1;
    n = sendto(tx->rtp_fd, pkt, len, MSG_NOSIGNAL,
               (struct sockaddr *)&tx->rtp_dst, sizeof(tx->rtp_dst));
    if (n != (ssize_t)len)
        return -1;
    tx->pkt_count++;
    if (len > 12)
        tx->octet_count += (uint32_t)(len - 12);
    if (len >= 12) {
        tx->last_ts = ((uint32_t)pkt[4] << 24) | ((uint32_t)pkt[5] << 16) |
                      ((uint32_t)pkt[6] << 8) | (uint32_t)pkt[7];
    }
    if (tx->first_rtp_ms == 0)
        tx->first_rtp_ms = mono_ms();
    return 0;
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void mcast_send_sr(struct rtsp_session *s)
{
    struct mcast_tx *tx = s->mtx;
    uint8_t sr[28];
    struct timespec ts;
    uint32_t ntp_sec;
    uint32_t ntp_frac;
    ssize_t n;

    if (!tx || tx->rtcp_fd < 0 || tx->pkt_count == 0)
        return;
    clock_gettime(CLOCK_REALTIME, &ts);
    ntp_sec = (uint32_t)ts.tv_sec + 2208988800u;
    ntp_frac = (uint32_t)(((uint64_t)ts.tv_nsec << 32) / 1000000000ull);
    memset(sr, 0, sizeof(sr));
    sr[0] = 0x80;
    sr[1] = 200;
    sr[2] = 0;
    sr[3] = 6;
    put_be32(sr + 4, s->rtp.ssrc);
    put_be32(sr + 8, ntp_sec);
    put_be32(sr + 12, ntp_frac);
    put_be32(sr + 16, tx->last_ts);
    put_be32(sr + 20, tx->pkt_count);
    put_be32(sr + 24, tx->octet_count);
    n = sendto(tx->rtcp_fd, sr, sizeof(sr), MSG_NOSIGNAL,
               (struct sockaddr *)&tx->rtcp_dst, sizeof(tx->rtcp_dst));
    if (n == (ssize_t)sizeof(sr))
        tx->last_sr_ms = mono_ms();
}

static void mcast_maybe_sr(struct rtsp_session *s)
{
    struct mcast_tx *tx = s->mtx;
    uint64_t now;
    if (!tx || s->mcast_passive || tx->pkt_count == 0)
        return;
    now = mono_ms();
    if (tx->last_sr_ms == 0) {
        if (tx->first_rtp_ms == 0 || now - tx->first_rtp_ms < 5000)
            return;
    } else if (now - tx->last_sr_ms < 5000) {
        return;
    }
    mcast_send_sr(s);
}

static const struct transport_ops g_mcast_ops = { "mcast", mcast_send_rtp };

struct send_ctx {
    struct rtsp_session *s;
    int err;
};

static void emit_rtp(void *user, const uint8_t *pkt, size_t len, int marker)
{
    struct send_ctx *ctx = (struct send_ctx *)user;
    (void)marker;
    if (ctx->err || ctx->s->stop)
        return;
    if (!ctx->s->ops || ctx->s->ops->send_rtp(ctx->s, pkt, len) != 0)
        ctx->err = -1;
}

static int session_send_frame(struct rtsp_session *s, struct frame_slot *fr)
{
    struct send_ctx ctx;
    ctx.s = s;
    ctx.err = 0;
    rtp_h264_pack(&s->rtp, fr->data, fr->len, fr->pts, fr->marker, emit_rtp, &ctx);
    free(fr->data);
    fr->data = NULL;
    return ctx.err;
}

static void session_close_media(struct rtsp_session *s)
{
    if (s->subscribed && s->stream) {
        stream_unsubscribe(s->stream, s);
        s->subscribed = 0;
    }
    mcast_detach(s);
    if (s->udp_rtp >= 0) {
        close(s->udp_rtp);
        s->udp_rtp = -1;
    }
    if (s->udp_rtcp >= 0) {
        close(s->udp_rtcp);
        s->udp_rtcp = -1;
    }
    if (s->server_rtp) {
        port_pool_free(s->server_rtp);
        s->server_rtp = 0;
    }
    s->ops = NULL;
    s->transport = 0;
}

static int session_match(struct rtsp_session *s, const struct rtsp_request *req, int required)
{
    unsigned long v;
    if (s->session_id == 0)
        return required ? 0 : 1;
    if (!req->session[0])
        return !required;
    v = strtoul(req->session, NULL, 10);
    return v == s->session_id;
}

static int reply_code(struct rtsp_session *s, const struct rtsp_request *req, int code)
{
    char extra[96];
    extra[0] = '\0';
    if (s->session_id)
        snprintf(extra, sizeof(extra), "Session: %u;timeout=60\r\n", s->session_id);
    if (rtsp_reply(s->fd, code, req->cseq, extra[0] ? extra : NULL, NULL) != 0)
        return -1;
    return 0;
}

static int bind_udp_pair(unsigned short port, int *rtp, int *rtcp)
{
    int a = socket(AF_INET, SOCK_DGRAM, 0);
    int b = socket(AF_INET, SOCK_DGRAM, 0);
    int on = 1;
    struct sockaddr_in addr;
    if (a < 0 || b < 0)
        goto fail;
    setsockopt(a, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    setsockopt(b, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(a, (struct sockaddr *)&addr, sizeof(addr)) != 0)
        goto fail;
    addr.sin_port = htons((unsigned short)(port + 1));
    if (bind(b, (struct sockaddr *)&addr, sizeof(addr)) != 0)
        goto fail;
    io_set_nonblock(a);
    io_set_nonblock(b);
    *rtp = a;
    *rtcp = b;
    return 0;
fail:
    if (a >= 0)
        close(a);
    if (b >= 0)
        close(b);
    return -1;
}

static int setup_udp(struct rtsp_session *s, int c0, int c1)
{
    int tries;
    struct sockaddr_in peer;
    socklen_t sl = sizeof(peer);
    if (getpeername(s->fd, (struct sockaddr *)&peer, &sl) != 0 || peer.sin_family != AF_INET)
        return -1;
    for (tries = 0; tries < 16; tries++) {
        unsigned short port = 0;
        int rtp = -1;
        int rtcp = -1;
        if (port_pool_alloc(&port) != 0)
            return -1;
        if (bind_udp_pair(port, &rtp, &rtcp) == 0) {
            peer.sin_port = htons((unsigned short)c0);
            s->udp_peer = peer;
            s->udp_rtp = rtp;
            s->udp_rtcp = rtcp;
            s->server_rtp = port;
            s->client_rtp = c0;
            (void)c1;
            return 0;
        }
        port_pool_free(port);
    }
    return -1;
}

static int on_options(struct rtsp_session *s, const struct rtsp_request *req)
{
    const char *hdr =
        "Public: OPTIONS, DESCRIBE, SETUP, TEARDOWN, PLAY, PAUSE, GET_PARAMETER, HEARTBEAT\r\n";
    if (rtsp_reply(s->fd, 200, req->cseq, hdr, NULL) != 0)
        return -1;
    return 0;
}

static int on_describe(struct rtsp_session *s, const struct rtsp_request *req)
{
    char name[RTS_NAME];
    struct media_stream *st;
    char base[576];
    char sdp[512];
    char hdr[704];
    uint32_t gen;

    rtsp_stream_name(req->url, name, sizeof(name));
    st = stream_find(s->server, name);
    if (!st)
        return reply_code(s, req, 404);
    gen = stream_gen(st);
    if (sdp_write_h264(sdp, sizeof(sdp)) < 0)
        return reply_code(s, req, 500);
    if (rtsp_content_base(req->url, name, base, sizeof(base)) != 0 &&
        rtsp_content_base_from_fd(s->fd, name, base, sizeof(base)) != 0)
        return reply_code(s, req, 500);
    pthread_mutex_lock(&s->mu);
    s->stream = st;
    s->stream_gen = gen;
    if (s->state == ST_INIT)
        s->state = ST_READY;
    pthread_mutex_unlock(&s->mu);
    snprintf(hdr, sizeof(hdr),
             "Content-Type: application/sdp\r\n"
             "Content-Base: %s\r\n",
             base);
    if (rtsp_reply(s->fd, 200, req->cseq, hdr, sdp) != 0)
        return -1;
    rtsps_log("DESCRIBE %s", name);
    return 0;
}

static int on_setup(struct rtsp_session *s, const struct rtsp_request *req)
{
    char name[RTS_NAME];
    int kind;
    char transport[192];
    char hdr[320];

    pthread_mutex_lock(&s->mu);
    if (s->state == ST_INIT) {
        pthread_mutex_unlock(&s->mu);
        return reply_code(s, req, 455);
    }
    pthread_mutex_unlock(&s->mu);
    if (!session_match(s, req, 0))
        return reply_code(s, req, 454);

    rtsp_stream_name(req->url, name, sizeof(name));
    if (name[0]) {
        struct media_stream *st = stream_find(s->server, name);
        uint32_t gen;
        if (!st)
            return reply_code(s, req, 404);
        gen = stream_gen(st);
        pthread_mutex_lock(&s->mu);
        s->stream = st;
        s->stream_gen = gen;
        pthread_mutex_unlock(&s->mu);
    }
    pthread_mutex_lock(&s->mu);
    if (!s->stream) {
        pthread_mutex_unlock(&s->mu);
        return reply_code(s, req, 404);
    }
    pthread_mutex_unlock(&s->mu);

    kind = rtsp_transport_kind(req->transport);
    if (kind < 0)
        return reply_code(s, req, 461);

    session_close_media(s);
    if (s->session_id == 0)
        s->session_id = next_sid();

    if (kind == RTSP_TP_MCAST) {
        int is_owner = 0;
        struct mcast_tx *tx = mcast_attach(s->stream, s, &is_owner);
        if (!tx)
            return reply_code(s, req, 500);
        s->mtx = tx;
        s->mcast_passive = is_owner ? 0 : 1;
        s->transport = RTSP_TP_MCAST;
        s->ops = &g_mcast_ops;
        if (rtsp_format_mcast_transport(transport, sizeof(transport),
                                        MCAST_ADDR, tx->port, MCAST_TTL) != 0)
            return reply_code(s, req, 500);
    } else if (kind == RTSP_TP_TCP) {
        int a = 0;
        int b = 0;
        if (rtsp_parse_port_pair(req->transport, "interleaved", &a, &b) != 0 ||
            a < 0 || a > 254 || b < 0 || b > 255)
            return reply_code(s, req, 400);
        s->rtp_ch = a;
        s->rtcp_ch = b;
        s->transport = RTSP_TP_TCP;
        s->ops = &g_tcp_ops;
        if (rtsp_format_tcp_transport(transport, sizeof(transport), a, b) != 0)
            return reply_code(s, req, 500);
    } else {
        int c0 = 0;
        int c1 = 0;
        if (rtsp_parse_port_pair(req->transport, "client_port", &c0, &c1) != 0 ||
            c0 <= 0 || c0 > 65535)
            return reply_code(s, req, 400);
        if (c1 <= 0)
            c1 = c0 + 1;
        if (setup_udp(s, c0, c1) != 0)
            return reply_code(s, req, 500);
        s->transport = RTSP_TP_UDP;
        s->ops = &g_udp_ops;
        if (rtsp_format_udp_transport(transport, sizeof(transport), c0, c1,
                                      s->server_rtp, s->server_rtp + 1) != 0)
            return reply_code(s, req, 500);
    }

    snprintf(hdr, sizeof(hdr), "Session: %u;timeout=60\r\nTransport: %s\r\n",
             s->session_id, transport);
    if (rtsp_reply(s->fd, 200, req->cseq, hdr, NULL) != 0)
        return -1;
    rtsps_log("SETUP session %u %s", s->session_id, s->ops->name);
    return 0;
}

static int on_play(struct rtsp_session *s, const struct rtsp_request *req)
{
    struct media_stream *st;
    uint32_t gen;
    char hdr[160];
    int rc;

    if (!session_match(s, req, 1))
        return reply_code(s, req, 454);
    pthread_mutex_lock(&s->mu);
    if ((s->state != ST_READY && s->state != ST_PLAYING) || !s->ops || !s->stream) {
        pthread_mutex_unlock(&s->mu);
        return reply_code(s, req, 455);
    }
    st = s->stream;
    gen = s->stream_gen;
    pthread_mutex_unlock(&s->mu);

    if (s->mcast_passive) {
        rc = 0;
    } else if (!s->subscribed) {
        rc = stream_subscribe(st, gen, session_on_frame, s);
        pthread_mutex_lock(&s->mu);
        if (rc == 0 && s->stream == st && s->stream_gen == gen)
            s->subscribed = 1;
        else
            rc = -1;
        pthread_mutex_unlock(&s->mu);
        if (rc != 0) {
            stream_unsubscribe(st, s);
            return reply_code(s, req, 500);
        }
    }
    pthread_mutex_lock(&s->mu);
    s->paused = 0;
    s->state = ST_PLAYING;
    pthread_mutex_unlock(&s->mu);
    snprintf(hdr, sizeof(hdr),
             "Session: %u;timeout=60\r\n"
             "Range: npt=0.000-\r\n",
             s->session_id);
    if (rtsp_reply(s->fd, 200, req->cseq, hdr, NULL) != 0)
        return -1;
    rtsps_log("PLAY session %u", s->session_id);
    return 0;
}

static int on_pause(struct rtsp_session *s, const struct rtsp_request *req)
{
    if (!session_match(s, req, 1))
        return reply_code(s, req, 454);
    pthread_mutex_lock(&s->mu);
    if (s->state != ST_PLAYING) {
        pthread_mutex_unlock(&s->mu);
        return reply_code(s, req, 455);
    }
    s->paused = 1;
    pthread_mutex_unlock(&s->mu);
    return reply_code(s, req, 200);
}

static int on_teardown(struct rtsp_session *s, const struct rtsp_request *req)
{
    reply_code(s, req, 200);
    rtsps_log("TEARDOWN session %u", s->session_id);
    return 1;
}

static int on_heartbeat(struct rtsp_session *s, const struct rtsp_request *req)
{
    char hdr[160];
    if (s->session_id)
        snprintf(hdr, sizeof(hdr),
                 "Session: %u;timeout=60\r\n"
                 "Function: Heartbeat\r\n",
                 s->session_id);
    else
        snprintf(hdr, sizeof(hdr), "Function: Heartbeat\r\n");
    if (rtsp_reply(s->fd, 200, req->cseq, hdr, NULL) != 0)
        return -1;
    return 0;
}

static int on_get_parameter(struct rtsp_session *s, const struct rtsp_request *req)
{
    return reply_code(s, req, 200);
}

struct method_slot {
    const char *name;
    int (*fn)(struct rtsp_session *s, const struct rtsp_request *req);
};

static const struct method_slot g_methods[] = {
    { "OPTIONS", on_options },
    { "DESCRIBE", on_describe },
    { "SETUP", on_setup },
    { "PLAY", on_play },
    { "PAUSE", on_pause },
    { "TEARDOWN", on_teardown },
    { "GET_PARAMETER", on_get_parameter },
    { "HEARTBEAT", on_heartbeat },
};

static int dispatch(struct rtsp_session *s, const struct rtsp_request *req)
{
    size_t i;
    rtsps_log("%s cseq %d", req->method, req->cseq);
    for (i = 0; i < sizeof(g_methods) / sizeof(g_methods[0]); i++) {
        if (strcasecmp(req->method, g_methods[i].name) == 0)
            return g_methods[i].fn(s, req);
    }
    if (rtsp_reply(s->fd, 501, req->cseq, NULL, NULL) != 0)
        return -1;
    return 0;
}

static int session_on_bytes(struct rtsp_session *s, const uint8_t *data, size_t n)
{
    if (s->split_len + n > s->split_cap) {
        size_t cap = s->split_cap ? s->split_cap : 8192;
        uint8_t *grown;
        while (cap < s->split_len + n)
            cap *= 2;
        if (cap > 1024 * 1024)
            return -1;
        grown = (uint8_t *)realloc(s->split, cap);
        if (!grown)
            return -1;
        s->split = grown;
        s->split_cap = cap;
    }
    memcpy(s->split + s->split_len, data, n);
    s->split_len += n;

    while (s->split_len > 0 && !s->stop) {
        if (s->split[0] == '$') {
            size_t plen;
            if (s->split_len < 4)
                return 0;
            plen = ((size_t)s->split[2] << 8) | s->split[3];
            if (plen > 65535)
                return -1;
            if (s->split_len < 4 + plen)
                return 0;
            memmove(s->split, s->split + 4 + plen, s->split_len - 4 - plen);
            s->split_len -= 4 + plen;
            continue;
        }
        {
            struct rtsp_request req;
            size_t used = 0;
            int rc = rtsp_parse_request(s->split, s->split_len, &req, &used);
            int dr;
            if (rc == 0) {
                if (s->split_len > 32768)
                    return -1;
                return 0;
            }
            if (rc < 0)
                return -1;
            dr = dispatch(s, &req);
            if (used > s->split_len)
                return -1;
            memmove(s->split, s->split + used, s->split_len - used);
            s->split_len -= used;
            if (dr != 0)
                return -1;
        }
    }
    return 0;
}

static int session_read(struct rtsp_session *s)
{
    uint8_t buf[4096];
    for (;;) {
        ssize_t n = recv(s->fd, buf, sizeof(buf), 0);
        if (n > 0) {
            if (session_on_bytes(s, buf, (size_t)n) < 0)
                return -1;
            continue;
        }
        if (n == 0)
            return -1;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        return -1;
    }
}

static int session_drain(void *user)
{
    struct rtsp_session *s = (struct rtsp_session *)user;
    if (s->in_drain)
        return 0;
    s->in_drain = 1;
    if (session_read(s) < 0) {
        s->stop = 1;
        s->in_drain = 0;
        return -1;
    }
    s->in_drain = 0;
    return s->stop ? -1 : 0;
}

static void drain_rtcp(struct rtsp_session *s)
{
    uint8_t buf[2048];
    if (s->udp_rtcp < 0)
        return;
    for (;;) {
        ssize_t n = recv(s->udp_rtcp, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (n == 0)
            break;
    }
}

static void *session_main(void *arg)
{
    struct rtsp_session *s = (struct rtsp_session *)arg;
    rtsps_log("session connected");
    while (!s->stop) {
        struct pollfd pf[2];
        int np = 1;
        int pr;
        pf[0].fd = s->fd;
        pf[0].events = POLLIN;
        pf[0].revents = 0;
        if (s->udp_rtcp >= 0) {
            pf[1].fd = s->udp_rtcp;
            pf[1].events = POLLIN;
            pf[1].revents = 0;
            np = 2;
        }
        pr = poll(pf, (nfds_t)np, 15);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pr > 0 && (pf[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            if (session_read(s) < 0)
                break;
        }
        if (np == 2 && (pf[1].revents & POLLIN) != 0)
            drain_rtcp(s);
        if (s->stop)
            break;
        if (s->state == ST_PLAYING && s->paused)
            fq_flush(&s->q);
        if (s->state == ST_PLAYING && !s->paused)
            mcast_maybe_sr(s);
        if (s->state != ST_PLAYING || s->paused || !s->ops)
            continue;
        {
            struct frame_slot fr;
            if (!fq_pop(&s->q, &fr))
                continue;
            if (session_send_frame(s, &fr) != 0)
                break;
        }
    }
    session_close_media(s);
    if (s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }
    fq_flush(&s->q);
    s->exited = 1;
    rtsps_log("session closed");
    return NULL;
}

struct rtsp_session *session_create(struct rtsps_server *sv, int fd)
{
    struct rtsp_session *s;
    int on = 1;
    s = (struct rtsp_session *)calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->server = sv;
    s->fd = fd;
    s->udp_rtp = -1;
    s->udp_rtcp = -1;
    s->rtp.pt = 96;
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        s->rtp.ssrc = (uint32_t)(ts.tv_nsec ^ (uintptr_t)s ^ (uint32_t)getpid()) | 1u;
        s->rtp.seq = (uint16_t)(ts.tv_nsec & 0xffff);
    }
    pthread_mutex_init(&s->mu, NULL);
    fq_init(&s->q);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    io_set_nonblock(fd);
    return s;
}

int session_start(struct rtsp_session *s)
{
    if (!s)
        return -1;
    if (pthread_create(&s->thread, NULL, session_main, s) != 0)
        return -1;
    s->thread_on = 1;
    return 0;
}

void session_request_stop(struct rtsp_session *s)
{
    if (!s)
        return;
    s->stop = 1;
    if (s->fd >= 0)
        shutdown(s->fd, SHUT_RDWR);
}

void session_join(struct rtsp_session *s)
{
    if (!s || !s->thread_on)
        return;
    pthread_join(s->thread, NULL);
    s->thread_on = 0;
}

int session_exited(struct rtsp_session *s)
{
    return s && s->exited;
}

void session_free(struct rtsp_session *s)
{
    if (!s)
        return;
    session_close_media(s);
    if (s->fd >= 0)
        close(s->fd);
    fq_destroy(&s->q);
    free(s->split);
    pthread_mutex_destroy(&s->mu);
    free(s);
}

void session_drop_stream(struct rtsp_session *s, struct media_stream *st)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->mu);
    if (s->stream == st) {
        s->stream = NULL;
        s->subscribed = 0;
        s->stream_gen = 0;
    }
    pthread_mutex_unlock(&s->mu);
}
