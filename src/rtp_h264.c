#include "rtp_h264.h"

#include <string.h>

#define RTP_MAX_PAYLOAD 1400

static void write_header(uint8_t *d, int marker, uint8_t pt, uint16_t seq,
                         uint32_t timestamp, uint32_t ssrc)
{
    d[0] = 0x80;
    d[1] = (uint8_t)((marker ? 0x80 : 0) | (pt & 0x7f));
    d[2] = (uint8_t)(seq >> 8);
    d[3] = (uint8_t)seq;
    d[4] = (uint8_t)(timestamp >> 24);
    d[5] = (uint8_t)(timestamp >> 16);
    d[6] = (uint8_t)(timestamp >> 8);
    d[7] = (uint8_t)timestamp;
    d[8] = (uint8_t)(ssrc >> 24);
    d[9] = (uint8_t)(ssrc >> 16);
    d[10] = (uint8_t)(ssrc >> 8);
    d[11] = (uint8_t)ssrc;
}

static void emit_one(struct rtp_state *st, const uint8_t *payload, size_t plen,
                     uint32_t timestamp, int marker, rtp_emit_fn emit, void *user)
{
    uint8_t pkt[12 + RTP_MAX_PAYLOAD];

    if (plen > RTP_MAX_PAYLOAD)
        plen = RTP_MAX_PAYLOAD;
    write_header(pkt, marker, st->pt, st->seq, timestamp, st->ssrc);
    memcpy(pkt + 12, payload, plen);
    st->seq++;
    emit(user, pkt, 12 + plen, marker);
}

void rtp_h264_pack(struct rtp_state *st, const uint8_t *nal, size_t len,
                   uint32_t timestamp, int marker, rtp_emit_fn emit, void *user)
{
    if (!st || !nal || len == 0 || !emit)
        return;
    if (st->pt == 0)
        st->pt = 96;
    if (len <= RTP_MAX_PAYLOAD) {
        emit_one(st, nal, len, timestamp, marker, emit, user);
        return;
    }

    {
        uint8_t nal0 = nal[0];
        uint8_t fu_ind = (uint8_t)((nal0 & 0xe0) | 28);
        uint8_t ntype = (uint8_t)(nal0 & 0x1f);
        const uint8_t *p = nal + 1;
        size_t left = len - 1;
        int start = 1;
        const size_t chunk_max = RTP_MAX_PAYLOAD - 2;

        while (left > 0) {
            size_t chunk = left > chunk_max ? chunk_max : left;
            int end = chunk == left;
            uint8_t fu[RTP_MAX_PAYLOAD];
            int pkt_marker = end && marker;
            fu[0] = fu_ind;
            fu[1] = (uint8_t)((start ? 0x80 : 0) | (end ? 0x40 : 0) | ntype);
            memcpy(fu + 2, p, chunk);
            emit_one(st, fu, chunk + 2, timestamp, pkt_marker, emit, user);
            p += chunk;
            left -= chunk;
            start = 0;
        }
    }
}
